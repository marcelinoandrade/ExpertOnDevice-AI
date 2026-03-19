#include "app.h"

#include <netdb.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "app_storage.h"
#include "audio_utils.h"
#include "bsp.h"
#include "captive_portal.h"
#include "cJSON.h"
#include "config_manager.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gui.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/base64.h"

#define APP_TASK_STACK_SIZE (10 * 1024)
#define APP_TASK_PRIORITY 5
#define APP_QUEUE_LENGTH 8
#define APP_BUTTON_POLL_MS 40
#define APP_BUTTON_DEBOUNCE_MS 140
#define APP_AUDIO_BUFFER_LEN (660 * 1024)
#define APP_CAPTURE_CHUNK_MS 100
#define APP_MAX_CAPTURE_MS 20000
#define APP_MIN_CAPTURE_BYTES 24000
#define APP_MODE_SELECT_TIMEOUT_MS 4000
#define APP_PREVIEW_REFRESH_MS 220
#define APP_RESPONSE_TEXT_MAX 1024
#define APP_RESPONSE_SCROLL_STEP_PX 22
#define APP_HTTP_TIMEOUT_MS 45000
#define APP_DEEP_SLEEP_TIMEOUT_MS 45000
#define APP_DEEP_SLEEP_WARNING_MS 35000
// AI Modes

typedef enum {
  APP_EVT_BOOT = 0,
  APP_EVT_INTERACTION_REQUESTED,
  APP_EVT_GUI_EVENT,
  APP_EVT_DEEP_SLEEP_WARNING,
  APP_EVT_DEEP_SLEEP_TRIGGERED,
} app_event_type_t;

typedef struct {
  app_event_type_t type;
  gui_event_type_t gui_event;
} app_event_t;

static const char *TAG = "app";
static QueueHandle_t s_app_queue;
static app_state_t s_state = APP_STATE_BOOTING;
static bool s_prev_button_state = false;
static uint32_t s_last_click_ms = 0;
static uint32_t s_last_interaction_end_ms = 0;
static uint32_t s_config_longpress_start;
static bool s_config_longpress_active;

/* Persistent HTTP client — reused across interactions to avoid TLS handshake */
static esp_http_client_handle_t s_http_client = NULL;
static char s_http_client_url[512] = {0};

static app_expert_profile_t s_expert_profile = 0; /* índice 0 = primeiro perfil */
static char s_last_response[APP_RESPONSE_TEXT_MAX] =
    "Pronto.\nSegure para falar.";

static TimerHandle_t s_deep_sleep_timer = NULL;
static TimerHandle_t s_sleep_warning_timer = NULL;

static void deep_sleep_timer_cb(TimerHandle_t xTimer) {
  app_event_t evt = {.type = APP_EVT_DEEP_SLEEP_TRIGGERED};
  if (s_app_queue) {
    xQueueSend(s_app_queue, &evt, 0);
  }
}

static void sleep_warning_timer_cb(TimerHandle_t xTimer) {
  app_event_t evt = {.type = APP_EVT_DEEP_SLEEP_WARNING};
  if (s_app_queue) {
    xQueueSend(s_app_queue, &evt, 0);
  }
}

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

static void app_gui_event_cb(gui_event_type_t event) {
  app_event_t evt = {.type = APP_EVT_GUI_EVENT, .gui_event = event};
  if (s_app_queue) {
    xQueueSend(s_app_queue, &evt, 0);
  }
}

/* Long-press config portal: btn2 + btn3 simultaneos por 10 s */
#define APP_CONFIG_PORTAL_LONGPRESS_MS 10000

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

static void app_set_state(app_state_t state);

static const char *app_profile_system_prompt(app_expert_profile_t profile) {
  const app_config_t *cfg = config_manager_get();
  if (profile < cfg->num_profiles) {
    return cfg->profiles[profile].prompt;
  }
  return cfg->profiles[0].prompt; /* fallback seguro */
}

static const char *
app_profile_transcription_terms(app_expert_profile_t profile) {
  const app_config_t *cfg = config_manager_get();
  if (profile < cfg->num_profiles) {
    return cfg->profiles[profile].terms;
  }
  return cfg->profiles[0].terms; /* fallback seguro */
}

static esp_err_t app_build_ai_request_json(
    const char *model, const char *audio_b64, const char *system_profile_text,
    const char *audio_context_text, bool inject_history, char **out_json) {
  if (!model || !out_json || !audio_b64) {
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
  cJSON_AddBoolToObject(root, "stream", true);
  cJSON_AddItemToObject(root, "messages", messages);

  cJSON *system_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(system_msg, "role", "system");
  /* Buffer dimensionado para o pior caso:
   * ~431 chars fixos + 255 (personality) + 511 (profile prompt) + margem = 1280.
   * Usa 1536 para folga segura. */
  char *system_content = malloc(1536);
  if (!system_content) {
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  const char *personality = config_manager_get()->ai_personality;
  const char *profile_text = (system_profile_text && system_profile_text[0])
                                 ? system_profile_text
                                 : "";
  snprintf(
      system_content, 1536,
      "Voce e um assistente por voz embarcado (apenas audio). "
      "REGRA PRINCIPAL: responda em LINGUAGEM NATURAL, como se estivesse "
      "conversando. "
      "NUNCA responda usando formatos de dados (JSON, dicionarios, listas). "
      "Formato: portugues-BR, texto plano, sem markdown, sem emojis, ASCII "
      "apenas. "
      "Maximo 60 palavras. Quebre linhas a cada ~30 chars para caber na tela. "
      "Sempre tente ser util.\n"
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

  cJSON_AddItemToArray(messages, user_msg);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    return ESP_ERR_NO_MEM;
  }

  *out_json = json;
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * Persistent HTTP client management
 * ----------------------------------------------------------------------- */

static void app_http_client_invalidate(void) {
  if (s_http_client) {
    esp_http_client_cleanup(s_http_client);
    s_http_client = NULL;
  }
  s_http_client_url[0] = '\0';
}

static esp_err_t app_http_client_ensure(void) {
  const char *url = config_manager_get()->ai_base_url;
  if (s_http_client && strcmp(s_http_client_url, url) == 0) {
    return ESP_OK; /* already valid and URL unchanged */
  }
  app_http_client_invalidate();
  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = APP_HTTP_TIMEOUT_MS,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
      .keep_alive_idle = 5,
      .keep_alive_interval = 5,
      .keep_alive_count = 3,
  };
  s_http_client = esp_http_client_init(&cfg);
  if (!s_http_client) {
    return ESP_FAIL;
  }
  strlcpy(s_http_client_url, url, sizeof(s_http_client_url));
  ESP_LOGI(TAG, "HTTP client initialized: %s", url);
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * SSE streaming parser
 * ----------------------------------------------------------------------- */

#define APP_SSE_LINE_BUF 4096

typedef struct {
  char text[APP_RESPONSE_TEXT_MAX]; /* accumulated response text */
  size_t text_len;
  char line_buf[APP_SSE_LINE_BUF]; /* current SSE line assembly buffer */
  size_t line_buf_pos;
  bool done;                /* [DONE] received */
  TickType_t last_gui_tick; /* rate-limit GUI updates */
} app_sse_ctx_t;

static void app_sse_on_data(app_sse_ctx_t *ctx, const char *data) {
  if (strcmp(data, "[DONE]") == 0) {
    ctx->done = true;
    return;
  }

  cJSON *chunk = cJSON_Parse(data);
  if (!chunk) {
    return;
  }

  const char *frag = NULL;
  cJSON *choices = cJSON_GetObjectItemCaseSensitive(chunk, "choices");
  if (cJSON_IsArray(choices)) {
    cJSON *c0 = cJSON_GetArrayItem(choices, 0);
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(c0, "delta");
    if (delta) {
      /* Standard text models: delta.content */
      cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
      if (cJSON_IsString(content) && content->valuestring &&
          content->valuestring[0]) {
        frag = content->valuestring;
      } else {
        /* gpt-4o-audio-preview: delta.audio.transcript */
        cJSON *audio = cJSON_GetObjectItemCaseSensitive(delta, "audio");
        if (audio) {
          cJSON *tr = cJSON_GetObjectItemCaseSensitive(audio, "transcript");
          if (cJSON_IsString(tr) && tr->valuestring && tr->valuestring[0]) {
            frag = tr->valuestring;
          }
        }
      }
    }
  }

  if (frag) {
    size_t fl = strlen(frag);
    if (ctx->text_len + fl < APP_RESPONSE_TEXT_MAX - 1) {
      memcpy(ctx->text + ctx->text_len, frag, fl);
      ctx->text_len += fl;
      ctx->text[ctx->text_len] = '\0';
    }
    /* Update display at most every 250 ms to avoid GUI overload */
    TickType_t now = xTaskGetTickCount();
    if ((now - ctx->last_gui_tick) >= pdMS_TO_TICKS(250)) {
      /* Alocado no heap para não pressionar a stack da app_task (10 KB).
       * Com APP_RESPONSE_TEXT_MAX=1024 + ai_response[1024] no caller,
       * colocar na stack causaria ~4 KB de uso simultâneo — risco de overflow. */
      char *disp = malloc(APP_RESPONSE_TEXT_MAX);
      if (disp) {
        strlcpy(disp, ctx->text, APP_RESPONSE_TEXT_MAX);
        app_utf8_to_ascii(disp);
        gui_set_response(disp);
        free(disp);
      }
      ctx->last_gui_tick = now;
    }
  }

  cJSON_Delete(chunk);
}

static void app_sse_feed(app_sse_ctx_t *ctx, const char *buf, int len) {
  for (int i = 0; i < len && !ctx->done; i++) {
    char c = buf[i];
    if (c == '\n') {
      /* strip trailing \r */
      if (ctx->line_buf_pos > 0 &&
          ctx->line_buf[ctx->line_buf_pos - 1] == '\r') {
        ctx->line_buf_pos--;
      }
      ctx->line_buf[ctx->line_buf_pos] = '\0';
      if (strncmp(ctx->line_buf, "data: ", 6) == 0) {
        app_sse_on_data(ctx, ctx->line_buf + 6);
      }
      ctx->line_buf_pos = 0;
    } else if (ctx->line_buf_pos < APP_SSE_LINE_BUF - 1) {
      ctx->line_buf[ctx->line_buf_pos++] = c;
    }
  }
}

/* -----------------------------------------------------------------------
 * HTTP POST (streaming, persistent client)
 * ----------------------------------------------------------------------- */

static esp_err_t app_http_post_json(const char *request_json, char *out_text,
                                    size_t out_text_len, int *http_code) {
  if (!request_json || !out_text || !http_code) {
    return ESP_ERR_INVALID_ARG;
  }
  *http_code = 0;

  esp_err_t err = app_http_client_ensure();
  if (err != ESP_OK) {
    return err;
  }

  const size_t json_len = strlen(request_json);

  esp_http_client_set_method(s_http_client, HTTP_METHOD_POST);
  /* Use token from config (set via Captive Portal or SD card).
   * Skip Authorization header for local servers (Ollama, etc.) that
   * don't require authentication. */
  const char *token = config_manager_get()->ai_token;
  if (token && token[0] != '\0') {
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
    esp_http_client_set_header(s_http_client, "Authorization", auth_header);
  }
  esp_http_client_set_header(s_http_client, "Content-Type", "application/json");
  esp_http_client_set_post_field(s_http_client, request_json, (int)json_len);

  err = esp_http_client_open(s_http_client, (int)json_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
    app_http_client_invalidate();
    return err;
  }

  int written =
      esp_http_client_write(s_http_client, request_json, (int)json_len);
  if (written < 0) {
    ESP_LOGE(TAG, "HTTP write failed");
    esp_http_client_close(s_http_client);
    app_http_client_invalidate();
    return ESP_FAIL;
  }

  if (esp_http_client_fetch_headers(s_http_client) < 0) {
    ESP_LOGE(TAG, "HTTP fetch headers failed");
    esp_http_client_close(s_http_client);
    app_http_client_invalidate();
    return ESP_FAIL;
  }

  *http_code = esp_http_client_get_status_code(s_http_client);
  if (*http_code < 200 || *http_code >= 300) {
    char err_buf[256] = {0};
    esp_http_client_read(s_http_client, err_buf, sizeof(err_buf) - 1);
    ESP_LOGE(TAG, "AI HTTP status=%d body=%.200s", *http_code, err_buf);
    esp_http_client_close(s_http_client);
    return ESP_FAIL;
  }

  /* Stream SSE response.
   * app_sse_ctx_t contém line_buf[4096] + text[1024] = ~5125 bytes.
   * Alocado no heap para não pressionar a stack da app_task (10 KB). */
  app_sse_ctx_t *sse = calloc(1, sizeof(app_sse_ctx_t));
  if (!sse) {
    esp_http_client_close(s_http_client);
    return ESP_ERR_NO_MEM;
  }
  sse->last_gui_tick = xTaskGetTickCount();

  char read_buf[512];
  while (!sse->done) {
    int rd = esp_http_client_read(s_http_client, read_buf, sizeof(read_buf));
    if (rd < 0) {
      err = ESP_FAIL;
      break;
    }
    if (rd == 0) {
      break; /* end of stream */
    }
    app_sse_feed(sse, read_buf, rd);
  }

  /* Close HTTP session — TCP socket kept alive for next interaction */
  esp_http_client_close(s_http_client);

  bool has_text = (sse->text_len > 0);
  if (has_text) {
    strlcpy(out_text, sse->text, out_text_len);
  }
  free(sse);

  if (has_text) return ESP_OK;
  return (err == ESP_OK) ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t app_call_ai_once(const char *model, const char *audio_b64,
                                  const char *system_profile_text,
                                  const char *audio_context_text,
                                  bool inject_history, char *out_text,
                                  size_t out_text_len) {
  if (!model || !out_text || out_text_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  char *request_json = NULL;
  esp_err_t err = app_build_ai_request_json(
      model, audio_b64, system_profile_text, audio_context_text, inject_history,
      &request_json);
  if (err != ESP_OK) {
    return err;
  }

  int http_code = 0;
  err = app_http_post_json(request_json, out_text, out_text_len, &http_code);
  free(request_json);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AI request failed (http=%d): %s", http_code,
             esp_err_to_name(err));
  }
  return err;
}

static esp_err_t app_call_ai_with_audio(const uint8_t *wav_data, size_t wav_len,
                                        char *out_text, size_t out_text_len) {
  if (!wav_data || wav_len == 0 || !out_text || out_text_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  /* Warn if no token configured and URL points to OpenAI (public cloud requires
   * auth) */
  const char *cfg_token = config_manager_get()->ai_token;
  const char *cfg_url = config_manager_get()->ai_base_url;
  /* Detecta servidores locais (Ollama, etc.) que não requerem token.
   * Usa prefixos após "://" para evitar falsos positivos como "110.x.x.x"
   * que conteria "10." em posição incorreta. */
  const char *url_host = strstr(cfg_url, "://");
  url_host = url_host ? url_host + 3 : cfg_url;
  bool is_local = (strncmp(url_host, "localhost", 9) == 0    ||
                   strncmp(url_host, "127.0.0.1", 9) == 0    ||
                   strncmp(url_host, "192.168.", 8) == 0      ||
                   strncmp(url_host, "10.", 3) == 0            ||
                   strncmp(url_host, "172.", 4) == 0);
  if ((!cfg_token || cfg_token[0] == '\0') && !is_local) {
    strlcpy(out_text,
            "Token nao configurado.\nAcesse o Portal para\nconfigurar a chave "
            "de API.",
            out_text_len);
    return ESP_ERR_INVALID_STATE;
  }

  /* No S3 Wifi we assume network is up if bsp_wifi_is_ready is true */

  char *audio_b64 = app_base64_encode(wav_data, wav_len);
  if (!audio_b64) {
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Audio-only path initiated");
  char *audio_only_prompt = malloc(APP_RESPONSE_TEXT_MAX);
  if (!audio_only_prompt) {
    free(audio_b64);
    return ESP_ERR_NO_MEM;
  }
  snprintf(audio_only_prompt, APP_RESPONSE_TEXT_MAX,
           "Ouca o audio e responda exatamente o que o usuario perguntou. "
           "Se a pergunta estiver cortada ou ruidosa, use o contexto para "
           "inferir a intencao. "
           "Nunca diga 'nao entendi' sem tentar responder. "
           "Vocabulario tecnico relevante: %s.",
           app_profile_transcription_terms(s_expert_profile));

  const char *audio_model = config_manager_get()->ai_model;

  esp_err_t err = app_call_ai_once(
      audio_model, audio_b64, app_profile_system_prompt(s_expert_profile),
      audio_only_prompt, true, out_text, out_text_len);
  free(audio_only_prompt);

  if (err == ESP_OK) {
    app_history_add(NULL, out_text);
  }

  free(audio_b64);
  return err;
}

static void app_set_state(app_state_t next_state) {
  s_state = next_state;

  if (s_deep_sleep_timer) {
    if (next_state == APP_STATE_IDLE ||
        next_state == APP_STATE_SHOWING_RESPONSE) {
      xTimerReset(s_deep_sleep_timer, 0);
      xTimerReset(s_sleep_warning_timer, 0);
    } else {
      xTimerStop(s_deep_sleep_timer, 0);
      xTimerStop(s_sleep_warning_timer, 0);
    }
  }

  char state_buf[64];
  const char *state_str = "";
  switch (next_state) {
  case APP_STATE_IDLE: {
    const app_config_t *cfg = config_manager_get();
    const char *p_name = (s_expert_profile < cfg->num_profiles)
                             ? cfg->profiles[s_expert_profile].name
                             : cfg->profiles[0].name;
    snprintf(state_buf, sizeof(state_buf), "Pronto - %s", p_name);
    state_str = state_buf;
    break;
  }
  case APP_STATE_LISTENING:
    state_str = "Gravando...";
    break;
  case APP_STATE_TRANSCRIBING:
  case APP_STATE_THINKING:
    state_str = "Analisando...";
    break;
  case APP_STATE_SHOWING_RESPONSE:
    state_str = "Resposta";
    break;
  case APP_STATE_BOOTING:
    state_str = "Iniciando...";
    break;
  default:
    state_str = "Erro";
    break;
  }
  gui_set_state(state_str);
}

static esp_err_t app_do_interaction(void) {
  ESP_LOGI(TAG, "starting interaction in audio mode");

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
  uint32_t local_last_edge_ms = pdTICKS_TO_MS(xTaskGetTickCount());

  while ((xTaskGetTickCount() - capture_start) <
         pdMS_TO_TICKS(APP_MAX_CAPTURE_MS)) {
    const uint32_t elapsed_ms =
        (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - capture_start);
    const uint8_t progress =
        (elapsed_ms >= APP_MAX_CAPTURE_MS)
            ? 100
            : (uint8_t)((elapsed_ms * 100U) / APP_MAX_CAPTURE_MS);
    gui_set_recording_progress(progress);

    // Push-to-Talk detection to stop recording
    bool current_btn = bsp_button_is_pressed();
    if (current_btn) {
      uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
      if (now - local_last_edge_ms > 50) { // Slight debounce on release
        ESP_LOGI(TAG, "Button released -> stopping recording");
        s_prev_button_state =
            false; // PHYSICAL RELEASE IS COMING, sync with app_task loop
        s_last_click_ms = now;
        break;
      }
    } else {
      local_last_edge_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    }

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

    float rms =
        audio_calculate_rms((const int16_t *)(audio_buffer + captured_bytes),
                            chunk_bytes / sizeof(int16_t));
    captured_bytes += chunk_bytes;
    ESP_LOGI(TAG, "[RMS] Window: %.2f (Total: %u bytes)", rms,
             (unsigned)captured_bytes);
  }

  /* If capture was extremely short (e.g. just a quick click to dismiss screen),
   * silently cancel. (Lower threshold to 100ms / 3200 bytes) */
  if (captured_bytes < 3200) {
    app_set_state(APP_STATE_IDLE);
    gui_set_response(s_last_response);
    free(audio_buffer);
    return ESP_OK;
  }

  if (captured_bytes < APP_MIN_CAPTURE_BYTES) {
    gui_set_response("Fale por mais tempo\n(minimo 2 segundos).");
    app_set_state(APP_STATE_IDLE);
    free(audio_buffer);
    return ESP_OK;
  }

  /* High-pass filter: remove low-freq noise to improve speech clarity */
  const size_t sample_count = captured_bytes / sizeof(int16_t);
  audio_apply_highpass((int16_t *)audio_buffer, sample_count, 100.0f, 16000.0f);
  ESP_LOGI(TAG, "HPF applied: 100 Hz cutoff, %u samples",
           (unsigned)sample_count);

  app_set_state(APP_STATE_THINKING);

  size_t wav_len = 0;
  uint8_t *wav_data =
      app_pcm16_to_wav(audio_buffer, captured_bytes, 16000, 1, 16, &wav_len);
  if (!wav_data) {
    free(audio_buffer);
    return ESP_ERR_NO_MEM;
  }

  char ai_response[APP_RESPONSE_TEXT_MAX] = {0};
  esp_err_t ai_err = app_call_ai_with_audio(wav_data, wav_len, ai_response,
                                            sizeof(ai_response));
  free(wav_data);

  // --- Queue audio to be saved to SD card opportunistically ---
  if (captured_bytes > 0) {
    esp_err_t audio_save_err =
        app_storage_queue_audio(audio_buffer, captured_bytes, 16000);
    if (audio_save_err != ESP_OK) {
      ESP_LOGW(TAG, "Audio not queued: %s", esp_err_to_name(audio_save_err));
    }
  }

  if (ai_err == ESP_OK) {
    app_utf8_to_ascii(ai_response);
    strlcpy(s_last_response, ai_response, sizeof(s_last_response));
    gui_set_response(s_last_response);

    esp_err_t log_err =
        app_storage_save_chat_log("AUDIO_TEXT", s_last_response);
    if (log_err != ESP_OK) {
      ESP_LOGW(TAG, "Chat log not saved: %s", esp_err_to_name(log_err));
    }
  } else {
    gui_set_response(s_last_response);
  }

  app_storage_notify_interaction();
  s_last_interaction_end_ms = pdTICKS_TO_MS(xTaskGetTickCount());
  app_set_state(APP_STATE_SHOWING_RESPONSE);

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

  /* Critical: Initialize interaction state based on button level at boot.
   * This prevents triggering a recording immediately if the user is
   * still holding the button from a deep sleep wakeup. */
  ESP_LOGI(TAG, "Boot level check for button... (Active-Low)");

  while (1) {
    const bool button_pressed = bsp_button_is_pressed();

    if (xQueueReceive(s_app_queue, &evt, pdMS_TO_TICKS(APP_BUTTON_POLL_MS)) ==
        pdTRUE) {
      if (evt.type == APP_EVT_BOOT) {
        // Only transition to IDLE if we are actually still in BOOTING.
        // Don't override SHOWING_RESPONSE if a late boot event arrives
        if (s_state == APP_STATE_BOOTING) {
          app_set_state(APP_STATE_IDLE);
        }
        s_prev_button_state = button_pressed; // Ignore if held at boot
        continue;
      }

      if (evt.type == APP_EVT_GUI_EVENT) {
        if (s_deep_sleep_timer)
          xTimerReset(s_deep_sleep_timer, 0);
        if (s_sleep_warning_timer)
          xTimerReset(s_sleep_warning_timer, 0);

        if (evt.gui_event == GUI_EVENT_PROFILE) {
          const app_config_t *cfg = config_manager_get();
          s_expert_profile =
              (app_expert_profile_t)((s_expert_profile + 1) % cfg->num_profiles);
          ESP_LOGI(TAG, "Profile changed to: %d (%s)",
                   (int)s_expert_profile,
                   cfg->profiles[s_expert_profile].name);
          app_set_state(s_state); /* Refresh state text to reflect new profile */

          config_manager_get()->expert_profile = s_expert_profile;
          esp_err_t sv_err = config_manager_save();
          if (sv_err != ESP_OK) {
            ESP_LOGW(TAG, "Profile save failed: %s", esp_err_to_name(sv_err));
          }
        } else if (evt.gui_event == GUI_EVENT_SCROLL_UP) {
          gui_scroll_response(-APP_RESPONSE_SCROLL_STEP_PX);
        } else if (evt.gui_event == GUI_EVENT_SCROLL_DOWN) {
          gui_scroll_response(APP_RESPONSE_SCROLL_STEP_PX);
        }
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

      if (evt.type == APP_EVT_DEEP_SLEEP_WARNING) {
        if (s_state == APP_STATE_IDLE ||
            s_state == APP_STATE_SHOWING_RESPONSE) {
          ESP_LOGI(TAG, "Deep sleep warning: 10s remaining");
          gui_set_state("Suspensao em 10s");
        }
      }

      if (evt.type == APP_EVT_DEEP_SLEEP_TRIGGERED) {
        if (s_state == APP_STATE_IDLE ||
            s_state == APP_STATE_SHOWING_RESPONSE) {
          ESP_LOGI(TAG, "Inactivity timeout reached, preparing deep sleep...");
          gui_set_state("Entrando na Suspensao...");
          vTaskDelay(pdMS_TO_TICKS(
              1500)); // Give time for the UI to render the goodbye message
          bsp_enter_deep_sleep();
        }
      }
    }

    if (xTaskGetTickCount() - last_status_update > pdMS_TO_TICKS(2000)) {
      last_status_update = xTaskGetTickCount();

      int batt_percent = -1;
      bsp_battery_get_percent(&batt_percent);
      bool wifi_ready = bsp_wifi_is_ready();
      if (!wifi_ready) {
        // While not ready, show the animated connecting text
        gui_set_wifi_status_anim(true);
      } else {
        gui_set_status_icons(wifi_ready, batt_percent);
      }
    } else if (!bsp_wifi_is_ready() &&
               (xTaskGetTickCount() % pdMS_TO_TICKS(500) < pdMS_TO_TICKS(50))) {
      // Update the spinner animation faster than the 2s loop, approximately
      // every 500ms
      gui_set_wifi_status_anim(true);
    }

    // Push-to-Talk: Start on Press (Falling edge)
    bool is_edge = (!button_pressed && s_prev_button_state);

    if (is_edge) {
      uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
      // Interaction lockout (1000ms) + Press debounce (150ms)
      if ((now - s_last_interaction_end_ms > 1000) &&
          (now - s_last_click_ms > 150)) {
        s_last_click_ms = now;
        if (s_state == APP_STATE_IDLE || s_state == APP_STATE_ERROR ||
            s_state == APP_STATE_SHOWING_RESPONSE) {

          if (!gui_is_profile_pressed()) {
            ESP_LOGI(TAG, "button pressed -> start recording");
            const app_event_t interaction_evt = {
                .type = APP_EVT_INTERACTION_REQUESTED};
            xQueueSend(s_app_queue, &interaction_evt, 0);
          } else {
            ESP_LOGI(
                TAG,
                "button pressed with M held -> deferring recording for portal");
          }
        }
      }
    }
    s_prev_button_state = button_pressed;

    /* --------------------------------------------------------
     * Detecção de Long-Press (10 s) para Captive Portal
     * Trigger: Physical Button + Touch Profile Button ('M')
     * -------------------------------------------------------- */
    if (!button_pressed && gui_is_profile_pressed()) {
      if (!s_config_longpress_active) {
        s_config_longpress_active = true;
        s_config_longpress_start = xTaskGetTickCount();
      } else {
        const uint32_t held_ms = (uint32_t)pdTICKS_TO_MS(
            xTaskGetTickCount() - s_config_longpress_start);

        // Show feedback after holding for 3 seconds
        if (held_ms > 3000 && (s_state == APP_STATE_IDLE ||
                               s_state == APP_STATE_SHOWING_RESPONSE)) {

          if ((held_ms % 2000) < APP_BUTTON_POLL_MS) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Portal: segure... %us/10s",
                     (unsigned)(held_ms / 1000));
            app_set_state(APP_STATE_IDLE);
            gui_set_response(msg);
          }
        }

        if (held_ms >= APP_CONFIG_PORTAL_LONGPRESS_MS) {
          ESP_LOGW(TAG, "Config portal triggered by double-hold!");
          /* Não retorna — reinicia após salvar */
          captive_portal_start();
        }
      }
    } else {
      if (s_config_longpress_active) {
        // Se cancelou o long press antes dos 10s mas segurou mais que 3s,
        // restaura a UI
        const uint32_t held_ms = (uint32_t)pdTICKS_TO_MS(
            xTaskGetTickCount() - s_config_longpress_start);
        if (held_ms > 3000 && held_ms < APP_CONFIG_PORTAL_LONGPRESS_MS) {
          app_set_state(APP_STATE_IDLE);
          gui_set_response(s_last_response);
        }
      }
      s_config_longpress_active = false;
    }

    // Simplified loop - no edge tracking needed for hold-to-talk
  }
}

esp_err_t app_init(void) {
  if (s_app_queue) {
    return ESP_OK;
  }

  bsp_battery_init();

  s_app_queue = xQueueCreate(APP_QUEUE_LENGTH, sizeof(app_event_t));
  if (!s_app_queue) {
    return ESP_ERR_NO_MEM;
  }

  s_deep_sleep_timer =
      xTimerCreate("deep_sleep_timer", pdMS_TO_TICKS(APP_DEEP_SLEEP_TIMEOUT_MS),
                   pdFALSE, NULL, deep_sleep_timer_cb);
  if (s_deep_sleep_timer) {
    xTimerStart(s_deep_sleep_timer, 0);
  } else {
    ESP_LOGE(TAG, "Failed to create deep sleep timer");
  }

  s_sleep_warning_timer =
      xTimerCreate("sleep_warn_timer", pdMS_TO_TICKS(APP_DEEP_SLEEP_WARNING_MS),
                   pdFALSE, NULL, sleep_warning_timer_cb);
  if (s_sleep_warning_timer) {
    xTimerStart(s_sleep_warning_timer, 0);
  } else {
    ESP_LOGE(TAG, "Failed to create sleep warning timer");
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

  // Carrega configuracao do SD card (config.txt)
  esp_err_t cfg_err = config_manager_load();
  if (cfg_err == ESP_OK) {
    ESP_LOGI(TAG, "Dynamic config loaded from SD card");
  } else if (cfg_err == ESP_ERR_NOT_FOUND) {
    ESP_LOGW(TAG, "config.txt not found - using compiled-in defaults");
  } else {
    ESP_LOGW(TAG, "Config load error: %s - using defaults",
             esp_err_to_name(cfg_err));
  }

  // Now that config is loaded, configure and start the WiFi connection
  const app_config_t *cfg = config_manager_get();
  esp_err_t wifi_err =
      bsp_wifi_config_and_start(cfg->wifi_ssid, cfg->wifi_pass);
  if (wifi_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure and start WiFi: %s",
             esp_err_to_name(wifi_err));
  }

  // Restore the active profile from NVS/settings
  s_expert_profile = config_manager_get()->expert_profile;

  gui_set_event_callback(app_gui_event_cb);
  gui_set_footer("Segure para falar  SD: OK");
  return ESP_OK;
}

void app_start(void) {
  app_set_state(APP_STATE_BOOTING);

  const app_event_t boot_evt = {.type = APP_EVT_BOOT};
  if (s_app_queue) {
    xQueueSend(s_app_queue, &boot_evt, 0);
  }
  gui_set_response("Segure o botao e fale.");
  gui_set_footer("S3 Assistant  V1.0");
}

void app_request_interaction(void) {
  const app_event_t evt = {.type = APP_EVT_INTERACTION_REQUESTED};
  if (s_app_queue) {
    xQueueSend(s_app_queue, &evt, 0);
  }
}
