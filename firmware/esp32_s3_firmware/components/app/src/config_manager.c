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

#include "secret.h" /* fallback: SECRET_WIFI_SSID, SECRET_OPENAI_API_KEY, etc. */

static const char *TAG = "config_mgr";

#define SETTINGS_PATH "/sdcard/data/config.txt"
#define SETTINGS_DIR "/sdcard/data"
#define JSON_READ_BUF_CAP                                                      \
  (8 * 1024) /* 8 KB — alinhado a pior caso razoável                        \
              */

/* -----------------------------------------------------------------------
 * Singleton — valores default (fallback quando config.txt não existe)
 * ----------------------------------------------------------------------- */
static app_config_t s_config = {
    .wifi_ssid = SECRET_WIFI_SSID,
    .wifi_pass = SECRET_WIFI_PASS,
    .ai_token = SECRET_OPENAI_API_KEY,
    /* ai_personality inicializada em config_manager_load() se não houver JSON
     */
    .ai_personality = "",
    .ai_base_url = "https://api.openai.com/v1/chat/completions",
    .ai_model = "gpt-4o", /* O modelo padrão de visão. Áudio usa preview
                             estático se não houver endpoint de áudio */
    .expert_profile = APP_EXPERT_PROFILE_GENERAL,

    .profile_general_name = "Geral",
    .profile_general_prompt =
        "Perfil ativo: Geral. Responda de forma pratica e direta. Identifique "
        "objetos, leia textos, descreva cenas. Sempre tente ser util.",
    .profile_general_terms = "NPK, fusarium, set-point, vazao",

    .profile_agronomo_name = "Agronomo",
    .profile_agronomo_prompt =
        "Perfil ativo: Agronomo. Ao ver plantas, identifique especie se "
        "possivel, avalie saude (cor das folhas, manchas, pragas visiveis). "
        "Sugira diagnostico provavel e acao imediata (ex: aplicar fungicida, "
        "irrigar, podar).",
    .profile_agronomo_terms =
        "NPK, fusarium, clorose, necrose, oidio, ferrugem, praga, fungicida, "
        "herbicida, irrigacao, vazao",

    .profile_engenheiro_name = "Engenheiro",
    .profile_engenheiro_prompt =
        "Perfil ativo: Engenheiro. Ao ver circuitos/equipamentos, identifique "
        "componentes, leia valores (resistores, capacitores), note estado de "
        "LEDs, conexoes soltas ou queimadas. Sugira ponto de verificacao.",
    .profile_engenheiro_terms =
        "set-point, vazao, corrente, tensao, curto, rele, inversor, sensor, "
        "atuador, LED vermelho, falha",

    .volume = 70,
    .brightness = 85,
    .audio_rms_threshold = 1000.0f,
    .loaded = false,
};

/* Personalidade padrão usada quando o JSON não contém o campo */
static const char *s_default_personality =
    "Voce e um assistente inteligente e conciso.";

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

  /* Verifica se o arquivo existe */
  struct stat st = {0};
  if (stat(SETTINGS_PATH, &st) != 0) {
    bsp_lvgl_unlock();
    ESP_LOGW(TAG, "config.txt not found (%s) — using fallback values",
             SETTINGS_PATH);
    /* Garante que o campo de personalidade tenha o valor padrão */
    if (s_config.ai_personality[0] == '\0') {
      strlcpy(s_config.ai_personality, s_default_personality,
              sizeof(s_config.ai_personality));
    }
    return ESP_ERR_NOT_FOUND;
  }

  if (st.st_size == 0 || st.st_size > JSON_READ_BUF_CAP) {
    bsp_lvgl_unlock();
    ESP_LOGW(TAG, "config.txt has unexpected size %ld — skipping",
             (long)st.st_size);
    return ESP_ERR_INVALID_SIZE;
  }

  /* Aloca buffer em PSRAM para não pressionar a DRAM */
  char *buf = (char *)heap_caps_malloc(JSON_READ_BUF_CAP,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    /* Fallback: DRAM */
    buf = (char *)malloc(JSON_READ_BUF_CAP);
  }
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

    const cJSON *prof = cJSON_GetObjectItemCaseSensitive(ai, "expert_profile");
    if (cJSON_IsNumber(prof)) {
      int p_val = prof->valueint;
      if (p_val >= 0 && p_val <= 2) {
        s_config.expert_profile = (app_expert_profile_t)p_val;
      }
    }

    const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(ai, "profiles");
    if (profiles) {
      const cJSON *gen = cJSON_GetObjectItemCaseSensitive(profiles, "general");
      if (gen) {
        safe_copy(s_config.profile_general_name,
                  sizeof(s_config.profile_general_name),
                  cJSON_GetObjectItemCaseSensitive(gen, "name"));
        safe_copy(s_config.profile_general_prompt,
                  sizeof(s_config.profile_general_prompt),
                  cJSON_GetObjectItemCaseSensitive(gen, "prompt"));
        safe_copy(s_config.profile_general_terms,
                  sizeof(s_config.profile_general_terms),
                  cJSON_GetObjectItemCaseSensitive(gen, "terms"));
      }
      const cJSON *agro =
          cJSON_GetObjectItemCaseSensitive(profiles, "agronomo");
      if (agro) {
        safe_copy(s_config.profile_agronomo_name,
                  sizeof(s_config.profile_agronomo_name),
                  cJSON_GetObjectItemCaseSensitive(agro, "name"));
        safe_copy(s_config.profile_agronomo_prompt,
                  sizeof(s_config.profile_agronomo_prompt),
                  cJSON_GetObjectItemCaseSensitive(agro, "prompt"));
        safe_copy(s_config.profile_agronomo_terms,
                  sizeof(s_config.profile_agronomo_terms),
                  cJSON_GetObjectItemCaseSensitive(agro, "terms"));
      }
      const cJSON *eng =
          cJSON_GetObjectItemCaseSensitive(profiles, "engenheiro");
      if (eng) {
        safe_copy(s_config.profile_engenheiro_name,
                  sizeof(s_config.profile_engenheiro_name),
                  cJSON_GetObjectItemCaseSensitive(eng, "name"));
        safe_copy(s_config.profile_engenheiro_prompt,
                  sizeof(s_config.profile_engenheiro_prompt),
                  cJSON_GetObjectItemCaseSensitive(eng, "prompt"));
        safe_copy(s_config.profile_engenheiro_terms,
                  sizeof(s_config.profile_engenheiro_terms),
                  cJSON_GetObjectItemCaseSensitive(eng, "terms"));
      }
    }
  }

  /* hardware */
  const cJSON *hw = cJSON_GetObjectItemCaseSensitive(root, "hardware");
  if (hw) {
    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(hw, "volume");
    const cJSON *bri = cJSON_GetObjectItemCaseSensitive(hw, "brightness");
    const cJSON *rms = cJSON_GetObjectItemCaseSensitive(hw, "rms_threshold");
    if (cJSON_IsNumber(vol)) {
      s_config.volume = (uint8_t)vol->valueint;
    }
    if (cJSON_IsNumber(bri)) {
      s_config.brightness = (uint8_t)bri->valueint;
    }
    if (cJSON_IsNumber(rms)) {
      s_config.audio_rms_threshold = (float)rms->valuedouble;
    }
  }

  cJSON_Delete(root);

  s_config.loaded = true;
  ESP_LOGI(TAG, "Configuration loaded: SSID='%s', volume=%d, brightness=%d",
           s_config.wifi_ssid, s_config.volume, s_config.brightness);
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * config_manager_save
 * ----------------------------------------------------------------------- */
esp_err_t config_manager_save(void) {
  /* Garante que o SD card está montado (especialmente para o Captive Portal) */
  esp_err_t mnt_ret = app_storage_ensure_mounted();
  if (mnt_ret != ESP_OK) {
    ESP_LOGE(TAG, "config_manager_save: Failed to mount SD card (%s)",
             esp_err_to_name(mnt_ret));
    return mnt_ret;
  }

  /* Nota: app_storage_ensure_mounted já chamou ensure_directory_structure() */

  cJSON *root = cJSON_CreateObject();
  if (!root)
    return ESP_ERR_NO_MEM;

  /* wifi */
  cJSON *wifi = cJSON_CreateObject();
  cJSON_AddStringToObject(wifi, "ssid", s_config.wifi_ssid);
  cJSON_AddStringToObject(wifi, "password", s_config.wifi_pass);
  cJSON_AddItemToObject(root, "wifi", wifi);

  /* ai */
  cJSON *ai = cJSON_CreateObject();
  cJSON_AddStringToObject(ai, "token", s_config.ai_token);
  cJSON_AddStringToObject(ai, "personality", s_config.ai_personality);
  cJSON_AddStringToObject(ai, "base_url", s_config.ai_base_url);
  cJSON_AddStringToObject(ai, "model", s_config.ai_model);
  cJSON_AddNumberToObject(ai, "expert_profile", (int)s_config.expert_profile);

  cJSON *profiles = cJSON_CreateObject();

  cJSON *gen = cJSON_CreateObject();
  cJSON_AddStringToObject(gen, "name", s_config.profile_general_name);
  cJSON_AddStringToObject(gen, "prompt", s_config.profile_general_prompt);
  cJSON_AddStringToObject(gen, "terms", s_config.profile_general_terms);
  cJSON_AddItemToObject(profiles, "general", gen);

  cJSON *agro = cJSON_CreateObject();
  cJSON_AddStringToObject(agro, "name", s_config.profile_agronomo_name);
  cJSON_AddStringToObject(agro, "prompt", s_config.profile_agronomo_prompt);
  cJSON_AddStringToObject(agro, "terms", s_config.profile_agronomo_terms);
  cJSON_AddItemToObject(profiles, "agronomo", agro);

  cJSON *eng = cJSON_CreateObject();
  cJSON_AddStringToObject(eng, "name", s_config.profile_engenheiro_name);
  cJSON_AddStringToObject(eng, "prompt", s_config.profile_engenheiro_prompt);
  cJSON_AddStringToObject(eng, "terms", s_config.profile_engenheiro_terms);
  cJSON_AddItemToObject(profiles, "engenheiro", eng);

  cJSON_AddItemToObject(ai, "profiles", profiles);

  cJSON_AddItemToObject(root, "ai", ai);

  /* hardware */
  cJSON *hw = cJSON_CreateObject();
  cJSON_AddNumberToObject(hw, "volume", s_config.volume);
  cJSON_AddNumberToObject(hw, "brightness", s_config.brightness);
  cJSON_AddNumberToObject(hw, "rms_threshold", s_config.audio_rms_threshold);
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
  size_t written = fwrite(json_str, 1, json_len, f);

  /* Flush and close */
  fflush(f);
  int fd = fileno(f);
  if (fd >= 0) {
    fsync(fd);
  }
  fclose(f);
  bsp_lvgl_unlock();
  cJSON_free(json_str);

  if (written != json_len) {
    ESP_LOGE(TAG, "Incomplete write to %s (%u/%u bytes)", SETTINGS_PATH,
             (unsigned)written, (unsigned)json_len);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Configuration saved to %s (%u bytes)", SETTINGS_PATH,
           (unsigned)json_len);
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * config_manager_update_and_save
 * ----------------------------------------------------------------------- */
esp_err_t config_manager_update_and_save(const char *ssid, const char *pass,
                                         const char *ai_token,
                                         const char *ai_personality,
                                         const char *ai_base_url,
                                         const char *ai_model,
                                         float audio_rms_threshold) {
  if (ssid)
    strlcpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid));
  if (pass)
    strlcpy(s_config.wifi_pass, pass, sizeof(s_config.wifi_pass));
  if (ai_token)
    strlcpy(s_config.ai_token, ai_token, sizeof(s_config.ai_token));
  if (ai_personality)
    strlcpy(s_config.ai_personality, ai_personality,
            sizeof(s_config.ai_personality));
  if (ai_base_url)
    strlcpy(s_config.ai_base_url, ai_base_url, sizeof(s_config.ai_base_url));
  if (ai_model)
    strlcpy(s_config.ai_model, ai_model, sizeof(s_config.ai_model));

  s_config.audio_rms_threshold = audio_rms_threshold;

  s_config.loaded = true;
  return config_manager_save();
}

esp_err_t config_manager_update_profiles(
    const char *gen_name, const char *gen_prompt, const char *gen_terms,
    const char *agro_name, const char *agro_prompt, const char *agro_terms,
    const char *eng_name, const char *eng_prompt, const char *eng_terms) {
  if (gen_name)
    strlcpy(s_config.profile_general_name, gen_name,
            sizeof(s_config.profile_general_name));
  if (gen_prompt)
    strlcpy(s_config.profile_general_prompt, gen_prompt,
            sizeof(s_config.profile_general_prompt));
  if (gen_terms)
    strlcpy(s_config.profile_general_terms, gen_terms,
            sizeof(s_config.profile_general_terms));

  if (agro_name)
    strlcpy(s_config.profile_agronomo_name, agro_name,
            sizeof(s_config.profile_agronomo_name));
  if (agro_prompt)
    strlcpy(s_config.profile_agronomo_prompt, agro_prompt,
            sizeof(s_config.profile_agronomo_prompt));
  if (agro_terms)
    strlcpy(s_config.profile_agronomo_terms, agro_terms,
            sizeof(s_config.profile_agronomo_terms));

  if (eng_name)
    strlcpy(s_config.profile_engenheiro_name, eng_name,
            sizeof(s_config.profile_engenheiro_name));
  if (eng_prompt)
    strlcpy(s_config.profile_engenheiro_prompt, eng_prompt,
            sizeof(s_config.profile_engenheiro_prompt));
  if (eng_terms)
    strlcpy(s_config.profile_engenheiro_terms, eng_terms,
            sizeof(s_config.profile_engenheiro_terms));

  s_config.loaded = true;
  return config_manager_save();
}
