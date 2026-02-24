#include "app.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/base64.h"

#include "app_state.h"
#include "app_storage.h"
#include "bsp.h"
#include "captive_portal.h"
#include "config_manager.h"
#include "gui.h"
#include "secret.h"

/* Guard against stale header indexing/build cache in IDE; real declaration is
 * in gui.h. */
esp_err_t gui_show_camera_preview_rgb565(const uint8_t *rgb565_data,
                                         uint16_t width, uint16_t height);

#define APP_TASK_STACK_SIZE (10 * 1024)
#define APP_TASK_PRIORITY 5
#define APP_QUEUE_LENGTH 8
#define APP_BUTTON_POLL_MS 40
#define APP_BUTTON_DEBOUNCE_MS 140
#define APP_AUDIO_BUFFER_LEN (352 * 1024)
#define APP_CAPTURE_CHUNK_MS 120
#define APP_MAX_CAPTURE_MS 10000
#define APP_MIN_CAPTURE_BYTES 24000
#define APP_MODE_SELECT_TIMEOUT_MS 4000
#define APP_PREVIEW_REFRESH_MS 220
#define APP_RESPONSE_TEXT_MAX 512
#define APP_RESPONSE_SCROLL_STEP_PX 22
#define APP_RESPONSE_TEXT_MAX 512
#define APP_RESPONSE_SCROLL_STEP_PX 22
#define APP_AI_MODEL_AUDIO_TEXT "gpt-4o-audio-preview"
#define APP_HTTP_TIMEOUT_MS 45000
// AI Modes

typedef enum {
  APP_EVT_BOOT = 0,
  APP_EVT_INTERACTION_REQUESTED,
} app_event_type_t;

typedef enum {
  APP_INTERACTION_MODE_AUDIO_TEXT = 0,
  APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT,
} app_interaction_mode_t;

typedef enum {
  APP_EXPERT_PROFILE_GENERAL = 0,
  APP_EXPERT_PROFILE_AGRONOMO,
  APP_EXPERT_PROFILE_ENGENHEIRO,
} app_expert_profile_t;

typedef struct {
  app_event_type_t type;
} app_event_t;

static const char *TAG = "app";
static QueueHandle_t s_app_queue;
static app_state_t s_state = APP_STATE_BOOTING;
static bool s_interaction_requested;
static bool s_prev_encoder_pressed;
static bool s_prev_photo_button_pressed;
static bool s_prev_btn2_pressed;
static bool s_prev_btn3_pressed;
static bool s_photo_capture_requested;
static bool s_photo_locked;
static TickType_t s_last_preview_ticks;
static TickType_t s_last_encoder_press_ticks;
static TickType_t s_last_photo_press_ticks;
static TickType_t s_last_btn2_press_ticks;
static TickType_t s_last_btn3_press_ticks;
static uint8_t *s_locked_photo_jpeg;
static size_t s_locked_photo_jpeg_len;
static TickType_t s_mode_select_last_activity_ticks;
static app_interaction_mode_t s_interaction_mode =
    APP_INTERACTION_MODE_AUDIO_TEXT;
static bool s_preview_active =
    false; // Track if camera preview is active (consuming DMA)
static app_expert_profile_t s_expert_profile = APP_EXPERT_PROFILE_GENERAL;
static char s_last_response[APP_RESPONSE_TEXT_MAX] =
    "Pronto.\nSegure encoder e fale.";

// Battery ADC
#define BATTERY_ADC_PIN GPIO_NUM_49
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_channel_t s_adc_channel;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_adc_calibrated = false;

/* Multi-turn Chat History in PSRAM */
#define APP_MAX_HISTORY_TURNS 10
#define APP_HISTORY_TIMEOUT_MS (5 * 60 * 1000)

typedef struct {
  char user_prompt[APP_RESPONSE_TEXT_MAX];
  char ai_response[APP_RESPONSE_TEXT_MAX];
} app_history_entry_t;

static app_history_entry_t *s_chat_history = NULL;
static size_t s_chat_history_count = 0;
static TickType_t s_last_interaction_ticks = 0;

static void app_history_add(const char *user, const char *ai) {
  if (!s_chat_history)
    return;

  if (s_chat_history_count == APP_MAX_HISTORY_TURNS) {
    memmove(&s_chat_history[0], &s_chat_history[1],
            (APP_MAX_HISTORY_TURNS - 1) * sizeof(app_history_entry_t));
    s_chat_history_count--;
  }

  strlcpy(s_chat_history[s_chat_history_count].user_prompt,
          (user && user[0]) ? user : "(Audio enviado)", APP_RESPONSE_TEXT_MAX);
  strlcpy(s_chat_history[s_chat_history_count].ai_response,
          (ai && ai[0]) ? ai : "", APP_RESPONSE_TEXT_MAX);
  s_chat_history_count++;
  s_last_interaction_ticks = xTaskGetTickCount();
}

static void app_history_clear(void) {
  s_chat_history_count = 0;
  s_last_interaction_ticks = 0;
  ESP_LOGI(TAG, "History cleared.");
}

/* Long-press config portal: btn2 + btn3 simultaneos por 10 s */
#define APP_CONFIG_PORTAL_LONGPRESS_MS 10000
static TickType_t s_config_longpress_start = 0;
static bool s_config_longpress_active = false;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} app_http_response_t;

static uint8_t *app_pcm16_to_wav(const uint8_t *pcm, size_t pcm_len,
                                 uint32_t sample_rate_hz, uint16_t channels,
                                 uint16_t bits_per_sample, size_t *wav_len) {
  if (!pcm || !wav_len || sample_rate_hz == 0 || channels == 0 ||
      bits_per_sample == 0) {
    return NULL;
  }

  const uint32_t byte_rate = sample_rate_hz * channels * (bits_per_sample / 8);
  const uint16_t block_align = channels * (bits_per_sample / 8);
  const size_t total_len = 44 + pcm_len;
  uint8_t *wav = malloc(total_len);
  if (!wav) {
    return NULL;
  }

  memcpy(wav + 0, "RIFF", 4);
  uint32_t chunk_size = (uint32_t)(36 + pcm_len);
  memcpy(wav + 4, &chunk_size, sizeof(uint32_t));
  memcpy(wav + 8, "WAVE", 4);
  memcpy(wav + 12, "fmt ", 4);
  uint32_t subchunk1_size = 16;
  memcpy(wav + 16, &subchunk1_size, sizeof(uint32_t));
  uint16_t audio_format = 1;
  memcpy(wav + 20, &audio_format, sizeof(uint16_t));
  memcpy(wav + 22, &channels, sizeof(uint16_t));
  memcpy(wav + 24, &sample_rate_hz, sizeof(uint32_t));
  memcpy(wav + 28, &byte_rate, sizeof(uint32_t));
  memcpy(wav + 32, &block_align, sizeof(uint16_t));
  memcpy(wav + 34, &bits_per_sample, sizeof(uint16_t));
  memcpy(wav + 36, "data", 4);
  uint32_t subchunk2_size = (uint32_t)pcm_len;
  memcpy(wav + 40, &subchunk2_size, sizeof(uint32_t));
  memcpy(wav + 44, pcm, pcm_len);

  *wav_len = total_len;
  return wav;
}

static char *app_base64_encode(const uint8_t *data, size_t data_len) {
  size_t out_len = 0;
  int ret = mbedtls_base64_encode(NULL, 0, &out_len, data, data_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && ret != 0) {
    return NULL;
  }

  char *out = malloc(out_len + 1);
  if (!out) {
    return NULL;
  }

  ret = mbedtls_base64_encode((unsigned char *)out, out_len, &out_len, data,
                              data_len);
  if (ret != 0) {
    free(out);
    return NULL;
  }
  out[out_len] = '\0';
  return out;
}

static esp_err_t app_http_append(app_http_response_t *resp, const char *data,
                                 int len) {
  if (!resp || !data || len <= 0) {
    return ESP_OK;
  }

  const size_t required = resp->len + (size_t)len + 1;
  if (required > resp->cap) {
    size_t new_cap = (resp->cap == 0) ? 1024 : resp->cap * 2;
    while (new_cap < required) {
      new_cap *= 2;
    }
    char *new_data = realloc(resp->data, new_cap);
    if (!new_data) {
      return ESP_ERR_NO_MEM;
    }
    resp->data = new_data;
    resp->cap = new_cap;
  }

  memcpy(resp->data + resp->len, data, (size_t)len);
  resp->len += (size_t)len;
  resp->data[resp->len] = '\0';
  return ESP_OK;
}

static void app_utf8_to_ascii(char *text) {
  if (!text) {
    return;
  }

  char *src = text;
  char *dst = text;

  while (*src) {
    unsigned char c = (unsigned char)*src;
    if (c < 0x80) {
      *dst++ = *src++;
      continue;
    }

    unsigned char c2 = (unsigned char)src[1];
    if (c == 0xC3) {
      switch (c2) {
      case 0xA1:
      case 0xA0:
      case 0xA2:
      case 0xA3:
      case 0xA4:
      case 0xA5:
        *dst++ = 'a';
        break; // a
      case 0x81:
      case 0x80:
      case 0x82:
      case 0x83:
      case 0x84:
      case 0x85:
        *dst++ = 'A';
        break; // A
      case 0xA9:
      case 0xA8:
      case 0xAA:
      case 0xAB:
        *dst++ = 'e';
        break; // e
      case 0x89:
      case 0x88:
      case 0x8A:
      case 0x8B:
        *dst++ = 'E';
        break; // E
      case 0xAD:
      case 0xAC:
      case 0xAE:
      case 0xAF:
        *dst++ = 'i';
        break; // i
      case 0x8D:
      case 0x8C:
      case 0x8E:
      case 0x8F:
        *dst++ = 'I';
        break; // I
      case 0xB3:
      case 0xB2:
      case 0xB4:
      case 0xB5:
      case 0xB6:
        *dst++ = 'o';
        break; // o
      case 0x93:
      case 0x92:
      case 0x94:
      case 0x95:
      case 0x96:
        *dst++ = 'O';
        break; // O
      case 0xBA:
      case 0xB9:
      case 0xBB:
      case 0xBC:
        *dst++ = 'u';
        break; // u
      case 0x9A:
      case 0x99:
      case 0x9B:
      case 0x9C:
        *dst++ = 'U';
        break; // U
      case 0xA7:
        *dst++ = 'c';
        break; // c
      case 0x87:
        *dst++ = 'C';
        break; // C
      case 0xB1:
        *dst++ = 'n';
        break; // n
      case 0x91:
        *dst++ = 'N';
        break; // N
      default:
        break;
      }
      src += 2;
      continue;
    }

    // Skip unsupported multibyte sequences to avoid square glyphs.
    if ((c & 0xE0) == 0xC0) {
      src += 2;
    } else if ((c & 0xF0) == 0xE0) {
      src += 3;
    } else if ((c & 0xF8) == 0xF0) {
      src += 4;
    } else {
      src += 1;
    }
  }

  *dst = '\0';
}

static esp_err_t app_extract_response_text(const char *json, char *out_text,
                                           size_t out_text_len) {
  if (!json || !out_text || out_text_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    return ESP_FAIL;
  }

  const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  if (cJSON_IsArray(choices)) {
    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");

    if (cJSON_IsString(content) && content->valuestring) {
      strlcpy(out_text, content->valuestring, out_text_len);
      cJSON_Delete(root);
      return ESP_OK;
    }

    if (cJSON_IsArray(content)) {
      cJSON *part = NULL;
      cJSON_ArrayForEach(part, content) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
        if (cJSON_IsString(text) && text->valuestring) {
          strlcpy(out_text, text->valuestring, out_text_len);
          cJSON_Delete(root);
          return ESP_OK;
        }
      }
    }
  }

  cJSON_Delete(root);
  return ESP_ERR_NOT_FOUND;
}

static const char *app_profile_name(app_expert_profile_t profile) {
  switch (profile) {
  case APP_EXPERT_PROFILE_AGRONOMO:
    return "Agronomo";
  case APP_EXPERT_PROFILE_ENGENHEIRO:
    return "Engenheiro";
  case APP_EXPERT_PROFILE_GENERAL:
  default:
    return "Geral";
  }
}

static const char *app_profile_system_prompt(app_expert_profile_t profile) {
  switch (profile) {
  case APP_EXPERT_PROFILE_AGRONOMO:
    return "Perfil ativo: Agronomo. "
           "Ao ver plantas, identifique especie se possivel, avalie saude (cor "
           "das folhas, manchas, pragas visiveis). "
           "Sugira diagnostico provavel e acao imediata (ex: aplicar "
           "fungicida, irrigar, podar).";
  case APP_EXPERT_PROFILE_ENGENHEIRO:
    return "Perfil ativo: Engenheiro. "
           "Ao ver circuitos/equipamentos, identifique componentes, leia "
           "valores (resistores, capacitores), "
           "note estado de LEDs, conexoes soltas ou queimadas. Sugira ponto de "
           "verificacao.";
  case APP_EXPERT_PROFILE_GENERAL:
  default:
    return "Perfil ativo: Geral. "
           "Responda de forma pratica e direta. "
           "Identifique objetos, leia textos, descreva cenas. Sempre tente ser "
           "util.";
  }
}

static const char *
app_profile_transcription_terms(app_expert_profile_t profile) {
  switch (profile) {
  case APP_EXPERT_PROFILE_AGRONOMO:
    return "NPK, fusarium, clorose, necrose, oidio, ferrugem, praga, "
           "fungicida, herbicida, irrigacao, vazao";
  case APP_EXPERT_PROFILE_ENGENHEIRO:
    return "set-point, vazao, corrente, tensao, curto, rele, inversor, sensor, "
           "atuador, LED vermelho, falha";
  case APP_EXPERT_PROFILE_GENERAL:
  default:
    return "NPK, fusarium, set-point, vazao";
  }
}

static esp_err_t app_build_ai_request_json(
    const char *model, const char *audio_b64, const char *image_data_url,
    const char *system_profile_text, const char *image_context_text,
    const char *audio_context_text, bool inject_history, char **out_json) {
  if (!model || !out_json) {
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *messages = cJSON_CreateArray();
  if (!root || !messages) {
    cJSON_Delete(root);
    cJSON_Delete(messages);
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddStringToObject(root, "model", model);
  cJSON_AddItemToObject(root, "messages", messages);

  cJSON *system_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(system_msg, "role", "system");
  char *system_content = malloc(1024);
  if (!system_content) {
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  const char *personality = config_manager_get()->ai_personality;
  const char *profile_text = (system_profile_text && system_profile_text[0])
                                 ? system_profile_text
                                 : "";
  snprintf(
      system_content, 1024,
      "Voce e um assistente visual e por voz embarcado com camera e microfone. "
      "REGRA PRINCIPAL: responda EXATAMENTE o que o usuario perguntou. "
      "Se ele pergunta 'o que e isso?', identifique o objeto. "
      "Se ele pergunta 'qual a cor?', diga a cor. "
      "Se ele pergunta sobre texto na imagem, leia o texto. "
      "Use a imagem como fonte primaria de informacao. "
      "Formato: portugues-BR, texto plano, sem markdown, sem emojis, ASCII "
      "apenas. "
      "Maximo 60 palavras. Quebre linhas a cada ~30 chars para caber na tela. "
      "Nunca recuse responder. Se nao tiver certeza, diga o que ve e sua "
      "melhor hipotese. "
      "Aja de acordo com esta personalidade customizada: %s\n"
      "%s",
      personality, profile_text);
  cJSON_AddStringToObject(system_msg, "content", system_content);
  free(system_content);
  cJSON_AddItemToArray(messages, system_msg);

  if (inject_history && s_chat_history && s_chat_history_count > 0) {
    for (size_t i = 0; i < s_chat_history_count; i++) {
      cJSON *hist_user = cJSON_CreateObject();
      cJSON_AddStringToObject(hist_user, "role", "user");
      cJSON_AddStringToObject(hist_user, "content",
                              s_chat_history[i].user_prompt);
      cJSON_AddItemToArray(messages, hist_user);

      cJSON *hist_ai = cJSON_CreateObject();
      cJSON_AddStringToObject(hist_ai, "role", "assistant");
      cJSON_AddStringToObject(hist_ai, "content",
                              s_chat_history[i].ai_response);
      cJSON_AddItemToArray(messages, hist_ai);
    }
  }

  cJSON *user_msg = cJSON_CreateObject();
  cJSON *user_content = cJSON_CreateArray();
  cJSON_AddStringToObject(user_msg, "role", "user");
  cJSON_AddItemToObject(user_msg, "content", user_content);

  if (image_data_url) {
    // Vision model path: image + text (audio is handled in a separate first
    // cycle)
    const char *vision_text = (image_context_text && image_context_text[0])
                                  ? image_context_text
                                  : "O que voce ve nesta imagem? Identifique "
                                    "os elementos principais.";
    cJSON *text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", vision_text);
    cJSON_AddItemToArray(user_content, text_part);

    cJSON *image_part = cJSON_CreateObject();
    cJSON *image_obj = cJSON_CreateObject();
    if (!image_part || !image_obj) {
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(image_part, "type", "image_url");
    cJSON_AddStringToObject(image_obj, "url", image_data_url);
    cJSON_AddItemToObject(image_part, "image_url", image_obj);
    cJSON_AddItemToArray(user_content, image_part);
  } else {
    // Audio-only path: keep original input_audio payload for OpenAI extension.
    if (!audio_b64) {
      cJSON_Delete(root);
      return ESP_ERR_INVALID_ARG;
    }

    const char *audio_text =
        (audio_context_text && audio_context_text[0])
            ? audio_context_text
            : "Ouca o audio e responda diretamente a pergunta do usuario.";
    cJSON *ctx_part = cJSON_CreateObject();
    cJSON_AddStringToObject(ctx_part, "type", "text");
    cJSON_AddStringToObject(ctx_part, "text", audio_text);
    cJSON_AddItemToArray(user_content, ctx_part);

    cJSON *audio_part = cJSON_CreateObject();
    cJSON *audio_obj = cJSON_CreateObject();
    if (!audio_part || !audio_obj) {
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(audio_part, "type", "input_audio");
    cJSON_AddStringToObject(audio_obj, "format", "wav");
    cJSON_AddStringToObject(audio_obj, "data", audio_b64);
    cJSON_AddItemToObject(audio_part, "input_audio", audio_obj);
    cJSON_AddItemToArray(user_content, audio_part);
  }

  cJSON_AddItemToArray(messages, user_msg);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    return ESP_ERR_NO_MEM;
  }

  *out_json = json;
  return ESP_OK;
}

/* app_check_dns_ready has been removed because DNS pre-check is skipped
 * for arbitrary dynamic endpoints, the HTTP client will fail gracefully
 * if the host does not exist or network is unavailable. */

static bool app_log_network_status(void) {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) {
    ESP_LOGW(
        TAG,
        "Netif WIFI_STA_DEF nao encontrado (link C6 pode nao estar ativo)");
    return false;
  }

  esp_netif_ip_info_t ip_info = {0};
  esp_err_t ip_err = esp_netif_get_ip_info(sta, &ip_info);
  if (ip_err != ESP_OK) {
    ESP_LOGW(TAG, "Falha ao ler IP da netif WIFI_STA_DEF: %s",
             esp_err_to_name(ip_err));
    return false;
  }

  ESP_LOGI(TAG, "WIFI_STA_DEF ip=" IPSTR " gw=" IPSTR " netmask=" IPSTR,
           IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));

  if (ip4_addr_isany_val(ip_info.ip)) {
    ESP_LOGW(TAG, "WIFI_STA_DEF sem IP (0.0.0.0)");
    return false;
  }

  return true;
}

static esp_err_t app_http_post_json(const char *request_json,
                                    app_http_response_t *resp, int *http_code) {
  if (!request_json || !resp || !http_code) {
    return ESP_ERR_INVALID_ARG;
  }

  *http_code = 0;
  resp->data = NULL;
  resp->len = 0;
  resp->cap = 0;

  esp_http_client_config_t config = {
      .url = config_manager_get()->ai_base_url,
      .timeout_ms = APP_HTTP_TIMEOUT_MS,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    return ESP_FAIL;
  }

  char auth_header[256];
  snprintf(auth_header, sizeof(auth_header), "Bearer %s",
           SECRET_OPENAI_API_KEY);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Authorization", auth_header);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, request_json,
                                 (int)strlen(request_json));

  esp_err_t err = esp_http_client_open(client, strlen(request_json));
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return err;
  }

  int written =
      esp_http_client_write(client, request_json, (int)strlen(request_json));
  if (written < 0) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  (void)esp_http_client_fetch_headers(client);
  char read_buf[512];
  while (1) {
    int read = esp_http_client_read(client, read_buf, sizeof(read_buf));
    if (read < 0) {
      err = ESP_FAIL;
      break;
    }
    if (read == 0) {
      break;
    }
    err = app_http_append(resp, read_buf, read);
    if (err != ESP_OK) {
      break;
    }
  }

  *http_code = esp_http_client_get_status_code(client);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return err;
}

static esp_err_t
app_call_ai_once(const char *model, const char *audio_b64,
                 const char *image_data_url, const char *system_profile_text,
                 const char *image_context_text, const char *audio_context_text,
                 bool inject_history, char *out_text, size_t out_text_len) {
  if (!model || !out_text || out_text_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  char *request_json = NULL;
  esp_err_t build_err = app_build_ai_request_json(
      model, audio_b64, image_data_url, system_profile_text, image_context_text,
      audio_context_text, inject_history, &request_json);
  if (build_err != ESP_OK) {
    return build_err;
  }

  app_http_response_t resp = {0};
  int http_code = 0;
  esp_err_t err = app_http_post_json(request_json, &resp, &http_code);
  free(request_json);

  if (err != ESP_OK) {
    free(resp.data);
    return err;
  }
  if (http_code < 200 || http_code >= 300) {
    ESP_LOGE(TAG, "AI HTTP status=%d body=%s", http_code,
             resp.data ? resp.data : "(empty)");
    free(resp.data);
    return ESP_FAIL;
  }

  err = app_extract_response_text(resp.data, out_text, out_text_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AI parse error (%s), body=%s", esp_err_to_name(err),
             resp.data ? resp.data : "(empty)");
  }
  free(resp.data);
  return err;
}

static esp_err_t app_call_ai_with_audio(const uint8_t *wav_data, size_t wav_len,
                                        const uint8_t *jpeg_data,
                                        size_t jpeg_len, char *out_text,
                                        size_t out_text_len) {
  if (!wav_data || wav_len == 0 || !out_text || out_text_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (strcmp(SECRET_OPENAI_API_KEY, "YOUR_API_KEY_HERE") == 0) {
    strlcpy(out_text,
            "Configure SECRET_OPENAI_API_KEY em secret.h para habilitar IA.",
            out_text_len);
    return ESP_ERR_INVALID_STATE;
  }

  if (!app_log_network_status()) {
    strlcpy(out_text, "Interface C6/hosted indisponivel.", out_text_len);
    return ESP_ERR_INVALID_STATE;
  }

  /* Keep S3-like behavior: we used to do a DNS precheck here when host was
   * fixed. Since host is now totally dynamic (it could even be an IP address
   * like 192.168.1.50 without DNS needed), we skip the extra getaddrinfo hurdle
   * and rely on the HTTP client internal routines to fail gracefully if
   * unreachable.
   */

  char *audio_b64 = app_base64_encode(wav_data, wav_len);
  if (!audio_b64) {
    return ESP_ERR_NO_MEM;
  }

  char *image_data_url = NULL;
  if (jpeg_data && jpeg_len > 0) {
    // Validate JPEG: must start with FF D8 (SOI marker)
    if (jpeg_len < 4) {
      ESP_LOGE(TAG, "JPEG too small: %u bytes", (unsigned)jpeg_len);
      free(audio_b64);
      return ESP_ERR_INVALID_ARG;
    }

    if (jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
      ESP_LOGE(TAG, "Invalid JPEG start: 0x%02X%02X (expected FF D8)",
               jpeg_data[0], jpeg_data[1]);
      free(audio_b64);
      return ESP_ERR_INVALID_ARG;
    }

    // Check end marker (EOI)
    bool has_eoi = (jpeg_len >= 2 && jpeg_data[jpeg_len - 2] == 0xFF &&
                    jpeg_data[jpeg_len - 1] == 0xD9);
    ESP_LOGI(TAG, "JPEG validated: %u bytes, start=FF D8, end=%s",
             (unsigned)jpeg_len, has_eoi ? "FF D9" : "missing");

    char *image_b64 = app_base64_encode(jpeg_data, jpeg_len);
    if (!image_b64) {
      ESP_LOGE(TAG, "Failed to encode JPEG to Base64");
      free(audio_b64);
      return ESP_ERR_NO_MEM;
    }

    const char *prefix = "data:image/jpeg;base64,";
    const size_t prefix_len = strlen(prefix);
    const size_t image_b64_len = strlen(image_b64);
    ESP_LOGI(TAG,
             "JPEG Base64: %u bytes -> %u chars, data URL will be %u chars",
             (unsigned)jpeg_len, (unsigned)image_b64_len,
             (unsigned)(prefix_len + image_b64_len));

    image_data_url = malloc(prefix_len + image_b64_len + 1);
    if (!image_data_url) {
      free(image_b64);
      free(audio_b64);
      return ESP_ERR_NO_MEM;
    }
    memcpy(image_data_url, prefix, prefix_len);
    memcpy(image_data_url + prefix_len, image_b64, image_b64_len + 1);
    image_data_url[prefix_len + image_b64_len] = '\0';
    free(image_b64);
  }

  const bool use_vision_model = (image_data_url != NULL);
  esp_err_t err = ESP_FAIL;

  if (use_vision_model) {
    // Ciclo 1: somente audio -> transcricao
    char transcript_text[APP_RESPONSE_TEXT_MAX] = {0};
    char *transcription_prompt = malloc(APP_RESPONSE_TEXT_MAX);
    if (!transcription_prompt) {
      free(audio_b64);
      free(image_data_url);
      return ESP_ERR_NO_MEM;
    }
    snprintf(transcription_prompt, APP_RESPONSE_TEXT_MAX,
             "Transcreva o audio em portugues-BR. "
             "Retorne SOMENTE o texto falado, sem comentarios nem explicacoes. "
             "Se ruidoso, faca sua melhor tentativa. "
             "Vocabulario tecnico esperado: %s.",
             app_profile_transcription_terms(s_expert_profile));

    const char *audio_model =
        (strcmp(config_manager_get()->ai_model, "gpt-4o") == 0)
            ? APP_AI_MODEL_AUDIO_TEXT
            : config_manager_get()->ai_model;
    err = app_call_ai_once(audio_model, audio_b64, NULL,
                           app_profile_system_prompt(s_expert_profile), NULL,
                           transcription_prompt, false, transcript_text,
                           sizeof(transcript_text));
    free(transcription_prompt);
    // CRITICO: libera audio_b64 logo apos o ciclo 1 para aliviar memoria
    // interna ANTES de abrir a segunda conexao HTTPS (ciclo 2 - visao).
    // O audio_b64 pode ter centenas de KB e concorre com o contexto TLS.
    free(audio_b64);
    audio_b64 = NULL;

    if (err != ESP_OK || transcript_text[0] == '\0') {
      strlcpy(transcript_text, "Descreva o que voce ve na imagem",
              sizeof(transcript_text));
      ESP_LOGW(TAG, "audio transcription failed; using fallback question");
    }

    // Ciclo 2: modelo de visao com imagem + texto da transcricao
    char *vision_prompt = malloc(APP_RESPONSE_TEXT_MAX * 2);
    if (!vision_prompt) {
      free(image_data_url);
      return ESP_ERR_NO_MEM;
    }
    snprintf(
        vision_prompt, APP_RESPONSE_TEXT_MAX * 2,
        "O usuario perguntou: \"%s\"\n"
        "INSTRUCOES:\n"
        "1. Responda a pergunta diretamente usando o que voce ve na imagem.\n"
        "2. Se a pergunta e sobre identificar algo, diga o que e.\n"
        "3. Se ha texto, numeros ou logos na imagem, leia-os.\n"
        "4. Se a pergunta nao tem relacao com a imagem, "
        "responda a pergunta mesmo assim usando seu conhecimento.\n"
        "5. Nunca diga apenas 'nao sei'. Sempre ofereca sua melhor analise.",
        transcript_text);

    err = app_call_ai_once(config_manager_get()->ai_model, NULL, image_data_url,
                           app_profile_system_prompt(s_expert_profile),
                           vision_prompt, NULL, true, out_text, out_text_len);
    free(vision_prompt);

    if (err == ESP_OK) {
      app_history_add(transcript_text, out_text);
    }
  } else {
    // Modo somente audio
    ESP_LOGI(TAG, "Audio-only path initiated");
    char *audio_only_prompt = malloc(APP_RESPONSE_TEXT_MAX);
    if (!audio_only_prompt) {
      free(audio_b64);
      free(image_data_url);
      return ESP_ERR_NO_MEM;
    }
    snprintf(audio_only_prompt, APP_RESPONSE_TEXT_MAX,
             "Ouca o audio e responda exatamente o que o usuario perguntou. "
             "Se a pergunta estiver cortada ou ruidosa, use o contexto para "
             "inferir a intencao. "
             "Nunca diga 'nao entendi' sem tentar responder. "
             "Vocabulario tecnico relevante: %s.",
             app_profile_transcription_terms(s_expert_profile));
    const char *audio_model =
        (strcmp(config_manager_get()->ai_model, "gpt-4o") == 0)
            ? APP_AI_MODEL_AUDIO_TEXT
            : config_manager_get()->ai_model;
    err = app_call_ai_once(audio_model, audio_b64, NULL,
                           app_profile_system_prompt(s_expert_profile), NULL,
                           audio_only_prompt, true, out_text, out_text_len);
    free(audio_only_prompt);

    if (err == ESP_OK) {
      app_history_add(NULL, out_text);
    }
    // Libera audio_b64 apos uso
    free(audio_b64);
    audio_b64 = NULL;
  }

  free(audio_b64); // seguro mesmo se ja foi liberado (NULL)
  free(image_data_url);
  return err;
}

static void app_set_state(app_state_t next_state) {
  s_state = next_state;

  /* Build status-bar text: "mode | profile | state" */
  const char *mode_short =
      (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) ? "Foto+Voz"
                                                                    : "Voz";
  const char *prof_short = app_profile_name(s_expert_profile);
  const char *state_str;
  const char *footer_str;

  switch (next_state) {
  case APP_STATE_IDLE:
    state_str = "Pronto";
    if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) {
      footer_str = s_photo_locked ? "Enc: falar | Knob: config"
                                  : "Btn1: foto | Knob: config";
    } else {
      footer_str = "Enc: falar | Knob: config";
    }
    break;
  case APP_STATE_SELECTING_MODE:
    state_str = "Config";
    footer_str = "Knob: modo | Btn1: perfil | Enc: OK";
    break;
  case APP_STATE_LISTENING:
    state_str = "Gravando";
    footer_str = "Solte para enviar...";
    break;
  case APP_STATE_TRANSCRIBING:
  case APP_STATE_THINKING:
    state_str = "Analisando";
    footer_str = "Aguarde...";
    break;
  case APP_STATE_SHOWING_RESPONSE:
    state_str = "Resposta";
    footer_str = "Btn2:sobe Btn3:desce Enc:sair";
    break;
  case APP_STATE_BOOTING:
    state_str = "Iniciando";
    footer_str = "Aguarde...";
    break;
  default:
    state_str = "Erro";
    footer_str = "Qualquer botao: tentar de novo";
    break;
  }

  char status[64];
  snprintf(status, sizeof(status), "%s | %s | %s", mode_short, prof_short,
           state_str);
  gui_set_state(status);
  gui_set_footer(footer_str);
}

static const char *app_mode_name(app_interaction_mode_t mode) {
  return (mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) ? "Foto+Voz" : "Voz";
}

static void app_show_mode_selection_ui(void) {
  char msg[APP_RESPONSE_TEXT_MAX];
  const bool is_voice = (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_TEXT);
  snprintf(msg, sizeof(msg),
           " %s Voz\n"
           " %s Foto + Voz\n\n"
           " Perfil: %s",
           is_voice ? "[*]" : "[ ]", is_voice ? "[ ]" : "[*]",
           app_profile_name(s_expert_profile));
  gui_set_response(msg);
}

static void app_enter_mode_selection(void) {
  // Check if we are already in mode selection to avoid recursion
  if (s_state == APP_STATE_SELECTING_MODE) {
    return;
  }
  app_set_state(APP_STATE_SELECTING_MODE);
  s_mode_select_last_activity_ticks = xTaskGetTickCount();
  gui_set_response_compact(true); /* Shrink panel so camera is visible */
  app_show_mode_selection_ui();
}

static void app_battery_init(void) {
  adc_unit_t unit;
  if (adc_oneshot_io_to_channel(BATTERY_ADC_PIN, &unit, &s_adc_channel) !=
      ESP_OK) {
    ESP_LOGE(TAG, "GPIO %d cannot be used for ADC", BATTERY_ADC_PIN);
    return;
  }

  adc_oneshot_unit_init_cfg_t init_config = {
      .unit_id = unit,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  if (adc_oneshot_new_unit(&init_config, &s_adc_handle) != ESP_OK) {
    return;
  }

  adc_oneshot_chan_cfg_t config = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = ADC_ATTEN_DB_12, // For reading up to ~3.3V / 3.1V
  };
  adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &config);

  // Call calibration (Scheme Curve Fitting for P4)
  adc_cali_curve_fitting_config_t cali_config = {
      .unit_id = unit,
      .chan = s_adc_channel,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  s_adc_calibrated = (adc_cali_create_scheme_curve_fitting(
                          &cali_config, &s_adc_cali_handle) == ESP_OK);
}

static int app_battery_read_percent(void) {
  if (!s_adc_handle)
    return -1;

  int raw = 0;
  if (adc_oneshot_read(s_adc_handle, s_adc_channel, &raw) != ESP_OK) {
    return -1;
  }

  int voltage_mv = 0;
  if (s_adc_calibrated) {
    adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &voltage_mv);
  } else {
    // Fallback rough estimate
    voltage_mv = (raw * 3100) / 4095;
  }

  // Tensão real multiplicada pelo divisor: (1M + 332k) / 332k = 1.332M / 332k
  // = 4.01 Caso use o divisor (R15=1M, R16=332K). Se R16 for NC, o ADC lerá
  // ~Raw VDD. Simulando divisor ativo:
  float vbat = (voltage_mv * 4.01f) / 1000.0f;

  int percent = (int)((vbat - 3.2f) * 100.0f / (4.2f - 3.2f));
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;

  return percent;
}

static void app_confirm_mode_selection(void) {
  gui_set_response_compact(false);
  char msg[APP_RESPONSE_TEXT_MAX];
  snprintf(msg, sizeof(msg), "OK! %s | %s\nSegure encoder e fale.",
           app_mode_name(s_interaction_mode),
           app_profile_name(s_expert_profile));
  strlcpy(s_last_response, msg, sizeof(s_last_response));
  app_set_state(APP_STATE_IDLE);

  if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) {
    /* Photo mode: hide panel so camera preview shows through. */
    gui_set_response_panel_visible(false);
  } else {
    gui_set_response(msg);
  }
}

static bool app_accept_button_edge(bool pressed_edge,
                                   TickType_t *last_press_ticks) {
  if (!pressed_edge || !last_press_ticks) {
    return false;
  }
  const TickType_t now = xTaskGetTickCount();
  if ((now - *last_press_ticks) < pdMS_TO_TICKS(APP_BUTTON_DEBOUNCE_MS)) {
    return false;
  }
  *last_press_ticks = now;
  return true;
}

static void app_clear_locked_photo(void) {
  free(s_locked_photo_jpeg);
  s_locked_photo_jpeg = NULL;
  s_locked_photo_jpeg_len = 0;
  s_photo_capture_requested = false;
  s_photo_locked = false;
}

static void app_update_live_preview_if_needed(TickType_t now_ticks) {
  if (app_storage_is_busy()) {
    if (s_preview_active) {
      s_preview_active = false;
      ESP_LOGI(TAG, "Camera preview paused while storage is busy");
    }
    return;
  }

  // Only update preview when in IDLE state and photo mode is active
  if (s_state != APP_STATE_IDLE ||
      s_interaction_mode != APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT ||
      s_photo_locked) {
    // Preview not needed - mark as inactive to free DMA
    if (s_preview_active) {
      s_preview_active = false;
      ESP_LOGI(TAG, "Camera preview disabled (state=%d) - DMA buffers freed",
               s_state);
      // Notify storage that preview is disabled - may have more DMA memory now
      app_storage_notify_interaction();
    }
    return;
  }

  // Preview is needed - mark as active
  if (!s_preview_active) {
    s_preview_active = true;
    ESP_LOGI(TAG, "Camera preview enabled - DMA buffers in use");
  }

  if ((now_ticks - s_last_preview_ticks) <
      pdMS_TO_TICKS(APP_PREVIEW_REFRESH_MS)) {
    return;
  }
  s_last_preview_ticks = now_ticks;

  /* Hide response panel so camera preview is fully visible. */
  gui_set_response_panel_visible(false);

  // CRITICAL: Protect against serial buffer overflow from camera init errors
  // Add exponential backoff if camera keeps failing to prevent flooding serial
  // port
  static int s_camera_error_count = 0;
  static TickType_t s_last_camera_error_ticks = 0;

  uint8_t *preview_data = NULL;
  uint16_t preview_w = 0;
  uint16_t preview_h = 0;

  static bool camera_vfs_registered = false;
  static bool camera_init_failed_permanently = false;

  esp_err_t camera_err = ESP_FAIL;

  if (!camera_init_failed_permanently) {
    if (!camera_vfs_registered) {
      camera_err = bsp_camera_capture_preview_rgb565(&preview_data, &preview_w,
                                                     &preview_h);
      if (camera_err == ESP_OK) {
        camera_vfs_registered = true;
      } else {
        // Wait longer before retrying to prevent VFS race
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    } else {
      camera_err = bsp_camera_capture_preview_rgb565(&preview_data, &preview_w,
                                                     &preview_h);
    }
  }

  if (camera_err == ESP_OK) {
    if (!camera_vfs_registered) {
      camera_vfs_registered = true;
    }
    s_camera_error_count = 0; // Reset error counter on success

    // Try one early mount after camera is stable to avoid runtime mount
    // failures due to heap fragmentation.
    // Atraso intencional para o subsistema ISP e barramento LDO estabilizarem
    vTaskDelay(pdMS_TO_TICKS(500));
    app_storage_mount_after_camera_init();

    (void)gui_show_camera_preview_rgb565(preview_data, preview_w, preview_h);
  } else {
    // Increment error counter and add delay to prevent serial buffer overflow
    s_camera_error_count++;
    TickType_t time_since_last_error = now_ticks - s_last_camera_error_ticks;

    // Se falhar consecutivamente muitas vezes, desiste de tentar ligar a câmera
    // este boot
    if (s_camera_error_count > 20) {
      camera_init_failed_permanently = true;
    }

    // Exponential backoff: delay increases with consecutive errors
    // Max delay: 5 seconds to prevent overwhelming serial port
    int delay_ms =
        (s_camera_error_count < 10) ? (s_camera_error_count * 100) : 5000;

    // Only delay if enough time has passed since last error (avoid blocking)
    if (time_since_last_error > pdMS_TO_TICKS(delay_ms)) {
      if (s_camera_error_count <= 3) {
        // Log first few errors normally
        ESP_LOGD(TAG, "Camera preview failed (count: %d): %s",
                 s_camera_error_count, esp_err_to_name(camera_err));
      } else if (s_camera_error_count == 4) {
        // Log once when entering backoff mode
        ESP_LOGW(TAG, "Camera init failing repeatedly, entering backoff mode "
                      "to protect serial port");
      }
      // Don't spam serial port - errors are already logged by BSP
      s_last_camera_error_ticks = now_ticks;
    }
  }
  free(preview_data);
}

static void app_capture_and_lock_photo(void) {
  app_clear_locked_photo();

  uint8_t *preview_data = NULL;
  uint16_t preview_w = 0;
  uint16_t preview_h = 0;
  esp_err_t prev_err =
      bsp_camera_capture_preview_rgb565(&preview_data, &preview_w, &preview_h);
  if (prev_err == ESP_OK) {
    (void)gui_show_camera_preview_rgb565(preview_data, preview_w, preview_h);
  }
  free(preview_data);

  esp_err_t cam_err =
      bsp_camera_capture_jpeg(&s_locked_photo_jpeg, &s_locked_photo_jpeg_len);
  if (cam_err != ESP_OK || !s_locked_photo_jpeg ||
      s_locked_photo_jpeg_len == 0) {
    app_clear_locked_photo();
    gui_set_response_compact(true);
    gui_set_response("Erro ao capturar foto.\nTente novamente.");
    return;
  }

  s_photo_capture_requested = true;
  s_photo_locked = true;
  gui_set_response_compact(true);
  gui_set_response("Foto OK!\nSegure encoder e fale.");
}

static esp_err_t app_do_interaction(void) {
  ESP_LOGI(TAG, "starting interaction in mode=%s",
           app_mode_name(s_interaction_mode));

  uint8_t *jpeg_data = NULL;
  size_t jpeg_len = 0;
  if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) {
    if (!s_photo_locked || !s_photo_capture_requested || !s_locked_photo_jpeg ||
        s_locked_photo_jpeg_len == 0) {
      gui_set_response_compact(true);
      gui_set_response("Capture foto primeiro.\n(Btn1)");
      app_set_state(APP_STATE_IDLE);
      return ESP_OK;
    }
    jpeg_data = s_locked_photo_jpeg;
    jpeg_len = s_locked_photo_jpeg_len;
    s_locked_photo_jpeg = NULL;
    s_locked_photo_jpeg_len = 0;
    s_photo_locked = false;
    s_photo_capture_requested = false;
  }

  const size_t audio_buffer_len = APP_AUDIO_BUFFER_LEN;
  uint8_t *audio_buffer =
      heap_caps_malloc(audio_buffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!audio_buffer) {
    audio_buffer = heap_caps_malloc(audio_buffer_len,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!audio_buffer) {
    ESP_LOGE(TAG, "no memory for audio buffer");
    return ESP_ERR_NO_MEM;
  }
  app_set_state(APP_STATE_LISTENING);
  size_t captured_bytes = 0;
  const TickType_t capture_start = xTaskGetTickCount();
  while (bsp_button_is_pressed() && (xTaskGetTickCount() - capture_start) <
                                        pdMS_TO_TICKS(APP_MAX_CAPTURE_MS)) {
    const uint32_t elapsed_ms =
        (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - capture_start);
    const uint8_t progress =
        (elapsed_ms >= APP_MAX_CAPTURE_MS)
            ? 100
            : (uint8_t)((elapsed_ms * 100U) / APP_MAX_CAPTURE_MS);
    gui_set_recording_progress(progress);

    const size_t remaining = audio_buffer_len - captured_bytes;
    if (remaining < 1024) {
      break;
    }

    bsp_audio_capture_cfg_t capture_cfg = {
        .sample_rate_hz = 16000,
        .bits_per_sample = 16,
        .channels = 1,
        .capture_ms = APP_CAPTURE_CHUNK_MS,
    };

    size_t chunk_bytes = 0;
    esp_err_t capture_err = bsp_audio_capture_blocking(
        &capture_cfg, audio_buffer + captured_bytes, remaining, &chunk_bytes);
    if (capture_err != ESP_OK) {
      ESP_LOGE(TAG, "audio capture failed: %s", esp_err_to_name(capture_err));
      free(audio_buffer);
      return capture_err;
    }
    captured_bytes += chunk_bytes;
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (captured_bytes < APP_MIN_CAPTURE_BYTES) {
    if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) {
      gui_set_response_compact(true);
    }
    gui_set_response("Fale por mais tempo\n(minimo 2 segundos).");
    app_set_state(APP_STATE_IDLE);
    free(audio_buffer);
    return ESP_OK;
  }

  // Disable preview when starting interaction to free DMA memory
  if (s_preview_active) {
    s_preview_active = false;
    ESP_LOGI(TAG,
             "Camera preview disabled for interaction - DMA buffers freed");
  }

  app_set_state(APP_STATE_THINKING);

  size_t wav_len = 0;
  uint8_t *wav_data =
      app_pcm16_to_wav(audio_buffer, captured_bytes, 16000, 1, 16, &wav_len);
  if (!wav_data) {
    free(audio_buffer);
    return ESP_ERR_NO_MEM;
  }

  char ai_response[APP_RESPONSE_TEXT_MAX];
  esp_err_t ai_err = app_call_ai_with_audio(
      wav_data, wav_len, jpeg_data, jpeg_len, ai_response, sizeof(ai_response));
  free(wav_data);

  // --- Save audio to SD card (WAV, opportunistic) ---
  // Salva o áudio PCM capturado no SD independente do resultado da IA.
  // Usa o audio_buffer (PCM bruto, 16-bit mono 16kHz) que ainda está válido
  // aqui.
  if (captured_bytes > 0) {
    esp_err_t audio_save_err =
        app_storage_save_audio(audio_buffer, captured_bytes, 16000);
    if (audio_save_err != ESP_OK) {
      ESP_LOGW(TAG, "Audio not saved to SD: %s",
               esp_err_to_name(audio_save_err));
    }
  }

  // Queue JPEG for saving after AI interaction (buffered in PSRAM)
  // This avoids SDMMC/DMA conflicts during network communication
  // Images will be saved after AI response is received
  if (jpeg_data && jpeg_len > 0) {
    esp_err_t queue_err = app_storage_queue_image(jpeg_data, jpeg_len);
    if (queue_err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to queue image for saving: %s",
               esp_err_to_name(queue_err));
    }
  }
  free(jpeg_data);

  /* Hide camera / photo so the response is shown on a clean background. */
  gui_hide_camera_preview();
  gui_set_response_compact(false); /* Ensure full panel for AI response */

  if (ai_err == ESP_OK) {
    app_utf8_to_ascii(ai_response);
    strlcpy(s_last_response, ai_response, sizeof(s_last_response));
    gui_set_response(s_last_response);

    // --- Salva log de chat no SD card ---
    const char *mode_label =
        (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT)
            ? "AUDIO_IMAGE_TEXT"
            : "AUDIO_TEXT";
    esp_err_t log_err = app_storage_save_chat_log(mode_label, s_last_response);
    if (log_err != ESP_OK) {
      ESP_LOGW(TAG, "Chat log not saved: %s", esp_err_to_name(log_err));
    }
  } else {
    gui_set_response(s_last_response);
  }

  // Notify storage system that interaction completed
  // Images are saved opportunistically when system is idle (after 10s
  // inactivity)
  app_storage_notify_interaction();

  app_set_state(APP_STATE_SHOWING_RESPONSE);
  // Don't immediately go to IDLE - wait for button press to clear response

  ESP_LOGI(TAG, "interaction finished (captured=%u bytes, ms=%u)",
           (unsigned)captured_bytes,
           (unsigned)((captured_bytes * 1000U) / (16000U * 2U)));
  free(audio_buffer);
  return ESP_OK;
}

static void app_task(void *arg) {
  app_event_t evt;
  (void)arg;
  TickType_t last_status_update = 0;

  while (1) {
    if (xQueueReceive(s_app_queue, &evt, pdMS_TO_TICKS(APP_BUTTON_POLL_MS)) ==
        pdTRUE) {
      if (evt.type == APP_EVT_BOOT) {
        app_set_state(APP_STATE_IDLE);
        continue;
      }

      if (evt.type == APP_EVT_INTERACTION_REQUESTED) {
        if (s_last_interaction_ticks > 0 &&
            (xTaskGetTickCount() - s_last_interaction_ticks) >
                pdMS_TO_TICKS(APP_HISTORY_TIMEOUT_MS)) {
          app_history_clear();
        }

        esp_err_t err = app_do_interaction();
        if (err != ESP_OK) {
          app_set_state(APP_STATE_ERROR);
          gui_set_response("Falha na comunicacao.\nTente novamente.");
        }
      }
    }

    if (xTaskGetTickCount() - last_status_update > pdMS_TO_TICKS(2000)) {
      last_status_update = xTaskGetTickCount();
      int batt = app_battery_read_percent();
      gui_set_status_icons(bsp_wifi_is_ready(), batt);
    }

    // Handle response view: btn2/btn3 scroll, encoder or btn1 exits.
    if (s_state == APP_STATE_SHOWING_RESPONSE) {
      const bool encoder_now_pressed = bsp_button_is_pressed();
      const bool encoder_pressed_edge =
          app_accept_button_edge(encoder_now_pressed && !s_prev_encoder_pressed,
                                 &s_last_encoder_press_ticks);
      const bool photo_button_now_pressed = bsp_photo_button_is_pressed();
      const bool photo_button_pressed_edge = app_accept_button_edge(
          photo_button_now_pressed && !s_prev_photo_button_pressed,
          &s_last_photo_press_ticks);
      const bool btn2_now = bsp_button2_is_pressed();
      const bool btn2_edge = app_accept_button_edge(
          btn2_now && !s_prev_btn2_pressed, &s_last_btn2_press_ticks);
      const bool btn3_now = bsp_button3_is_pressed();
      const bool btn3_edge = app_accept_button_edge(
          btn3_now && !s_prev_btn3_pressed, &s_last_btn3_press_ticks);

      /* Encoder or Btn1 exits response view. */
      if (encoder_pressed_edge || photo_button_pressed_edge) {
        app_set_state(APP_STATE_IDLE);
        if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) {
          app_clear_locked_photo();
          /* Camera preview will resume — hide response panel. */
          gui_set_response_compact(false);
          gui_set_response_panel_visible(false);
        } else {
          /* Voice-only mode: show default message on full panel. */
          gui_set_response_compact(false);
          gui_set_response(s_last_response);
        }

        /* Se descartou a tela usando o Encoder, ja tenta engatilhar
         * a proxima gravacao diretamente (One-Click-to-Talk) */
        if (encoder_pressed_edge) {
          if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT &&
              !s_photo_locked) {
            gui_set_response_compact(true);
            gui_set_response("Capture foto primeiro.\n(Btn1)");
          } else {
            s_interaction_requested = true;
            ESP_LOGI(TAG, "encoder press -> dismiss & start recording");
            const app_event_t interaction_evt = {
                .type = APP_EVT_INTERACTION_REQUESTED};
            xQueueSend(s_app_queue, &interaction_evt, 0);
          }
        }

        s_prev_encoder_pressed = encoder_now_pressed;
        s_prev_photo_button_pressed = photo_button_now_pressed;
        s_prev_btn2_pressed = btn2_now;
        s_prev_btn3_pressed = btn3_now;
        continue;
      }

      /* Btn2 = scroll up, Btn3 = scroll down */
      if (btn2_edge) {
        (void)gui_scroll_response(-APP_RESPONSE_SCROLL_STEP_PX);
      }
      if (btn3_edge) {
        (void)gui_scroll_response(APP_RESPONSE_SCROLL_STEP_PX);
      }

      s_prev_encoder_pressed = encoder_now_pressed;
      s_prev_photo_button_pressed = photo_button_now_pressed;
      s_prev_btn2_pressed = btn2_now;
      s_prev_btn3_pressed = btn3_now;
    }

    if (s_state == APP_STATE_IDLE || s_state == APP_STATE_SELECTING_MODE) {
      const bool encoder_now_pressed = bsp_button_is_pressed();
      const bool encoder_pressed_edge =
          app_accept_button_edge(encoder_now_pressed && !s_prev_encoder_pressed,
                                 &s_last_encoder_press_ticks);
      const bool photo_button_now_pressed = bsp_photo_button_is_pressed();
      const bool photo_button_pressed_edge = app_accept_button_edge(
          photo_button_now_pressed && !s_prev_photo_button_pressed,
          &s_last_photo_press_ticks);
      const int knob_delta = bsp_knob_consume_delta();

      if (knob_delta != 0) {
        if (s_state != APP_STATE_SELECTING_MODE) {
          app_enter_mode_selection();
        }
        if (knob_delta > 0) {
          s_interaction_mode = APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT;
        } else {
          s_interaction_mode = APP_INTERACTION_MODE_AUDIO_TEXT;
          app_clear_locked_photo();
        }
        app_history_clear();
        s_mode_select_last_activity_ticks = xTaskGetTickCount();
        app_show_mode_selection_ui();
      }

      if (s_state == APP_STATE_SELECTING_MODE) {
        if (encoder_pressed_edge) {
          app_confirm_mode_selection();
        } else if (photo_button_pressed_edge) {
          s_expert_profile = (app_expert_profile_t)((s_expert_profile + 1) % 3);
          s_mode_select_last_activity_ticks = xTaskGetTickCount();
          app_show_mode_selection_ui();
          app_history_clear();
        } else if ((xTaskGetTickCount() - s_mode_select_last_activity_ticks) >
                   pdMS_TO_TICKS(APP_MODE_SELECT_TIMEOUT_MS)) {
          gui_set_response_compact(false);
          app_set_state(APP_STATE_IDLE);
          if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT &&
              !s_photo_locked) {
            gui_set_response_panel_visible(false);
          } else {
            gui_set_response(s_last_response);
          }
        }
      } else {
        if (photo_button_pressed_edge) {
          if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT) {
            app_capture_and_lock_photo();
          } else {
            gui_set_response_compact(true);
            gui_set_response("Ative Foto+Voz no knob.");
          }
        }

        if (encoder_pressed_edge && !s_interaction_requested) {
          if (s_interaction_mode == APP_INTERACTION_MODE_AUDIO_IMAGE_TEXT &&
              !s_photo_locked) {
            gui_set_response_compact(true);
            gui_set_response("Capture foto primeiro.\n(Btn1)");
            s_prev_encoder_pressed = encoder_now_pressed;
            s_prev_photo_button_pressed = photo_button_now_pressed;
            continue;
          }
          s_interaction_requested = true;
          ESP_LOGI(TAG, "encoder press -> start recording");
          const app_event_t interaction_evt = {
              .type = APP_EVT_INTERACTION_REQUESTED};
          xQueueSend(s_app_queue, &interaction_evt, 0);
        } else if (!encoder_now_pressed) {
          s_interaction_requested = false;
        }

        app_update_live_preview_if_needed(xTaskGetTickCount());
      }

      /* --------------------------------------------------------
       * Detecção de Long-Press: Btn2 + Btn3 por 10 s
       * Abre o Captive Portal de configuração
       * -------------------------------------------------------- */
      const bool both_pressed =
          bsp_button2_is_pressed() && bsp_button3_is_pressed();
      if (both_pressed) {
        if (!s_config_longpress_active) {
          s_config_longpress_active = true;
          s_config_longpress_start = xTaskGetTickCount();
          ESP_LOGI(TAG, "Config portal long-press started (Btn2+Btn3)");
        } else {
          const uint32_t held_ms = (uint32_t)pdTICKS_TO_MS(
              xTaskGetTickCount() - s_config_longpress_start);
          /* Feedback visual de progresso (a cada 2 s) */
          if (held_ms > 0 && (held_ms % 2000) < APP_BUTTON_POLL_MS) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Config: segure... %us/10s",
                     (unsigned)(held_ms / 1000));
            gui_set_response(msg);
          }
          if (held_ms >= APP_CONFIG_PORTAL_LONGPRESS_MS) {
            ESP_LOGW(TAG, "Config portal triggered by long-press!");
            /* Não retorna — reinicia após salvar */
            captive_portal_start();
          }
        }
      } else {
        s_config_longpress_active = false;
      }

      s_prev_encoder_pressed = encoder_now_pressed;
      s_prev_photo_button_pressed = photo_button_now_pressed;
      s_prev_btn2_pressed = bsp_button2_is_pressed();
      s_prev_btn3_pressed = bsp_button3_is_pressed();
    }
  }
}

esp_err_t app_init(void) {
  if (s_app_queue) {
    return ESP_OK;
  }

  s_app_queue = xQueueCreate(APP_QUEUE_LENGTH, sizeof(app_event_t));
  if (!s_app_queue) {
    return ESP_ERR_NO_MEM;
  }

  BaseType_t task_ok =
      xTaskCreatePinnedToCore(app_task, "app_task", APP_TASK_STACK_SIZE, NULL,
                              APP_TASK_PRIORITY, NULL, 0);
  if (task_ok != pdPASS) {
    return ESP_FAIL;
  }

  s_chat_history =
      heap_caps_calloc(APP_MAX_HISTORY_TURNS, sizeof(app_history_entry_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_chat_history) {
    ESP_LOGW(TAG,
             "Failed to allocate PSRAM history, allocating in regular RAM");
    s_chat_history =
        heap_caps_calloc(APP_MAX_HISTORY_TURNS, sizeof(app_history_entry_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  // Initialize storage subsystem
  esp_err_t storage_err = app_storage_init();
  if (storage_err != ESP_OK) {
    ESP_LOGW(TAG, "Storage initialization failed: %s (continuing anyway)",
             esp_err_to_name(storage_err));
  }

  // Carrega configuração do SD card (settings.json)
  // Após o storage_init, o SD pode ou não estar montado ainda;
  // config_manager_load tenta ler e silencia se não encontrar.
  esp_err_t cfg_err = config_manager_load();
  if (cfg_err == ESP_OK) {
    ESP_LOGI(TAG, "Dynamic config loaded from SD card");
  } else if (cfg_err == ESP_ERR_NOT_FOUND) {
    ESP_LOGW(TAG, "settings.json not found — using compiled-in defaults");
  } else {
    ESP_LOGW(TAG, "Config load error: %s — using defaults",
             esp_err_to_name(cfg_err));
  }

  app_battery_init();

  return ESP_OK;
}

void app_start(void) {
  app_set_state(APP_STATE_BOOTING);

  const app_event_t boot_evt = {.type = APP_EVT_BOOT};
  if (s_app_queue) {
    xQueueSend(s_app_queue, &boot_evt, 0);
  }
  gui_set_response(s_last_response);
}

void app_request_interaction(void) {
  const app_event_t evt = {.type = APP_EVT_INTERACTION_REQUESTED};
  if (s_app_queue) {
    xQueueSend(s_app_queue, &evt, 0);
  }
}
