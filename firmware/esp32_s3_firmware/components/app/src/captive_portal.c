#include "captive_portal.h"

#include <stdio.h>
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
static const char *s_html_page =
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
    "input,textarea{width:100%;box-sizing:border-box;padding:8px;border-radius:"
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
    "<form method='POST' action='/save'>"
    "<label>Wi-Fi SSID</label><input name='ssid' maxlength='63' required>"
    "<label>Senha Wi-Fi</label><input name='pass' type='password' "
    "maxlength='63'>"
    "<label>Token OpenAI (sk-...)</label>"
    "<input name='token' maxlength='219' required>"
    "<label>URL Base da IA</label>"
    "<input name='base_url' maxlength='127' "
    "placeholder='https://api.openai.com/v1/chat/completions'>"
    "<label>Modelo da IA</label>"
    "<input name='model' maxlength='63' placeholder='gpt-4o'>"
    "<label>Personalidade da IA</label>"
    "<textarea name='personality' maxlength='255'>"
    "Voce e um assistente inteligente e conciso.</textarea>"
    "<button type='submit'>&#128190; Salvar &amp; Reiniciar</button>"
    "<p class='note'>O dispositivo reiniciar&aacute; ap&oacute;s salvar.</p>"
    "</form>"
    "</body></html>";

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
 * Handlers HTTP
 * ----------------------------------------------------------------------- */
static esp_err_t get_root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=UTF-8");
  httpd_resp_send(req, s_html_page, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* URL decode simples (application/x-www-form-urlencoded) */
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
  if (!p)
    return false;
  p += strlen(search);

  char raw[512] = {0};
  size_t ri = 0;
  while (*p && *p != '&' && ri < sizeof(raw) - 1) {
    raw[ri++] = *p++;
  }
  raw[ri] = '\0';
  url_decode(dst, raw, dst_max);
  return true;
}

static esp_err_t post_save_handler(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len > 2048) {
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

  form_get_field(body, "ssid", ssid, sizeof(ssid));
  form_get_field(body, "pass", pass, sizeof(pass));
  form_get_field(body, "token", token, sizeof(token));
  form_get_field(body, "personality", personality, sizeof(personality));
  form_get_field(body, "base_url", base_url, sizeof(base_url));
  form_get_field(body, "model", model, sizeof(model));
  free(body);

  if (strlen(ssid) == 0 || strlen(token) == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "SSID e Token sao obrigatorios");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "POST /save => ssid='%s' token='%.8s...' url='%s' model='%s'",
           ssid, token, base_url, model);

  esp_err_t save_err = config_manager_update_and_save(
      ssid, pass, token, strlen(personality) > 0 ? personality : NULL,
      strlen(base_url) > 0 ? base_url : NULL, strlen(model) > 0 ? model : NULL);

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

/* Responde com 200 OK vazio para URLs de captive portal detection.
 * iOS usa /hotspot-detect.html, Android usa /generate_204 e /gen_204 */
static esp_err_t captive_detect_handler(httpd_req_t *req) {
  /* Para /generate_204 e /gen_204: responder 204 */
  if (strstr(req->uri, "204")) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
  }
  /* Para /hotspot-detect.html e outros: redirecionar ao portal */
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* Catch-all: redireciona qualquer outro path para o portal */
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

  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
  }

  /* GET / — página principal */
  const httpd_uri_t root_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = get_root_handler,
  };
  httpd_register_uri_handler(server, &root_uri);

  /* POST /save */
  const httpd_uri_t save_uri = {
      .uri = "/save",
      .method = HTTP_POST,
      .handler = post_save_handler,
  };
  httpd_register_uri_handler(server, &save_uri);

  /* Captive portal detection (iOS, Android, Windows) */
  const httpd_uri_t detect_uri = {
      .uri = "/generate_204",
      .method = HTTP_GET,
      .handler = captive_detect_handler,
  };
  httpd_register_uri_handler(server, &detect_uri);

  const httpd_uri_t detect2_uri = {
      .uri = "/gen_204",
      .method = HTTP_GET,
      .handler = captive_detect_handler,
  };
  httpd_register_uri_handler(server, &detect2_uri);

  const httpd_uri_t detect3_uri = {
      .uri = "/hotspot-detect.html",
      .method = HTTP_GET,
      .handler = captive_detect_handler,
  };
  httpd_register_uri_handler(server, &detect3_uri);

  const httpd_uri_t detect4_uri = {
      .uri = "/ncsi.txt",
      .method = HTTP_GET,
      .handler = captive_detect_handler,
  };
  httpd_register_uri_handler(server, &detect4_uri);

  /* Catch-all */
  const httpd_uri_t catchall_uri = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = catch_all_handler,
  };
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

  /* ----------------------------------------------------------------
   * 1. Para completamente o Wi-Fi STA antes de entrar em modo AP.
   *    Isso evita que o reconnect handler do BSP interfira.
   * ---------------------------------------------------------------- */
  ESP_LOGI(TAG, "Stopping STA Wi-Fi...");
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(800));

  /* ----------------------------------------------------------------
   * 2. Cria o netif para o AP (DHCP server + IP 192.168.4.1).
   *    SEM isso o P4 não tem rota para 192.168.4.1 e nenhum cliente
   *    recebe IP via DHCP → ERR_ADDRESS_UNREACHABLE.
   * ---------------------------------------------------------------- */
  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap_netif) {
    ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
      ESP_LOGE(TAG, "Failed to create AP netif");
      return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AP netif created (DHCP server will assign 192.168.4.x)");
  } else {
    ESP_LOGI(TAG, "AP netif already exists");
  }

  /* ----------------------------------------------------------------
   * 3. Configura e sobe SoftAP
   * ---------------------------------------------------------------- */
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_mode(AP) failed: %s", esp_err_to_name(err));
    return err;
  }

  wifi_config_t ap_cfg = {0};
  strlcpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
  ap_cfg.ap.ssid_len = (uint8_t)strlen(AP_SSID);
  ap_cfg.ap.channel = AP_CHANNEL;
  ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
  ap_cfg.ap.max_connection = AP_MAX_CONN;
  ap_cfg.ap.beacon_interval = 100;

  err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_config(AP) failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_wifi_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    ESP_LOGE(TAG, "wifi_start(AP) failed: %s", esp_err_to_name(err));
    return err;
  }

  /* Aguarda AP e DHCP estabilizarem */
  vTaskDelay(pdMS_TO_TICKS(1500));
  ESP_LOGI(TAG, "SoftAP '%s' started (open, IP: %s)", AP_SSID, AP_IP);

  /* ----------------------------------------------------------------
   * 4. Atualiza display
   * ---------------------------------------------------------------- */
  gui_set_response("== Config Mode ==\n"
                   "Wi-Fi: " AP_SSID "\n"
                   "Acesse: http://" AP_IP "\n"
                   "(Abra o browser)");

  /* ----------------------------------------------------------------
   * 5. DNS server (porta 53) — redireciona todos os hosts ao portal
   * ---------------------------------------------------------------- */
  xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, NULL);
  ESP_LOGI(TAG, "DNS server task created");

  /* ----------------------------------------------------------------
   * 6. HTTP server (porta 80)
   * ---------------------------------------------------------------- */
  httpd_handle_t server = start_http_server();
  if (!server) {
    return ESP_FAIL;
  }

  /* Aguarda configuração via browser — restart acontece no handler POST */
  ESP_LOGI(TAG,
           "Portal active at http://" AP_IP " — waiting for browser config...");
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGD(TAG, "Portal active, waiting...");
  }

  return ESP_OK;
}
