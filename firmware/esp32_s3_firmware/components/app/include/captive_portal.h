#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicia o modo de configuração via Captive Portal.
 *
 * Esta função:
 *   1. Suspende as tasks de câmera e áudio (se ativas).
 *   2. Instrui o C6 a subir um SoftAP "Assistant-Config-P4".
 *   3. Inicia um esp_http_server com a página de configuração.
 *   4. Exibe o IP do portal no display (LVGL).
 *
 * A função bloqueia internamente num loop aguardando que o usuário salve
 * a configuração via POST. Após salvar, dispara esp_restart().
 *
 * Deve ser chamada somente quando o long-press de 10 s for detectado.
 *
 * @return Normalmente não retorna (reinicia o dispositivo após salvar).
 *         Retorna ESP_FAIL se não for possível iniciar o AP/HTTP server.
 */
esp_err_t captive_portal_start(void);

#ifdef __cplusplus
}
#endif
