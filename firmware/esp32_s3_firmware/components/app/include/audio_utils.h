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

/**
 * @brief Apply a 1st-order Butterworth IIR high-pass filter in-place.
 *
 * Removes low-frequency noise (hum, rumble, mic vibration) to improve
 * speech intelligibility for AI transcription.  Latency: 1 sample.
 *
 * @param samples  Pointer to 16-bit PCM samples (modified in-place).
 * @param count    Number of samples (not bytes).
 * @param fc_hz    Cut-off frequency in Hz (e.g. 100.0f).
 * @param fs_hz    Sample rate in Hz (e.g. 16000.0f).
 */
void audio_apply_highpass(int16_t *samples, size_t count, float fc_hz,
                          float fs_hz);
