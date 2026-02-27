#include "bsp.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "bsp_battery";

#define BATTERY_ADC_UNIT ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_4
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc1_handle = NULL;
static adc_cali_handle_t s_adc1_cali_handle = NULL;
static bool s_do_calibration = false;

#define BATTERY_NUM_SAMPLES 10
static float s_smoothed_voltage = -1.0f;
static int s_last_percent = -1;

static bool battery_adc_calibration_init(adc_unit_t unit, adc_channel_t channel,
                                         adc_atten_t atten,
                                         adc_cali_handle_t *out_handle) {
  adc_cali_handle_t handle = NULL;
  esp_err_t ret = ESP_FAIL;
  bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  if (!calibrated) {
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
    if (ret == ESP_OK) {
      calibrated = true;
    }
  }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  if (!calibrated) {
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
    if (ret == ESP_OK) {
      calibrated = true;
    }
  }
#endif

  *out_handle = handle;
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "ADC Calibration Success");
  } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
    ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
  } else {
    ESP_LOGE(TAG, "Invalid arg or no memory for calibration");
  }

  return calibrated;
}

esp_err_t bsp_battery_init(void) {
  if (s_adc1_handle != NULL) {
    return ESP_OK; // Already initialized
  }

  adc_oneshot_unit_init_cfg_t init_config1 = {
      .unit_id = BATTERY_ADC_UNIT,
  };
  esp_err_t err = adc_oneshot_new_unit(&init_config1, &s_adc1_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init ADC unit");
    return err;
  }

  adc_oneshot_chan_cfg_t config = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = BATTERY_ADC_ATTEN,
  };
  err = adc_oneshot_config_channel(s_adc1_handle, BATTERY_ADC_CHANNEL, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to config ADC channel");
    return err;
  }

  s_do_calibration =
      battery_adc_calibration_init(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL,
                                   BATTERY_ADC_ATTEN, &s_adc1_cali_handle);

  ESP_LOGI(TAG, "Battery ADC initialized");
  return ESP_OK;
}

esp_err_t bsp_battery_get_percent(int *out_percent) {
  if (s_adc1_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (out_percent == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  int raw_sum = 0;
  int valid_samples = 0;

  for (int i = 0; i < BATTERY_NUM_SAMPLES; i++) {
    int adc_raw = 0;
    esp_err_t err =
        adc_oneshot_read(s_adc1_handle, BATTERY_ADC_CHANNEL, &adc_raw);
    if (err == ESP_OK) {
      raw_sum += adc_raw;
      valid_samples++;
    }
  }

  if (valid_samples == 0) {
    ESP_LOGE(TAG, "Failed to read ADC");
    return ESP_FAIL;
  }

  int avg_raw = raw_sum / valid_samples;

  float voltage = 0.0f;
  if (s_do_calibration) {
    int voltage_int = 0;
    esp_err_t err =
        adc_cali_raw_to_voltage(s_adc1_cali_handle, avg_raw, &voltage_int);
    if (err == ESP_OK) {
      // According to Waveshare example, multiplier is 3.0 for the voltage
      // divider
      voltage = (voltage_int / 1000.0f) * 3.0f;
    } else {
      return err;
    }
  } else {
    // Fallback rough estimation if no calibration
    // ADC 0-4095 over 3.3v reference
    float pin_voltage = ((float)avg_raw / 4095.0f) * 3.3f;
    voltage = pin_voltage * 3.0f;
  }

  if (s_smoothed_voltage < 0.0f) {
    s_smoothed_voltage = voltage;
  } else {
    // Exponential moving average: alpha = 0.1
    s_smoothed_voltage = (s_smoothed_voltage * 0.90f) + (voltage * 0.10f);
  }

  // Map valid li-ion voltage to 0-100%
  // Adjusted V_MAX to 4.00f due to typical ESP32 ADC underreading 
  // and natural voltage drop disconnected from charger.
  const float V_MAX = 4.00f;
  const float V_MIN = 3.30f;

  int new_percent = 0;
  if (s_smoothed_voltage >= V_MAX) {
    new_percent = 100;
  } else if (s_smoothed_voltage <= V_MIN) {
    new_percent = 0;
  } else {
    new_percent =
        (int)(((s_smoothed_voltage - V_MIN) / (V_MAX - V_MIN)) * 100.0f);
  }

  if (s_last_percent == -1) {
    s_last_percent = new_percent;
  } else if (new_percent > s_last_percent + 1 ||
             new_percent < s_last_percent - 1) {
    s_last_percent = new_percent;
  }

  *out_percent = s_last_percent;

  return ESP_OK;
}
