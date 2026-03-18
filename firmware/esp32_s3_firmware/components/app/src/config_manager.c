#include "config_manager.h"
#include "app_storage.h"
#include "bsp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "config_mgr";

#define SETTINGS_PATH     "/sdcard/data/config.txt"
#define SETTINGS_DIR      "/sdcard/data"
#define JSON_READ_BUF_CAP (8 * 1024)

/* -----------------------------------------------------------------------
 * Singleton — valores default (fallback quando config.txt não existe)
 * ----------------------------------------------------------------------- */
static app_config_t s_config = {
    .wifi_ssid      = "",
    .wifi_pass      = "",
    .ai_token       = "",
    .ai_personality = "",
    .ai_base_url    = "https://api.openai.com/v1/chat/completions",
    .ai_model       = "gpt-4o",
    .expert_profile = 0,

    .num_profiles = 3,
    .profiles = {
        [0] = {
            .name   = "Geral",
            .prompt = "Perfil ativo: Geral. Responda de forma pratica e direta. "
                      "Identifique objetos, leia textos, descreva cenas. "
                      "Sempre tente ser util.",
            .terms  = "NPK, fusarium, set-point, vazao",
        },
        [1] = {
            .name   = "Agronomo",
            .prompt = "Perfil ativo: Agronomo. Ao ver plantas, identifique especie "
                      "se possivel, avalie saude (cor das folhas, manchas, pragas "
                      "visiveis). Sugira diagnostico provavel e acao imediata "
                      "(ex: aplicar fungicida, irrigar, podar).",
            .terms  = "NPK, fusarium, clorose, necrose, oidio, ferrugem, praga, "
                      "fungicida, herbicida, irrigacao, vazao",
        },
        [2] = {
            .name   = "Engenheiro",
            .prompt = "Perfil ativo: Engenheiro. Ao ver circuitos/equipamentos, "
                      "identifique componentes, leia valores (resistores, capacitores), "
                      "note estado de LEDs, conexoes soltas ou queimadas. "
                      "Sugira ponto de verificacao.",
            .terms  = "set-point, vazao, corrente, tensao, curto, rele, inversor, "
                      "sensor, atuador, LED vermelho, falha",
        },
    },

    .volume     = 70,
    .brightness = 85,
    .loaded     = false,
};

/* Personalidade padrão usada quando o JSON não contém o campo */
static const char *s_default_personality =
    "Voce e um assistente inteligente e conciso.";

/* NOTA DE THREAD SAFETY:
 * s_config é acessado por duas tasks (app_task e httpd).
 * Não há mutex explícito porque o portal sempre chama esp_restart() após salvar,
 * tornando a janela de concorrência real inexistente em produção.
 * Se futuramente o portal for usado sem restart, adicionar um
 * SemaphoreHandle_t aqui e proteger load/save/get com xSemaphoreTake. */
app_config_t *config_manager_get(void) { return &s_config; }

/* -----------------------------------------------------------------------
 * Helpers internos
 * ----------------------------------------------------------------------- */
static void safe_copy(char *dst, size_t dst_max, const cJSON *item) {
  if (cJSON_IsString(item) && item->valuestring) {
    strlcpy(dst, item->valuestring, dst_max);
  }
}

/* -----------------------------------------------------------------------
 * config_manager_load
 * ----------------------------------------------------------------------- */
esp_err_t config_manager_load(void) {
  /* Garante que o SD card está montado antes de tentar carregar */
  app_storage_ensure_mounted();

  /* Protege barramento SPI */
  bsp_lvgl_lock(-1);

  struct stat st = {0};
  if (stat(SETTINGS_PATH, &st) != 0) {
    bsp_lvgl_unlock();
    ESP_LOGW(TAG, "config.txt not found (%s) — using fallback values", SETTINGS_PATH);
    if (s_config.ai_personality[0] == '\0') {
      strlcpy(s_config.ai_personality, s_default_personality,
              sizeof(s_config.ai_personality));
    }
    return ESP_ERR_NOT_FOUND;
  }

  if (st.st_size == 0 || st.st_size > JSON_READ_BUF_CAP) {
    bsp_lvgl_unlock();
    ESP_LOGW(TAG, "config.txt has unexpected size %ld — skipping", (long)st.st_size);
    return ESP_ERR_INVALID_SIZE;
  }

  /* Aloca buffer em PSRAM para não pressionar a DRAM */
  char *buf = (char *)heap_caps_malloc(JSON_READ_BUF_CAP,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (char *)malloc(JSON_READ_BUF_CAP);
  if (!buf) {
    bsp_lvgl_unlock();
    ESP_LOGE(TAG, "Failed to allocate JSON read buffer");
    return ESP_ERR_NO_MEM;
  }

  FILE *f = fopen(SETTINGS_PATH, "r");
  if (!f) {
    bsp_lvgl_unlock();
    free(buf);
    ESP_LOGE(TAG, "fopen failed: %s (errno %d)", SETTINGS_PATH, errno);
    return ESP_FAIL;
  }

  size_t read_len = fread(buf, 1, JSON_READ_BUF_CAP - 1, f);
  fclose(f);
  bsp_lvgl_unlock();
  buf[read_len] = '\0';

  if (read_len == 0) {
    free(buf);
    ESP_LOGW(TAG, "config.txt is empty — using fallback values");
    return ESP_ERR_NOT_FOUND;
  }

  /* Parse JSON */
  cJSON *root = cJSON_ParseWithLength(buf, read_len);
  free(buf);

  if (!root) {
    const char *err = cJSON_GetErrorPtr();
    ESP_LOGE(TAG, "JSON parse error near: %s", err ? err : "(unknown)");
    return ESP_FAIL;
  }

  /* wifi */
  const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
  if (wifi) {
    safe_copy(s_config.wifi_ssid, sizeof(s_config.wifi_ssid),
              cJSON_GetObjectItemCaseSensitive(wifi, "ssid"));
    safe_copy(s_config.wifi_pass, sizeof(s_config.wifi_pass),
              cJSON_GetObjectItemCaseSensitive(wifi, "password"));
  }

  /* ai */
  const cJSON *ai = cJSON_GetObjectItemCaseSensitive(root, "ai");
  if (ai) {
    safe_copy(s_config.ai_token, sizeof(s_config.ai_token),
              cJSON_GetObjectItemCaseSensitive(ai, "token"));

    const cJSON *pers = cJSON_GetObjectItemCaseSensitive(ai, "personality");
    if (cJSON_IsString(pers) && pers->valuestring && pers->valuestring[0]) {
      strlcpy(s_config.ai_personality, pers->valuestring,
              sizeof(s_config.ai_personality));
    } else {
      strlcpy(s_config.ai_personality, s_default_personality,
              sizeof(s_config.ai_personality));
    }

    const cJSON *base_url = cJSON_GetObjectItemCaseSensitive(ai, "base_url");
    if (cJSON_IsString(base_url) && base_url->valuestring &&
        base_url->valuestring[0]) {
      strlcpy(s_config.ai_base_url, base_url->valuestring,
              sizeof(s_config.ai_base_url));
    }

    const cJSON *model = cJSON_GetObjectItemCaseSensitive(ai, "model");
    if (cJSON_IsString(model) && model->valuestring && model->valuestring[0]) {
      strlcpy(s_config.ai_model, model->valuestring, sizeof(s_config.ai_model));
    }

    /* -------------------------------------------------------------------
     * Perfis: suporta novo formato (array) E formato legado (objeto nomeado)
     * ------------------------------------------------------------------- */
    const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(ai, "profiles");
    if (profiles) {
      if (cJSON_IsArray(profiles)) {
        /* Novo formato: array */
        int i = 0;
        const cJSON *p;
        cJSON_ArrayForEach(p, profiles) {
          if (i >= CONFIG_MAX_PROFILES) break;
          safe_copy(s_config.profiles[i].name,
                    sizeof(s_config.profiles[i].name),
                    cJSON_GetObjectItemCaseSensitive(p, "name"));
          safe_copy(s_config.profiles[i].prompt,
                    sizeof(s_config.profiles[i].prompt),
                    cJSON_GetObjectItemCaseSensitive(p, "prompt"));
          safe_copy(s_config.profiles[i].terms,
                    sizeof(s_config.profiles[i].terms),
                    cJSON_GetObjectItemCaseSensitive(p, "terms"));
          i++;
        }
        if (i > 0) s_config.num_profiles = (uint8_t)i;

      } else if (cJSON_IsObject(profiles)) {
        /* Formato legado: objeto com chaves nomeadas — migra automaticamente */
        ESP_LOGW(TAG, "Migrando perfis do formato legado (objeto -> array)");
        const char *keys[] = {"general", "agronomo", "engenheiro"};
        int loaded = 0;
        for (int i = 0; i < 3; i++) {
          const cJSON *p =
              cJSON_GetObjectItemCaseSensitive(profiles, keys[i]);
          if (p) {
            safe_copy(s_config.profiles[i].name,
                      sizeof(s_config.profiles[i].name),
                      cJSON_GetObjectItemCaseSensitive(p, "name"));
            safe_copy(s_config.profiles[i].prompt,
                      sizeof(s_config.profiles[i].prompt),
                      cJSON_GetObjectItemCaseSensitive(p, "prompt"));
            safe_copy(s_config.profiles[i].terms,
                      sizeof(s_config.profiles[i].terms),
                      cJSON_GetObjectItemCaseSensitive(p, "terms"));
            loaded++;
          }
        }
        if (loaded > 0) s_config.num_profiles = (uint8_t)loaded;
      }
    }

    /* expert_profile — valida contra num_profiles carregado */
    const cJSON *prof = cJSON_GetObjectItemCaseSensitive(ai, "expert_profile");
    if (cJSON_IsNumber(prof)) {
      int p_val = prof->valueint;
      if (p_val >= 0 && p_val < s_config.num_profiles) {
        s_config.expert_profile = (app_expert_profile_t)p_val;
      } else {
        s_config.expert_profile = 0;
      }
    }
  }

  /* hardware */
  const cJSON *hw = cJSON_GetObjectItemCaseSensitive(root, "hardware");
  if (hw) {
    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(hw, "volume");
    const cJSON *bri = cJSON_GetObjectItemCaseSensitive(hw, "brightness");
    if (cJSON_IsNumber(vol)) s_config.volume     = (uint8_t)vol->valueint;
    if (cJSON_IsNumber(bri)) s_config.brightness = (uint8_t)bri->valueint;
  }

  cJSON_Delete(root);

  s_config.loaded = true;
  ESP_LOGI(TAG, "Config loaded: SSID='%s' profiles=%d volume=%d brightness=%d",
           s_config.wifi_ssid, s_config.num_profiles,
           s_config.volume, s_config.brightness);
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * config_manager_save
 * ----------------------------------------------------------------------- */
esp_err_t config_manager_save(void) {
  esp_err_t mnt_ret = app_storage_ensure_mounted();
  if (mnt_ret != ESP_OK) {
    ESP_LOGE(TAG, "config_manager_save: Failed to mount SD card (%s)",
             esp_err_to_name(mnt_ret));
    return mnt_ret;
  }

  cJSON *root = cJSON_CreateObject();
  if (!root) return ESP_ERR_NO_MEM;

  /* wifi */
  cJSON *wifi = cJSON_CreateObject();
  cJSON_AddStringToObject(wifi, "ssid",     s_config.wifi_ssid);
  cJSON_AddStringToObject(wifi, "password", s_config.wifi_pass);
  cJSON_AddItemToObject(root, "wifi", wifi);

  /* ai */
  cJSON *ai = cJSON_CreateObject();
  cJSON_AddStringToObject(ai, "token",        s_config.ai_token);
  cJSON_AddStringToObject(ai, "personality",  s_config.ai_personality);
  cJSON_AddStringToObject(ai, "base_url",     s_config.ai_base_url);
  cJSON_AddStringToObject(ai, "model",        s_config.ai_model);
  cJSON_AddNumberToObject(ai, "expert_profile", (int)s_config.expert_profile);

  /* profiles — novo formato: array */
  cJSON *profiles = cJSON_CreateArray();
  for (int i = 0; i < s_config.num_profiles; i++) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name",   s_config.profiles[i].name);
    cJSON_AddStringToObject(p, "prompt", s_config.profiles[i].prompt);
    cJSON_AddStringToObject(p, "terms",  s_config.profiles[i].terms);
    cJSON_AddItemToArray(profiles, p);
  }
  cJSON_AddItemToObject(ai, "profiles", profiles);
  cJSON_AddItemToObject(root, "ai", ai);

  /* hardware */
  cJSON *hw = cJSON_CreateObject();
  cJSON_AddNumberToObject(hw, "volume",     s_config.volume);
  cJSON_AddNumberToObject(hw, "brightness", s_config.brightness);
  cJSON_AddItemToObject(root, "hardware", hw);

  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!json_str) {
    ESP_LOGE(TAG, "cJSON_PrintUnformatted returned NULL");
    return ESP_ERR_NO_MEM;
  }

  /* Protect SPI bus shared with LCD */
  bsp_lvgl_lock(-1);
  ESP_LOGI(TAG, "Opening config file for writing: %s", SETTINGS_PATH);
  FILE *f = fopen(SETTINGS_PATH, "w");
  if (!f) {
    int err = errno;
    bsp_lvgl_unlock();
    cJSON_free(json_str);
    ESP_LOGE(TAG, "fopen(%s, w) failed (errno %d: %s)", SETTINGS_PATH, err,
             strerror(err));
    return ESP_FAIL;
  }

  size_t json_len = strlen(json_str);
  size_t written  = fwrite(json_str, 1, json_len, f);

  fflush(f);
  int fd = fileno(f);
  if (fd >= 0) fsync(fd);
  fclose(f);
  bsp_lvgl_unlock();
  cJSON_free(json_str);

  if (written != json_len) {
    ESP_LOGE(TAG, "Incomplete write to %s (%u/%u bytes)", SETTINGS_PATH,
             (unsigned)written, (unsigned)json_len);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Config saved to %s (%u bytes, %d perfis)",
           SETTINGS_PATH, (unsigned)json_len, s_config.num_profiles);
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * config_manager_update_and_save
 * ----------------------------------------------------------------------- */
esp_err_t config_manager_update_and_save(const char *ssid, const char *pass,
                                         const char *ai_token,
                                         const char *ai_personality,
                                         const char *ai_base_url,
                                         const char *ai_model) {
  if (ssid) strlcpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid));
  if (pass && pass[0] != '\0')
    strlcpy(s_config.wifi_pass, pass, sizeof(s_config.wifi_pass));
  if (ai_token && ai_token[0] != '\0')
    strlcpy(s_config.ai_token, ai_token, sizeof(s_config.ai_token));
  if (ai_personality)
    strlcpy(s_config.ai_personality, ai_personality,
            sizeof(s_config.ai_personality));
  if (ai_base_url)
    strlcpy(s_config.ai_base_url, ai_base_url, sizeof(s_config.ai_base_url));
  if (ai_model)
    strlcpy(s_config.ai_model, ai_model, sizeof(s_config.ai_model));

  s_config.loaded = true;
  return config_manager_save();
}

/* -----------------------------------------------------------------------
 * config_manager_update_profiles
 * ----------------------------------------------------------------------- */
esp_err_t config_manager_update_profiles(uint8_t count,
                                         const app_profile_t *profiles) {
  if (!profiles || count == 0) return ESP_ERR_INVALID_ARG;
  if (count > CONFIG_MAX_PROFILES) count = CONFIG_MAX_PROFILES;

  s_config.num_profiles = count;
  for (uint8_t i = 0; i < count; i++) {
    strlcpy(s_config.profiles[i].name, profiles[i].name,
            sizeof(s_config.profiles[i].name));
    strlcpy(s_config.profiles[i].prompt, profiles[i].prompt,
            sizeof(s_config.profiles[i].prompt));
    strlcpy(s_config.profiles[i].terms, profiles[i].terms,
            sizeof(s_config.profiles[i].terms));
  }

  /* Garante que o perfil ativo não ultrapassa o novo limite */
  if (s_config.expert_profile >= count) {
    s_config.expert_profile = 0;
  }

  s_config.loaded = true;
  return config_manager_save();
}
