#pragma once

#include <stddef.h>
#include <stdint.h>


/**
 * @brief Calculate the Root Mean Square (RMS) of a signal.
 *
 * @param samples Pointer to 16-bit PCM samples.
 * @param count Number of samples (not bytes).
 * @return float The RMS value.
 */
float audio_calculate_rms(const int16_t *samples, size_t count);
