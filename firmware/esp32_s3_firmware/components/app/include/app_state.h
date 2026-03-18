#pragma once
#include <stdint.h>

typedef enum {
  APP_STATE_IDLE = 0,
  APP_STATE_SELECTING_MODE,
  APP_STATE_LISTENING,
  APP_STATE_TRANSCRIBING,
  APP_STATE_THINKING,
  APP_STATE_SHOWING_RESPONSE,
  APP_STATE_ERROR,
  APP_STATE_BOOTING,
} app_state_t;

/* Índice do perfil ativo: 0 .. (num_profiles - 1) */
typedef uint8_t app_expert_profile_t;
