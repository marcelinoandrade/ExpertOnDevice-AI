#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize storage subsystem
 *
 * This function initializes the SD card monitoring and creates
 * the directory structure if needed.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_storage_init(void);

/**
 * @brief Save JPEG image to SD card
 *
 * Saves a JPEG image to /sdcard/media/images/ with timestamp filename.
 * This is a synchronous operation (blocks until write completes).
 *
 * @param jpeg_data Pointer to JPEG data
 * @param jpeg_len Length of JPEG data in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_storage_save_image(const uint8_t *jpeg_data, size_t jpeg_len);

/**
 * @brief Check if SD card is ready for operations
 *
 * @return true if SD card is present and mounted, false otherwise
 */
bool app_storage_is_ready(void);

/**
 * @brief Mount SD card after camera initialization
 *
 * This function should be called after the camera successfully initializes.
 * At this point, camera buffers are allocated but memory is not yet
 * fragmented by audio/IA processing - optimal time to mount SD card.
 */
void app_storage_mount_after_camera_init(void);

/**
 * @brief Queue JPEG image for saving (buffered in PSRAM)
 *
 * This function copies the JPEG data to PSRAM buffer and queues it for
 * saving after the AI interaction completes. This avoids SDMMC/DMA conflicts
 * during network communication.
 *
 * @param jpeg_data Pointer to JPEG data (will be copied to PSRAM)
 * @param jpeg_len Length of JPEG data in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_storage_queue_image(const uint8_t *jpeg_data, size_t jpeg_len);

/**
 * @brief Process queued images and audio and save to SD card
 *
 * This function should be called after AI interaction completes.
 * It mounts SD card (if needed) and saves all queued media.
 * This avoids DMA conflicts during network communication.
 */
void app_storage_process_queue(void);

/**
 * @brief Notify storage system that user interaction occurred
 *
 * This resets the inactivity timer. The system will save queued images
 * after a period of inactivity (default: 10 seconds).
 */
void app_storage_notify_interaction(void);

/**
 * @brief Get number of queued images waiting to be saved
 *
 * @return Number of images in queue (0-5)
 */
int app_storage_get_queue_count(void);

/**
 * @brief Indicates whether storage subsystem is currently mounting/saving
 *
 * Used by app loop to avoid camera preview activity during SD operations.
 */
bool app_storage_is_busy(void);

/**
 * @brief Save raw PCM16 audio as WAV file to SD card
 *
 * Saves recorded audio to /sdcard/media/audio/R_HHMMSS.WAV.
 * This is a synchronous bypass; normally audio should be queued.
 *
 * @param pcm_data   Pointer to raw 16-bit PCM samples (mono)
 * @param pcm_bytes  Number of bytes (not samples) of PCM data
 * @param sample_rate_hz Sample rate (e.g. 16000)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_storage_save_audio(const uint8_t *pcm_data, size_t pcm_bytes,
                                 uint32_t sample_rate_hz);

/**
 * @brief Queue recording audio for saving (buffered in PSRAM)
 *
 * This function copies the audio data to PSRAM buffer and queues it for
 * saving later. This avoids SDMMC/DMA conflicts during AI voice recording.
 *
 * @param pcm_data Pointer to PCM data (will be copied to PSRAM)
 * @param pcm_bytes Length of PCM data in bytes
 * @param sample_rate_hz Sample rate for the WAV header
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_storage_queue_audio(const uint8_t *pcm_data, size_t pcm_bytes,
                                  uint32_t sample_rate_hz);

/**
 * @brief Append an interaction entry to the daily chat log on SD card
 *
 * Appends a timestamped line to /sdcard/logs/chat/CHAT_YYYYMMDD.txt.
 * Creates the file if it does not yet exist.
 *
 * @param mode_label  Short label for the interaction mode (e.g. "AUDIO_TEXT")
 * @param ai_response The text response received from the AI
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_storage_save_chat_log(const char *mode_label,
                                    const char *ai_response);

/**
 * @brief Force mount SD card if not already mounted
 *
 * This can be used in configuration mode to ensure the SD card is available
 * for saving config.txt.
 *
 * @return ESP_OK if mounted or already mounted, error code otherwise
 */
esp_err_t app_storage_ensure_mounted(void);