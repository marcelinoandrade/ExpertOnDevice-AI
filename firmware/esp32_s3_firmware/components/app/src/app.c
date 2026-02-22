#include "app.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "app_state.h"
#include "bsp.h"
#include "gui.h"
#include "secret.h"

#define APP_TASK_STACK_SIZE (6 * 1024)
#define APP_TASK_PRIORITY 5
#define APP_QUEUE_LENGTH 8
#define APP_BUTTON_POLL_MS 40
#define APP_AUDIO_BUFFER_LEN (352 * 1024)
#define APP_CAPTURE_CHUNK_MS 120
#define APP_MAX_CAPTURE_MS 10000
#define APP_MIN_CAPTURE_BYTES 24000
#define APP_RESPONSE_TEXT_MAX 512
#define APP_AI_ENDPOINT "https://api.openai.com/v1/chat/completions"
#define APP_AI_MODEL "gpt-4o-audio-preview"
#define APP_HTTP_TIMEOUT_MS 45000

typedef enum {
    APP_EVT_BOOT = 0,
    APP_EVT_INTERACTION_REQUESTED,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
} app_event_t;

static const char *TAG = "app";
static QueueHandle_t s_app_queue;
static app_state_t s_state = APP_STATE_IDLE;
static bool s_interaction_requested;
static char s_last_response[APP_RESPONSE_TEXT_MAX] = "Segure o botao para falar";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} app_http_response_t;

static uint8_t *app_pcm16_to_wav(
    const uint8_t *pcm,
    size_t pcm_len,
    uint32_t sample_rate_hz,
    uint16_t channels,
    uint16_t bits_per_sample,
    size_t *wav_len
)
{
    if (!pcm || !wav_len || sample_rate_hz == 0 || channels == 0 || bits_per_sample == 0) {
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

static char *app_base64_encode(const uint8_t *data, size_t data_len)
{
    size_t out_len = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &out_len, data, data_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && ret != 0) {
        return NULL;
    }

    char *out = malloc(out_len + 1);
    if (!out) {
        return NULL;
    }

    ret = mbedtls_base64_encode((unsigned char *)out, out_len, &out_len, data, data_len);
    if (ret != 0) {
        free(out);
        return NULL;
    }
    out[out_len] = '\0';
    return out;
}

static esp_err_t app_http_append(app_http_response_t *resp, const char *data, int len)
{
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

static void app_utf8_to_ascii(char *text)
{
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
            case 0xA1: case 0xA0: case 0xA2: case 0xA3: case 0xA4: case 0xA5: *dst++ = 'a'; break; // a
            case 0x81: case 0x80: case 0x82: case 0x83: case 0x84: case 0x85: *dst++ = 'A'; break; // A
            case 0xA9: case 0xA8: case 0xAA: case 0xAB: *dst++ = 'e'; break; // e
            case 0x89: case 0x88: case 0x8A: case 0x8B: *dst++ = 'E'; break; // E
            case 0xAD: case 0xAC: case 0xAE: case 0xAF: *dst++ = 'i'; break; // i
            case 0x8D: case 0x8C: case 0x8E: case 0x8F: *dst++ = 'I'; break; // I
            case 0xB3: case 0xB2: case 0xB4: case 0xB5: case 0xB6: *dst++ = 'o'; break; // o
            case 0x93: case 0x92: case 0x94: case 0x95: case 0x96: *dst++ = 'O'; break; // O
            case 0xBA: case 0xB9: case 0xBB: case 0xBC: *dst++ = 'u'; break; // u
            case 0x9A: case 0x99: case 0x9B: case 0x9C: *dst++ = 'U'; break; // U
            case 0xA7: *dst++ = 'c'; break; // c
            case 0x87: *dst++ = 'C'; break; // C
            case 0xB1: *dst++ = 'n'; break; // n
            case 0x91: *dst++ = 'N'; break; // N
            default: break;
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

static esp_err_t app_extract_response_text(const char *json, char *out_text, size_t out_text_len)
{
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

static esp_err_t app_build_ai_request_json(
    const char *audio_b64,
    char **out_json
)
{
    if (!audio_b64 || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    if (!root || !messages) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", APP_AI_MODEL);
    cJSON_AddItemToObject(root, "messages", messages);

    cJSON *system_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(
        system_msg,
        "content",
        "Voce e um assistente embarcado em ESP32 com display 240x320. "
        "Responda em portugues-BR, curto e pragmatico, usando apenas caracteres ASCII (sem acentos). "
        "Maximo 120 caracteres."
    );
    cJSON_AddItemToArray(messages, system_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON *user_content = cJSON_CreateArray();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddItemToObject(user_msg, "content", user_content);

    cJSON *ctx_part = cJSON_CreateObject();
    cJSON_AddStringToObject(ctx_part, "type", "text");
    cJSON_AddStringToObject(ctx_part, "text", "Interprete o audio do usuario e responda a pergunta falada.");
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

static esp_err_t app_call_ai_with_audio(
    const uint8_t *wav_data,
    size_t wav_len,
    char *out_text,
    size_t out_text_len
)
{
    if (!wav_data || wav_len == 0 || !out_text || out_text_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(SECRET_OPENAI_API_KEY, "YOUR_API_KEY_HERE") == 0) {
        strlcpy(out_text, "Configure SECRET_OPENAI_API_KEY em secret.h para habilitar IA.", out_text_len);
        return ESP_ERR_INVALID_STATE;
    }

    char *audio_b64 = app_base64_encode(wav_data, wav_len);
    if (!audio_b64) {
        return ESP_ERR_NO_MEM;
    }

    char *request_json = NULL;
    esp_err_t build_err = app_build_ai_request_json(audio_b64, &request_json);
    free(audio_b64);
    if (build_err != ESP_OK) {
        return build_err;
    }

    app_http_response_t resp = {0};
    esp_http_client_config_t config = {
        .url = APP_AI_ENDPOINT,
        .timeout_ms = APP_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(request_json);
        return ESP_FAIL;
    }

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", SECRET_OPENAI_API_KEY);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_json, (int)strlen(request_json));

    esp_err_t err = esp_http_client_open(client, strlen(request_json));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(request_json);
        return err;
    }

    int written = esp_http_client_write(client, request_json, (int)strlen(request_json));
    if (written < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(request_json);
        return ESP_FAIL;
    }

    int status = esp_http_client_fetch_headers(client);
    (void)status;
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
        err = app_http_append(&resp, read_buf, read);
        if (err != ESP_OK) {
            break;
        }
    }

    const int http_code = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(request_json);

    if (err != ESP_OK) {
        free(resp.data);
        return err;
    }

    if (http_code < 200 || http_code >= 300) {
        ESP_LOGE(TAG, "AI HTTP status=%d body=%s", http_code, resp.data ? resp.data : "(empty)");
        free(resp.data);
        return ESP_FAIL;
    }

    err = app_extract_response_text(resp.data, out_text, out_text_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI parse error (%s), body=%s", esp_err_to_name(err), resp.data ? resp.data : "(empty)");
    }
    free(resp.data);
    return err;
}

static void app_set_state(app_state_t next_state)
{
    s_state = next_state;
    switch (next_state) {
    case APP_STATE_IDLE:
        gui_set_state("Pronto");
        break;
    case APP_STATE_LISTENING:
        gui_set_state("Gravando...");
        break;
    case APP_STATE_TRANSCRIBING:
        gui_set_state("Processando...");
        break;
    case APP_STATE_THINKING:
        gui_set_state("Processando...");
        break;
    case APP_STATE_SHOWING_RESPONSE:
        gui_set_state("Pronto");
        break;
    default:
        gui_set_state("Erro");
        break;
    }
}

static esp_err_t app_do_interaction(void)
{
    const size_t audio_buffer_len = APP_AUDIO_BUFFER_LEN;
    uint8_t *audio_buffer = heap_caps_malloc(audio_buffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_buffer) {
        audio_buffer = heap_caps_malloc(audio_buffer_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!audio_buffer) {
        ESP_LOGE(TAG, "no memory for audio buffer");
        return ESP_ERR_NO_MEM;
    }
    app_set_state(APP_STATE_LISTENING);
    size_t captured_bytes = 0;
    const TickType_t capture_start = xTaskGetTickCount();
    while (bsp_button_is_pressed() &&
           (xTaskGetTickCount() - capture_start) < pdMS_TO_TICKS(APP_MAX_CAPTURE_MS)) {
        const uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - capture_start);
        const uint8_t progress = (elapsed_ms >= APP_MAX_CAPTURE_MS)
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
            &capture_cfg,
            audio_buffer + captured_bytes,
            remaining,
            &chunk_bytes
        );
        if (capture_err != ESP_OK) {
            ESP_LOGE(TAG, "audio capture failed: %s", esp_err_to_name(capture_err));
            free(audio_buffer);
            return capture_err;
        }
        captured_bytes += chunk_bytes;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (captured_bytes < APP_MIN_CAPTURE_BYTES) {
        gui_set_response("Segure e fale por 2-4s");
        app_set_state(APP_STATE_IDLE);
        free(audio_buffer);
        return ESP_OK;
    }

    app_set_state(APP_STATE_THINKING);
    size_t wav_len = 0;
    uint8_t *wav_data = app_pcm16_to_wav(
        audio_buffer,
        captured_bytes,
        16000,
        1,
        16,
        &wav_len
    );
    if (!wav_data) {
        free(audio_buffer);
        return ESP_ERR_NO_MEM;
    }

    char ai_response[APP_RESPONSE_TEXT_MAX];
    esp_err_t ai_err = app_call_ai_with_audio(
        wav_data,
        wav_len,
        ai_response,
        sizeof(ai_response)
    );
    free(wav_data);

    if (ai_err == ESP_OK) {
        app_utf8_to_ascii(ai_response);
        strlcpy(s_last_response, ai_response, sizeof(s_last_response));
        gui_set_response(s_last_response);
    } else {
        gui_set_response(s_last_response);
    }

    app_set_state(APP_STATE_SHOWING_RESPONSE);
    app_set_state(APP_STATE_IDLE);

    ESP_LOGI(TAG, "interaction finished (captured=%u bytes, ms=%u)", (unsigned)captured_bytes,
             (unsigned)((captured_bytes * 1000U) / (16000U * 2U)));
    free(audio_buffer);
    return ESP_OK;
}

static void app_task(void *arg)
{
    app_event_t evt;
    (void)arg;

    while (1) {
        if (xQueueReceive(s_app_queue, &evt, pdMS_TO_TICKS(APP_BUTTON_POLL_MS)) == pdTRUE) {
            if (evt.type == APP_EVT_BOOT) {
                app_set_state(APP_STATE_IDLE);
                continue;
            }

            if (evt.type == APP_EVT_INTERACTION_REQUESTED) {
                esp_err_t err = app_do_interaction();
                if (err != ESP_OK) {
                    app_set_state(APP_STATE_ERROR);
                    gui_set_response("Erro na interacao. Veja logs.");
                }
            }
        }

        if (s_state == APP_STATE_IDLE) {
            const bool button_pressed = bsp_button_is_pressed();
            if (button_pressed && !s_interaction_requested) {
                s_interaction_requested = true;
                ESP_LOGI(TAG, "button press -> start recording");
                const app_event_t interaction_evt = {.type = APP_EVT_INTERACTION_REQUESTED};
                xQueueSend(s_app_queue, &interaction_evt, 0);
            } else if (!button_pressed) {
                s_interaction_requested = false;
            }
        }
    }
}

esp_err_t app_init(void)
{
    if (s_app_queue) {
        return ESP_OK;
    }

    s_app_queue = xQueueCreate(APP_QUEUE_LENGTH, sizeof(app_event_t));
    if (!s_app_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        app_task,
        "app_task",
        APP_TASK_STACK_SIZE,
        NULL,
        APP_TASK_PRIORITY,
        NULL,
        0
    );
    if (task_ok != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void app_start(void)
{
    const app_event_t evt = {.type = APP_EVT_BOOT};
    if (s_app_queue) {
        xQueueSend(s_app_queue, &evt, 0);
    }
    gui_set_response(s_last_response);
}

void app_request_interaction(void)
{
    const app_event_t evt = {.type = APP_EVT_INTERACTION_REQUESTED};
    if (s_app_queue) {
        xQueueSend(s_app_queue, &evt, 0);
    }
}
