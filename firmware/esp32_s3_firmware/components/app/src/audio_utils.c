#include "audio_utils.h"
#include <math.h>

float audio_calculate_rms(const int16_t *samples, size_t count) {
  if (count == 0)
    return 0.0f;

  // First pass: calculate mean for DC offset removal
  double sum = 0;
  for (size_t i = 0; i < count; i++) {
    sum += (double)samples[i];
  }
  double mean = sum / (double)count;

  // Second pass: sum squares relative to mean (variance)
  double sum_sq = 0;
  for (size_t i = 0; i < count; i++) {
    double s = (double)samples[i] - mean;
    sum_sq += s * s;
  }

  return (float)sqrt(sum_sq / (double)count);
}

void audio_apply_highpass(int16_t *samples, size_t count, float fc_hz,
                          float fs_hz) {
  if (!samples || count == 0 || fc_hz <= 0.0f || fs_hz <= 0.0f) {
    return;
  }

  /*
   * 1st-order Butterworth high-pass (bilinear transform):
   *   RC    = 1 / (2 * PI * fc)
   *   alpha = RC / (RC + dt)     where dt = 1 / fs
   *
   * Difference equation:
   *   y[n] = alpha * (y[n-1] + x[n] - x[n-1])
   */
  const float dt = 1.0f / fs_hz;
  const float rc = 1.0f / (2.0f * (float)M_PI * fc_hz);
  const float alpha = rc / (rc + dt);

  float prev_x = (float)samples[0];
  float prev_y = (float)samples[0];

  for (size_t i = 1; i < count; i++) {
    float x = (float)samples[i];
    float y = alpha * (prev_y + x - prev_x);

    /* Clamp to int16 range */
    if (y > 32767.0f) {
      y = 32767.0f;
    } else if (y < -32768.0f) {
      y = -32768.0f;
    }

    samples[i] = (int16_t)y;
    prev_x = x;
    prev_y = y;
  }
}
