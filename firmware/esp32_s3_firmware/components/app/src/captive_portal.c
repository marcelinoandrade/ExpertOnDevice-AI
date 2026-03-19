#include "captive_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "bsp.h"
#include "config_manager.h"
#include "gui.h"

static const char *TAG = "captive_portal";

#define AP_SSID "Assistant-Config-S3"
#define AP_PASS "" /* Aberto — sem senha */
#define AP_CHANNEL 6
#define AP_MAX_CONN 4
#define AP_IP "192.168.4.1"

/* -----------------------------------------------------------------------
 * Página HTML embarcada
 * ----------------------------------------------------------------------- */

static const char *s_html_saved =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;"
    "display:flex;flex-direction:column;align-items:center;padding:40px}"
    "h1{color:#4caf50}</style></head><body>"
    "<h1>&#10003; Salvo!</h1>"
    "<p>Reiniciando o dispositivo...</p>"
    "</body></html>";

/* -----------------------------------------------------------------------
 * Servidor DNS simples (UDP porta 53) — redireciona tudo para AP_IP
 * Necessário para o Captive Portal funcionar em Android/iOS
 * ----------------------------------------------------------------------- */
#define DNS_PORT 53
#define DNS_BUF 512

/* Monta uma resposta DNS mínima: A record apontando para AP_IP */
static int dns_build_reply(const uint8_t *req, int req_len, uint8_t *reply,
                           int reply_max) {
  if (req_len < 12 || reply_max < req_len + 16)
    return -1;

  memcpy(reply, req, req_len); /* copia o cabeçalho + pergunta */
  reply[2] = 0x81;             /* QR=1 (response), OPCODE=0, AA=1 */
  reply[3] = 0x80;             /* RA=1 */
  reply[7] = 1;                /* ANCOUNT = 1 */

  int off = req_len;

  /* Ponteiro para a pergunta (C0 0C = offset 12) */
  reply[off++] = 0xC0;
  reply[off++] = 0x0C;

  /* Tipo A, Classe IN */
  reply[off++] = 0x00;
  reply[off++] = 0x01; /* TYPE A */
  reply[off++] = 0x00;
  reply[off++] = 0x01; /* CLASS IN */

  /* TTL = 60 s */
  reply[off++] = 0x00;
  reply[off++] = 0x00;
  reply[off++] = 0x00;
  reply[off++] = 0x3C;

  /* RDLENGTH = 4 */
  reply[off++] = 0x00;
  reply[off++] = 0x04;

  /* RDATA: 192.168.4.1 */
  reply[off++] = 192;
  reply[off++] = 168;
  reply[off++] = 4;
  reply[off++] = 1;

  return off;
}

static void dns_server_task(void *pvParameters) {
  ESP_LOGI(TAG, "DNS server task started (port %d)", DNS_PORT);

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket creation failed: %d", errno);
    vTaskDelete(NULL);
    return;
  }

  /* Timeout para que a task não fique bloqueada para sempre */
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* Reutiliza porta */
  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in srv = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed: %d", errno);
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  uint8_t buf[DNS_BUF];
  uint8_t reply[DNS_BUF + 32];

  while (1) {
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);
    int len =
        recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
    if (len <= 0) {
      continue; /* timeout ou erro — itera */
    }

    int rlen = dns_build_reply(buf, len, reply, sizeof(reply));
    if (rlen > 0) {
      sendto(sock, reply, rlen, 0, (struct sockaddr *)&client, clen);
    }
  }

  close(sock);
  vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------
 * HTML attribute escaping — evita quebra de formulário se o valor
 * contiver caracteres especiais como " < > &
 * Substitui in-place: src → dst, trunca em dst_max-1 chars úteis.
 * ----------------------------------------------------------------------- */
static void html_attr_escape(char *dst, const char *src, size_t dst_max) {
  size_t di = 0;
  while (*src && di < dst_max - 1) {
    const char *ent = NULL;
    switch ((unsigned char)*src) {
      case '&':  ent = "&amp;";  break;
      case '"':  ent = "&quot;"; break;
      case '\'': ent = "&#39;";  break;
      case '<':  ent = "&lt;";   break;
      case '>':  ent = "&gt;";   break;
      default:   break;
    }
    if (ent) {
      size_t el = strlen(ent);
      if (di + el >= dst_max - 1) break; /* sem espaço para entidade completa */
      memcpy(dst + di, ent, el);
      di += el;
    } else {
      dst[di++] = *src;
    }
    src++;
  }
  dst[di] = '\0';
}

/* -----------------------------------------------------------------------
 * URL decode simples (application/x-www-form-urlencoded)
 * ----------------------------------------------------------------------- */
static void url_decode(char *dst, const char *src, size_t dst_max) {
  size_t di = 0;
  while (*src && di < dst_max - 1) {
    if (*src == '%' && src[1] && src[2]) {
      char hex[3] = {src[1], src[2], '\0'};
      dst[di++] = (char)strtol(hex, NULL, 16);
      src += 3;
    } else if (*src == '+') {
      dst[di++] = ' ';
      src++;
    } else {
      dst[di++] = *src++;
    }
  }
  dst[di] = '\0';
}

static bool form_get_field(const char *body, const char *key, char *dst,
                           size_t dst_max) {
  char search[72];
  snprintf(search, sizeof(search), "%s=", key);
  const char *p = strstr(body, search);

  while (p) {
    if (p == body || *(p - 1) == '&') {
      break;
    }
    p = strstr(p + 1, search);
  }

  if (!p)
    return false;
  p += strlen(search);

  /* Buffer de 2048 para suportar prompts de até 511 chars com encoding
   * URL completo (%XX). Pior caso: 511 × 3 = 1533 bytes encoded.
   * Com 1024 o prompt era truncado silenciosamente para ~341 chars. */
  char raw[2048] = {0};
  size_t ri = 0;
  while (*p && *p != '&' && ri < sizeof(raw) - 1) {
    raw[ri++] = *p++;
  }
  raw[ri] = '\0';
  url_decode(dst, raw, dst_max);
  return true;
}

/* -----------------------------------------------------------------------
 * Handlers HTTP
 * ----------------------------------------------------------------------- */
static esp_err_t get_root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=UTF-8");
  const app_config_t *conf = config_manager_get();

  // Part 1: Header and Style
  httpd_resp_sendstr_chunk(
      req,
      "<!DOCTYPE html><html lang='pt-BR'><head>"
      "<meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Assistant-S3 Config</title>"
      "<style>"
      "body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;"
      "flex-direction:column;align-items:center;padding:20px}"
      "h1{color:#e94560;margin-bottom:6px}p{color:#aaa;font-size:13px}"
      "form{background:#16213e;padding:24px;border-radius:12px;width:100%;max-"
      "width:400px}"
      "label{display:block;margin:10px 0 4px;font-size:14px;color:#a0c4ff}"
      "input,textarea{width:100%;box-sizing:border-box;padding:8px;border-"
      "radius:"
      "6px;"
      "border:1px solid #0f3460;background:#0f3460;color:#eee;font-size:14px}"
      "textarea{height:70px;resize:vertical}"
      "button{margin-top:18px;width:100%;padding:12px;background:#e94560;"
      "color:#fff;border:none;border-radius:8px;font-size:16px;cursor:pointer}"
      "button:hover{background:#c73652}"
      ".note{font-size:11px;color:#888;margin-top:6px}"
      "</style></head><body>"
      "<h1>&#9881; Assistant S3</h1>"
      "<p>Configure Wi-Fi e IA, depois salve.</p>"
      "<form method='POST' action='/save'>");

  // Part 2: Wi-Fi and Core AI
  /* Padrão de renderização segura: envia label+prefixo | valor escapado | sufixo
   * como chunks separados — evita snprintf com buffers de tamanho variável. */

  /* --- SSID (esc pequeno: 64*6=384 bytes, pode ir na stack) --- */
  {
    char esc[CONFIG_WIFI_SSID_MAX * 6];
    html_attr_escape(esc, conf->wifi_ssid, sizeof(esc));
    httpd_resp_sendstr_chunk(req, "<label>Wi-Fi SSID</label>"
                                  "<input name='ssid' value='");
    httpd_resp_sendstr_chunk(req, esc);
    httpd_resp_sendstr_chunk(req, "' maxlength='63' required>");
  }
  httpd_resp_sendstr_chunk(req,
      "<label>Senha Wi-Fi</label>"
      "<input name='pass' value='' type='password' maxlength='63'"
      " placeholder='(deixe vazio para manter)'>");

  /* --- Token (esc grande: 220*6=1320 bytes → heap) --- */
  {
    char *esc = malloc(CONFIG_AI_TOKEN_MAX * 6);
    if (!esc) return ESP_ERR_NO_MEM;
    html_attr_escape(esc, conf->ai_token, CONFIG_AI_TOKEN_MAX * 6);
    httpd_resp_sendstr_chunk(req,
        "<label>Token / Chave de API</label>"
        "<input name='token' value='");
    httpd_resp_sendstr_chunk(req, esc);
    httpd_resp_sendstr_chunk(req,
        "' maxlength='219' placeholder='sk-... (vazio para servidor local)'>");
    free(esc);
  }

  /* --- URL Base (esc: 128*6=768 bytes, stack) --- */
  {
    char esc[CONFIG_AI_BASE_URL_MAX * 6];
    html_attr_escape(esc, conf->ai_base_url, sizeof(esc));
    httpd_resp_sendstr_chunk(req,
        "<label>URL Base da IA</label><input name='base_url' value='");
    httpd_resp_sendstr_chunk(req, esc);
    httpd_resp_sendstr_chunk(req, "' maxlength='127'>");
  }

  /* --- Modelo (esc: 64*6=384 bytes, stack) --- */
  {
    char esc[CONFIG_AI_MODEL_MAX * 6];
    html_attr_escape(esc, conf->ai_model, sizeof(esc));
    httpd_resp_sendstr_chunk(req,
        "<label>Modelo da IA</label><input name='model' value='");
    httpd_resp_sendstr_chunk(req, esc);
    httpd_resp_sendstr_chunk(req, "' maxlength='63'>");
  }

  /* --- Personalidade (esc grande: 256*6=1536 bytes → heap) --- */
  {
    char *esc = malloc(CONFIG_AI_PERSONALITY_MAX * 6);
    if (!esc) return ESP_ERR_NO_MEM;
    html_attr_escape(esc, conf->ai_personality, CONFIG_AI_PERSONALITY_MAX * 6);
    httpd_resp_sendstr_chunk(req,
        "<label>Personalidade da IA</label>"
        "<textarea name='personality' maxlength='255'>");
    httpd_resp_sendstr_chunk(req, esc);
    httpd_resp_sendstr_chunk(req, "</textarea>");
    free(esc);
  }

  // Part 3: Profiles — dinâmicos (1 a CONFIG_MAX_PROFILES)
  httpd_resp_sendstr_chunk(req,
      "<hr style='border:1px solid #0f3460;margin:16px 0'>"
      "<p style='color:#e94560;margin:0 0 8px 0'>"
      "<b>Perfis Especialistas (1 a 6)</b></p>"
      "<div style='display:flex;gap:8px;margin-bottom:10px'>"
      "<button type='button' onclick='addP()'"
      " style='flex:1;padding:8px;background:#0f3460;color:#a0c4ff;"
      "border:none;border-radius:6px;font-size:14px;cursor:pointer'>"
      "&#43; Perfil</button>"
      "<button type='button' onclick='rmP()'"
      " style='flex:1;padding:8px;background:#0f3460;color:#e94560;"
      "border:none;border-radius:6px;font-size:14px;cursor:pointer'>"
      "&#8722; Perfil</button>"
      "</div>");

  // Renderiza os 6 slots — esconde os inativos via style inline
  char *pbuf = malloc(1500);
  if (!pbuf) {
    return ESP_ERR_NO_MEM;
  }

  for (int i = 0; i < CONFIG_MAX_PROFILES; i++) {
    const char *disp = (i < conf->num_profiles) ? "" : "display:none;";

    /* Cabeçalho do div do slot */
    snprintf(pbuf, 1500,
        "<div id='d%d' style='%sborder:1px solid #0f3460;border-radius:8px;"
        "padding:10px;margin-bottom:8px'>"
        "<b style='color:#e94560'>Perfil %d</b>",
        i, disp, i + 1);
    httpd_resp_sendstr_chunk(req, pbuf);

    /* --- Campo Nome (esc pequeno: 32*6=192 bytes) --- */
    {
      char esc_name[CONFIG_PROFILE_NAME_MAX * 6];
      html_attr_escape(esc_name,
                       (i < conf->num_profiles) ? conf->profiles[i].name : "",
                       sizeof(esc_name));
      snprintf(pbuf, 1500,
          "<label>Nome</label><input name='n%d' value='%s' maxlength='31'>",
          i, esc_name);
      httpd_resp_sendstr_chunk(req, pbuf);
    }

    /* --- Campo Prompt (esc grande: 512*6=3072 bytes → heap + chunks) --- */
    snprintf(pbuf, 1500,
        "<label>Prompt</label><textarea name='r%d' maxlength='511'>", i);
    httpd_resp_sendstr_chunk(req, pbuf);
    if (i < conf->num_profiles && conf->profiles[i].prompt[0]) {
      char *esc_prompt = malloc(CONFIG_PROFILE_PROMPT_MAX * 6);
      if (esc_prompt) {
        html_attr_escape(esc_prompt, conf->profiles[i].prompt,
                         CONFIG_PROFILE_PROMPT_MAX * 6);
        httpd_resp_sendstr_chunk(req, esc_prompt);
        free(esc_prompt);
      }
    }

    /* --- Campo Termos (esc grande: 256*6=1536 bytes → heap + chunks) --- */
    httpd_resp_sendstr_chunk(req, "</textarea>");
    httpd_resp_sendstr_chunk(req, "<label>Termos</label>");
    httpd_resp_sendstr_chunk(req, "<input name='t");
    /* envia índice e prefixo do value separado para evitar snprintf longo */
    snprintf(pbuf, 16, "%d' value='", i);
    httpd_resp_sendstr_chunk(req, pbuf);
    if (i < conf->num_profiles && conf->profiles[i].terms[0]) {
      char *esc_terms = malloc(CONFIG_PROFILE_TERMS_MAX * 6);
      if (esc_terms) {
        html_attr_escape(esc_terms, conf->profiles[i].terms,
                         CONFIG_PROFILE_TERMS_MAX * 6);
        httpd_resp_sendstr_chunk(req, esc_terms);
        free(esc_terms);
      }
    }
    httpd_resp_sendstr_chunk(req, "' maxlength='255'></div>");
  }
  free(pbuf);

  // Campo oculto com contagem atual + JavaScript de controle
  {
    char np_val[4];
    snprintf(np_val, sizeof(np_val), "%d", conf->num_profiles);
    httpd_resp_sendstr_chunk(req,
        "<input type='hidden' name='np' id='np' value='");
    httpd_resp_sendstr_chunk(req, np_val);
    httpd_resp_sendstr_chunk(req, "'>");
  }
  httpd_resp_sendstr_chunk(req,
      "<script>"
      "var c=parseInt(document.getElementById('np').value)||1;"
      "function addP(){"
        "if(c<6){"
          "document.getElementById('d'+c).style.display='block';"
          "c++;"
          "document.getElementById('np').value=c;"
        "}}"
      "function rmP(){"
        "if(c>1){"
          "c--;"
          "document.getElementById('d'+c).style.display='none';"
          "document.getElementById('np').value=c;"
        "}}"
      "</script>");

  // Part 4: Footer
  httpd_resp_sendstr_chunk(
      req,
      "<button type='submit'>&#128190; Salvar &amp; Reiniciar</button>"
      "<p class='note'>O dispositivo reiniciar&aacute; ap&oacute;s salvar.</p>"
      "</form></body></html>");

  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req) {
  /* Limite aumentado: 6 perfis × ~800 chars + outros campos */
  if (req->content_len == 0 || req->content_len > 10240) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
    return ESP_FAIL;
  }

  char *body = calloc(1, req->content_len + 1);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    return ESP_FAIL;
  }

  /* httpd_req_recv pode retornar menos bytes que content_len numa única
   * chamada.  É necessário iterar até receber tudo — caso contrário os
   * campos de perfil (que ficam no final do body) são perdidos. */
  int total_recv = 0;
  while (total_recv < req->content_len) {
    int ret = httpd_req_recv(req, body + total_recv,
                             req->content_len - total_recv);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        /* Timeout — tenta novamente */
        continue;
      }
      free(body);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv error");
      return ESP_FAIL;
    }
    total_recv += ret;
  }
  body[total_recv] = '\0';

  /* Campos de configuração básica */
  char ssid[64]         = {0};
  char pass[64]         = {0};
  char token[220]       = {0};
  char personality[256] = {0};
  char base_url[128]    = {0};
  char model[64]        = {0};

  form_get_field(body, "ssid",        ssid,        sizeof(ssid));
  form_get_field(body, "pass",        pass,        sizeof(pass));
  form_get_field(body, "token",       token,       sizeof(token));
  form_get_field(body, "personality", personality, sizeof(personality));
  form_get_field(body, "base_url",    base_url,    sizeof(base_url));
  form_get_field(body, "model",       model,       sizeof(model));

  if (ssid[0] == '\0') {
    free(body);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID e obrigatorio");
    return ESP_FAIL;
  }

  /* Número de perfis enviados pelo portal (campo oculto 'np') */
  char np_str[4] = "1";
  form_get_field(body, "np", np_str, sizeof(np_str));
  int num_profiles = atoi(np_str);
  if (num_profiles < 1)                    num_profiles = 1;
  if (num_profiles > CONFIG_MAX_PROFILES)  num_profiles = CONFIG_MAX_PROFILES;

  /* Parse dos perfis: n{i}=nome, r{i}=prompt, t{i}=termos
   * Alocado no HEAP para não explodir a stack da task httpd
   * (6 × 800 bytes = 4800 bytes — inaceitável na stack) */
  app_profile_t *profiles = calloc(CONFIG_MAX_PROFILES, sizeof(app_profile_t));
  if (!profiles) {
    free(body);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM profiles");
    return ESP_FAIL;
  }

  for (int i = 0; i < num_profiles; i++) {
    /* Chaves construídas diretamente — CONFIG_MAX_PROFILES ≤ 6, i é 0-5 */
    char key[3] = {0, (char)('0' + i), '\0'};

    key[0] = 'n';
    form_get_field(body, key, profiles[i].name, sizeof(profiles[i].name));
    key[0] = 'r';
    form_get_field(body, key, profiles[i].prompt, sizeof(profiles[i].prompt));
    key[0] = 't';
    form_get_field(body, key, profiles[i].terms, sizeof(profiles[i].terms));

    /* Garante nome mínimo se o usuário deixou em branco */
    if (profiles[i].name[0] == '\0') {
      snprintf(profiles[i].name, sizeof(profiles[i].name), "Perfil %d", i + 1);
    }
  }

  free(body);

  ESP_LOGI(TAG,
           "POST /save => ssid='%s' token='%.8s...' url='%s' model='%s' "
           "profiles=%d",
           ssid, token, base_url, model, num_profiles);

  /* ---------------------------------------------------------------
   * Atualiza TUDO em memória de uma vez, depois salva UMA única vez.
   * Evita double-save (que pode deixar num_profiles antigo no SD se
   * a segunda escrita falhar silenciosamente).
   * --------------------------------------------------------------- */
  app_config_t *cfg = config_manager_get();

  /* Campos básicos */
  if (ssid[0])        strlcpy(cfg->wifi_ssid,      ssid,        sizeof(cfg->wifi_ssid));
  if (pass[0])        strlcpy(cfg->wifi_pass,       pass,        sizeof(cfg->wifi_pass));
  if (token[0])       strlcpy(cfg->ai_token,        token,       sizeof(cfg->ai_token));
  if (personality[0]) strlcpy(cfg->ai_personality,  personality, sizeof(cfg->ai_personality));
  if (base_url[0])    strlcpy(cfg->ai_base_url,     base_url,    sizeof(cfg->ai_base_url));
  if (model[0])       strlcpy(cfg->ai_model,        model,       sizeof(cfg->ai_model));

  /* Perfis — sobrescreve array inteiro com o que veio do formulário */
  cfg->num_profiles = (uint8_t)num_profiles;
  for (int i = 0; i < num_profiles; i++) {
    strlcpy(cfg->profiles[i].name,   profiles[i].name,
            sizeof(cfg->profiles[i].name));
    strlcpy(cfg->profiles[i].prompt, profiles[i].prompt,
            sizeof(cfg->profiles[i].prompt));
    strlcpy(cfg->profiles[i].terms,  profiles[i].terms,
            sizeof(cfg->profiles[i].terms));
  }
  free(profiles); /* libera heap — já copiado para cfg */

  /* Garante que o perfil ativo não aponte para um índice inexistente */
  if (cfg->expert_profile >= cfg->num_profiles) cfg->expert_profile = 0;
  cfg->loaded = true;

  /* Salva UMA vez com tudo atualizado */
  esp_err_t save_err = config_manager_save();

  if (save_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(save_err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Erro ao salvar no SD card");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "text/html; charset=UTF-8");
  httpd_resp_send(req, s_html_saved, HTTPD_RESP_USE_STRLEN);

  vTaskDelay(pdMS_TO_TICKS(1500));
  ESP_LOGI(TAG, "Configuration saved — restarting device...");
  esp_restart();
  return ESP_OK;
}

/* Captive portal detection (iOS, Android, Windows) */
static esp_err_t captive_detect_handler(httpd_req_t *req) {
  if (strstr(req->uri, "204")) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t catch_all_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * Inicia HTTP Server
 * ----------------------------------------------------------------------- */
static httpd_handle_t start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = 7;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 12288; /* aumentado: save handler chama cJSON + SD write */
  config.max_uri_handlers = 12;

  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
  }

  const httpd_uri_t root_uri = {
      .uri = "/", .method = HTTP_GET, .handler = get_root_handler};
  httpd_register_uri_handler(server, &root_uri);

  const httpd_uri_t save_uri = {
      .uri = "/save", .method = HTTP_POST, .handler = post_save_handler};
  httpd_register_uri_handler(server, &save_uri);

  const httpd_uri_t detect_uri = {.uri = "/generate_204",
                                  .method = HTTP_GET,
                                  .handler = captive_detect_handler};
  httpd_register_uri_handler(server, &detect_uri);

  const httpd_uri_t detect2_uri = {
      .uri = "/gen_204", .method = HTTP_GET, .handler = captive_detect_handler};
  httpd_register_uri_handler(server, &detect2_uri);

  const httpd_uri_t detect3_uri = {.uri = "/hotspot-detect.html",
                                   .method = HTTP_GET,
                                   .handler = captive_detect_handler};
  httpd_register_uri_handler(server, &detect3_uri);

  const httpd_uri_t detect4_uri = {.uri = "/ncsi.txt",
                                   .method = HTTP_GET,
                                   .handler = captive_detect_handler};
  httpd_register_uri_handler(server, &detect4_uri);

  const httpd_uri_t catchall_uri = {
      .uri = "/*", .method = HTTP_GET, .handler = catch_all_handler};
  httpd_register_uri_handler(server, &catchall_uri);

  ESP_LOGI(TAG, "HTTP server started on port 80");
  return server;
}

/* -----------------------------------------------------------------------
 * captive_portal_start
 * ----------------------------------------------------------------------- */
esp_err_t captive_portal_start(void) {
  ESP_LOGI(TAG, "=== Entering Configuration Mode (Captive Portal) ===");
  gui_set_response("Modo Config Web\nIniciando AP...");

  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(500));

  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap_netif) {
    ap_netif = esp_netif_create_default_wifi_ap();
  }

  esp_wifi_set_mode(WIFI_MODE_AP);
  wifi_config_t ap_cfg = {0};
  strlcpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
  ap_cfg.ap.ssid_len = (uint8_t)strlen(AP_SSID);
  ap_cfg.ap.channel = AP_CHANNEL;
  ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
  ap_cfg.ap.max_connection = AP_MAX_CONN;

  esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
  esp_wifi_start();

  vTaskDelay(pdMS_TO_TICKS(1000));
  gui_set_response("== Config Mode ==\nWi-Fi: " AP_SSID
                   "\nAcesse: http://" AP_IP);

  xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, NULL);
  httpd_handle_t server = start_http_server();
  if (!server)
    return ESP_FAIL;

  while (bsp_button_is_pressed())
    vTaskDelay(pdMS_TO_TICKS(100));

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (bsp_button_is_pressed()) {
      gui_set_response("Cancelando...\nReiniciando...");
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
    }
  }
  return ESP_OK;
}
