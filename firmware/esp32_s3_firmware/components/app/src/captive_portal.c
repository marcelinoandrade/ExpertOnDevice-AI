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

  char raw[1024] = {0};
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
  char *buf = malloc(1024);
  if (!buf)
    return ESP_ERR_NO_MEM;

  snprintf(buf, 1024,
           "<label>Wi-Fi SSID</label><input name='ssid' value='%s' "
           "maxlength='63' required>"
           "<label>Senha Wi-Fi</label><input name='pass' value='%s' "
           "type='password' maxlength='63'>"
           "<label>Token OpenAI (sk-...)</label><input name='token' value='%s' "
           "maxlength='219' required>"
           "<label>URL Base da IA</label><input name='base_url' value='%s' "
           "maxlength='127'>"
           "<label>Modelo da IA</label><input name='model' value='%s' "
           "maxlength='63'>",
           conf->wifi_ssid, conf->wifi_pass, conf->ai_token, conf->ai_base_url,
           conf->ai_model);
  httpd_resp_sendstr_chunk(req, buf);

  snprintf(buf, 1024,
           "<label>Personalidade da IA</label><textarea name='personality' "
           "maxlength='255'>%s</textarea>"
           "<hr style='border:1px solid #0f3460;margin:16px 0'>"
           "<p style='color:#e94560;margin:0 0 8px 0'><b>Perfis (Natureza, "
           "Prompt, Termos)</b></p>",
           conf->ai_personality);
  httpd_resp_sendstr_chunk(req, buf);

  // Part 3: Profiles
  // Profile 1 (Geral)
  snprintf(buf, 1024,
           "<label>Perfil 1 - Nome</label><input name='gn' value='%s' "
           "maxlength='31'>"
           "<label>Perfil 1 - Prompt</label><textarea name='gp' "
           "maxlength='511'>%s</textarea>"
           "<label>Perfil 1 - Termos</label><input name='gt' value='%s' "
           "maxlength='255'>",
           conf->profile_general_name, conf->profile_general_prompt,
           conf->profile_general_terms);
  httpd_resp_sendstr_chunk(req, buf);

  // Profile 2 (Agrônomo)
  snprintf(buf, 1024,
           "<label>Perfil 2 - Nome</label><input name='an' value='%s' "
           "maxlength='31'>"
           "<label>Perfil 2 - Prompt</label><textarea name='ap' "
           "maxlength='511'>%s</textarea>"
           "<label>Perfil 2 - Termos</label><input name='at' value='%s' "
           "maxlength='255'>",
           conf->profile_agronomo_name, conf->profile_agronomo_prompt,
           conf->profile_agronomo_terms);
  httpd_resp_sendstr_chunk(req, buf);

  // Profile 3 (Engenheiro)
  snprintf(buf, 1024,
           "<label>Perfil 3 - Nome</label><input name='en' value='%s' "
           "maxlength='31'>"
           "<label>Perfil 3 - Prompt</label><textarea name='ep' "
           "maxlength='511'>%s</textarea>"
           "<label>Perfil 3 - Termos</label><input name='et' value='%s' "
           "maxlength='255'>",
           conf->profile_engenheiro_name, conf->profile_engenheiro_prompt,
           conf->profile_engenheiro_terms);
  httpd_resp_sendstr_chunk(req, buf);

  // Part 4: Footer
  httpd_resp_sendstr_chunk(
      req,
      "<button type='submit'>&#128190; Salvar &amp; Reiniciar</button>"
      "<p class='note'>O dispositivo reiniciar&aacute; ap&oacute;s salvar.</p>"
      "</form></body></html>");

  free(buf);
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len > 4096) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
    return ESP_FAIL;
  }

  char *body = calloc(1, req->content_len + 1);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    return ESP_FAIL;
  }

  int recv_len = httpd_req_recv(req, body, req->content_len);
  if (recv_len <= 0) {
    free(body);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    return ESP_FAIL;
  }
  body[recv_len] = '\0';

  char ssid[64] = {0};
  char pass[64] = {0};
  char token[220] = {0};
  char personality[256] = {0};
  char base_url[128] = {0};
  char model[64] = {0};

  char p_gen_name[32] = {0};
  char p_gen_prompt[512] = {0};
  char p_gen_terms[256] = {0};

  char p_agr_name[32] = {0};
  char p_agr_prompt[512] = {0};
  char p_agr_terms[256] = {0};

  char p_eng_name[32] = {0};
  char p_eng_prompt[512] = {0};
  char p_eng_terms[256] = {0};

  form_get_field(body, "ssid", ssid, sizeof(ssid));
  form_get_field(body, "pass", pass, sizeof(pass));
  form_get_field(body, "token", token, sizeof(token));
  form_get_field(body, "personality", personality, sizeof(personality));
  form_get_field(body, "base_url", base_url, sizeof(base_url));
  form_get_field(body, "model", model, sizeof(model));

  form_get_field(body, "gn", p_gen_name, sizeof(p_gen_name));
  form_get_field(body, "gp", p_gen_prompt, sizeof(p_gen_prompt));
  form_get_field(body, "gt", p_gen_terms, sizeof(p_gen_terms));

  form_get_field(body, "an", p_agr_name, sizeof(p_agr_name));
  form_get_field(body, "ap", p_agr_prompt, sizeof(p_agr_prompt));
  form_get_field(body, "at", p_agr_terms, sizeof(p_agr_terms));

  form_get_field(body, "en", p_eng_name, sizeof(p_eng_name));
  form_get_field(body, "ep", p_eng_prompt, sizeof(p_eng_prompt));
  form_get_field(body, "et", p_eng_terms, sizeof(p_eng_terms));

  free(body);

  if (ssid[0] == '\0' || token[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "SSID e Token sao obrigatorios");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "POST /save => ssid='%s' token='%.8s...' url='%s' model='%s'",
           ssid, token, base_url, model);

  esp_err_t save_err = config_manager_update_and_save(
      ssid, pass, token, strlen(personality) > 0 ? personality : NULL,
      strlen(base_url) > 0 ? base_url : NULL, strlen(model) > 0 ? model : NULL);

  config_manager_update_profiles(strlen(p_gen_name) > 0 ? p_gen_name : NULL,
                                 strlen(p_gen_prompt) > 0 ? p_gen_prompt : NULL,
                                 strlen(p_gen_terms) > 0 ? p_gen_terms : NULL,

                                 strlen(p_agr_name) > 0 ? p_agr_name : NULL,
                                 strlen(p_agr_prompt) > 0 ? p_agr_prompt : NULL,
                                 strlen(p_agr_terms) > 0 ? p_agr_terms : NULL,

                                 strlen(p_eng_name) > 0 ? p_eng_name : NULL,
                                 strlen(p_eng_prompt) > 0 ? p_eng_prompt : NULL,
                                 strlen(p_eng_terms) > 0 ? p_eng_terms : NULL);

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
  config.stack_size = 8192;
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
