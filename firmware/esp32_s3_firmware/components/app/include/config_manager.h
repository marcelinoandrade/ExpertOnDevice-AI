#pragma once

#include "app_state.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Tamanhos máximos dos campos de configuração
 * ----------------------------------------------------------------------- */
#define CONFIG_WIFI_SSID_MAX      64
#define CONFIG_WIFI_PASS_MAX      64
#define CONFIG_AI_TOKEN_MAX       220
#define CONFIG_AI_PERSONALITY_MAX 256
#define CONFIG_AI_BASE_URL_MAX    128
#define CONFIG_AI_MODEL_MAX       64
#define CONFIG_PROFILE_PROMPT_MAX 512
#define CONFIG_PROFILE_TERMS_MAX  256
#define CONFIG_PROFILE_NAME_MAX   32
#define CONFIG_MAX_PROFILES       6   /* máximo de perfis dinâmicos */

/* -----------------------------------------------------------------------
 * Estrutura de um perfil especialista
 * ----------------------------------------------------------------------- */
typedef struct {
  char name[CONFIG_PROFILE_NAME_MAX];
  char prompt[CONFIG_PROFILE_PROMPT_MAX];
  char terms[CONFIG_PROFILE_TERMS_MAX];
} app_profile_t;

/* -----------------------------------------------------------------------
 * Estrutura principal de configuração
 * ----------------------------------------------------------------------- */
typedef struct {
  /* Wi-Fi */
  char wifi_ssid[CONFIG_WIFI_SSID_MAX];
  char wifi_pass[CONFIG_WIFI_PASS_MAX];

  /* IA */
  char ai_token[CONFIG_AI_TOKEN_MAX];
  char ai_personality[CONFIG_AI_PERSONALITY_MAX];
  char ai_base_url[CONFIG_AI_BASE_URL_MAX];
  char ai_model[CONFIG_AI_MODEL_MAX];
  app_expert_profile_t expert_profile; /* índice 0..num_profiles-1 */

  /* Perfis Especialistas — dinâmicos */
  uint8_t       num_profiles;                   /* 1..CONFIG_MAX_PROFILES */
  app_profile_t profiles[CONFIG_MAX_PROFILES];  /* array de perfis */

  /* Hardware */
  uint8_t volume;     /* 0–100 */
  uint8_t brightness; /* 0–100 */

  /* Estado interno */
  bool loaded;
} app_config_t;

/* -----------------------------------------------------------------------
 * Singleton de acesso global
 * ----------------------------------------------------------------------- */
/**
 * @brief Retorna o ponteiro para a configuração global.
 *        Nunca retorna NULL (aponta para struct estático interno).
 */
app_config_t *config_manager_get(void);

/* -----------------------------------------------------------------------
 * API pública
 * ----------------------------------------------------------------------- */

/**
 * @brief Carrega /sdcard/data/config.txt para a struct interna.
 */
esp_err_t config_manager_load(void);

/**
 * @brief Salva a struct atual em /sdcard/data/config.txt.
 */
esp_err_t config_manager_save(void);

/**
 * @brief Atualiza Wi-Fi, token e personalidade, depois chama config_manager_save().
 */
esp_err_t config_manager_update_and_save(const char *ssid, const char *pass,
                                         const char *ai_token,
                                         const char *ai_personality,
                                         const char *ai_base_url,
                                         const char *ai_model);

/**
 * @brief Atualiza o array de perfis especialistas na memória e salva.
 * @param count   Número de perfis ativos (1..CONFIG_MAX_PROFILES).
 * @param profiles Array com os perfis preenchidos.
 */
esp_err_t config_manager_update_profiles(uint8_t count,
                                         const app_profile_t *profiles);

#ifdef __cplusplus
}
#endif
