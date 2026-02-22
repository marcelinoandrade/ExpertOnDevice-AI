#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Tamanhos máximos dos campos de configuração
 * ----------------------------------------------------------------------- */
#define CONFIG_WIFI_SSID_MAX 64
#define CONFIG_WIFI_PASS_MAX 64
#define CONFIG_AI_TOKEN_MAX 220 /* Tokens OpenAI svcacct chegam a ~180 chars   \
                                 */
#define CONFIG_AI_PERSONALITY_MAX 256

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

  /* Hardware */
  uint8_t volume;     /* 0–100 */
  uint8_t brightness; /* 0–100 */

  /* Estado interno */
  bool loaded; /* true se o arquivo foi lido com sucesso */
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
 * @brief Carrega /sdcard/data/settings.json para a struct interna.
 *
 * Deve ser chamado APÓS o SD card estar montado.
 * Se o arquivo não existir, mantém os valores de fallback (secret.h).
 *
 * @return ESP_OK se o arquivo foi lido e parseado com sucesso.
 *         ESP_ERR_NOT_FOUND se o arquivo não existe (usando fallback).
 *         Outros códigos em caso de erro de I/O ou parse.
 */
esp_err_t config_manager_load(void);

/**
 * @brief Salva a struct atual em /sdcard/data/settings.json.
 *
 * Sobrescreve o arquivo existente.
 *
 * @return ESP_OK em caso de sucesso.
 */
esp_err_t config_manager_save(void);

/**
 * @brief Atualiza Wi-Fi, token e personalidade, depois chama
 * config_manager_save().
 *
 * Conveniência para o handler do Captive Portal.
 */
esp_err_t config_manager_update_and_save(const char *ssid, const char *pass,
                                         const char *ai_token,
                                         const char *ai_personality);

#ifdef __cplusplus
}
#endif
