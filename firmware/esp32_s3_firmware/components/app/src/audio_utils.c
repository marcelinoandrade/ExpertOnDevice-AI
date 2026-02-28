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
