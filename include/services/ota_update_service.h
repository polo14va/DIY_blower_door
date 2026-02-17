#ifndef OTA_UPDATE_SERVICE_H
#define OTA_UPDATE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_UPDATE_VERSION_LABEL_MAX_LEN 24u
#define OTA_UPDATE_ERROR_TEXT_MAX_LEN 64u

typedef enum {
  OTA_UPDATE_STATE_IDLE = 0,
  OTA_UPDATE_STATE_RECEIVING,
  OTA_UPDATE_STATE_READY,
  OTA_UPDATE_STATE_APPLYING,
  OTA_UPDATE_STATE_ERROR,
} ota_update_state_t;

typedef enum {
  OTA_UPDATE_RESULT_OK = 0,
  OTA_UPDATE_RESULT_BUSY,
  OTA_UPDATE_RESULT_INVALID_ARGUMENT,
  OTA_UPDATE_RESULT_INVALID_STATE,
  OTA_UPDATE_RESULT_SIZE_OUT_OF_RANGE,
  OTA_UPDATE_RESULT_OFFSET_MISMATCH,
  OTA_UPDATE_RESULT_FLASH_IO,
  OTA_UPDATE_RESULT_IMAGE_INVALID,
  OTA_UPDATE_RESULT_INTERNAL,
} ota_update_result_t;

typedef struct {
  ota_update_state_t state;
  uint32_t expected_size_bytes;
  uint32_t received_size_bytes;
  uint32_t expected_crc32;
  uint32_t computed_crc32;
  bool apply_task_active;
  char staged_version[OTA_UPDATE_VERSION_LABEL_MAX_LEN];
  char last_error[OTA_UPDATE_ERROR_TEXT_MAX_LEN];
} ota_update_status_t;

void ota_update_service_init(void);
const char *ota_update_service_get_firmware_version(void);

ota_update_result_t ota_update_service_begin(uint32_t image_size_bytes,
                                             uint32_t expected_crc32,
                                             const char *staged_version);
ota_update_result_t ota_update_service_write_chunk(uint32_t offset,
                                                   const uint8_t *chunk_data,
                                                   size_t chunk_length);
ota_update_result_t ota_update_service_finish(void);
ota_update_result_t ota_update_service_request_apply_async(void);

void ota_update_service_get_status(ota_update_status_t *out_status);
const char *ota_update_service_state_name(ota_update_state_t state);
const char *ota_update_result_name(ota_update_result_t result);

#endif
