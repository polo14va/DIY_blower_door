#include "services/ota_update_service.h"

#include "FreeRTOS.h"
#include "app/app_config.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "semphr.h"
#include "task.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern const uint8_t __flash_binary_end;

typedef struct {
  SemaphoreHandle_t mutex;
  bool initialized;
  ota_update_state_t state;
  uint32_t expected_size_bytes;
  uint32_t received_size_bytes;
  uint32_t expected_crc32;
  uint32_t computed_crc32;
  uint32_t running_crc32;
  uint32_t next_expected_offset;
  uint32_t staged_programmed_size_bytes;
  size_t page_fill_bytes;
  uint8_t page_buffer[FLASH_PAGE_SIZE];
  TaskHandle_t apply_task_handle;
  char staged_version[OTA_UPDATE_VERSION_LABEL_MAX_LEN];
  char last_error[OTA_UPDATE_ERROR_TEXT_MAX_LEN];
} ota_update_context_t;

static ota_update_context_t g_context;
static volatile uint32_t g_apply_image_size_bytes = 0u;
static uint8_t g_apply_sector_buffer[FLASH_SECTOR_SIZE];

static bool ota_layout_is_valid(void) {
  const uintptr_t current_binary_end =
      (uintptr_t)(&__flash_binary_end) - (uintptr_t)XIP_BASE;
  const uint32_t staging_end =
      APP_OTA_STAGING_OFFSET_BYTES + APP_OTA_STAGING_SIZE_BYTES;

  if ((APP_OTA_STAGING_OFFSET_BYTES % FLASH_SECTOR_SIZE) != 0u) {
    return false;
  }
  if ((APP_OTA_STAGING_SIZE_BYTES % FLASH_SECTOR_SIZE) != 0u) {
    return false;
  }
  if (APP_OTA_STAGING_OFFSET_BYTES >= PICO_FLASH_SIZE_BYTES ||
      staging_end > PICO_FLASH_SIZE_BYTES) {
    return false;
  }
  if (APP_OTA_TARGET_MAX_IMAGE_SIZE_BYTES > APP_OTA_STAGING_OFFSET_BYTES) {
    return false;
  }
  if (current_binary_end >= APP_OTA_STAGING_OFFSET_BYTES) {
    return false;
  }

  return true;
}

static uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  size_t index = 0u;
  uint32_t value = crc;

  for (index = 0u; index < len; ++index) {
    uint32_t byte = data[index];
    uint32_t bit = 0u;
    value ^= byte;
    for (bit = 0u; bit < 8u; ++bit) {
      const uint32_t mask = (uint32_t)-(int32_t)(value & 1u);
      value = (value >> 1u) ^ (0xedb88320u & mask);
    }
  }

  return value;
}

static void ota_copy_string(char *destination, size_t destination_size,
                            const char *source) {
  size_t write_index = 0u;

  if (destination == NULL || destination_size == 0u) {
    return;
  }

  if (source != NULL) {
    while (source[write_index] != '\0' && write_index < (destination_size - 1u)) {
      destination[write_index] = source[write_index];
      write_index++;
    }
  }

  destination[write_index] = '\0';
}

static void ota_sanitize_version_label(char *destination, size_t destination_size,
                                       const char *source) {
  size_t write_index = 0u;
  size_t read_index = 0u;
  bool has_content = false;

  if (destination == NULL || destination_size == 0u) {
    return;
  }

  if (source != NULL) {
    while (source[read_index] != '\0' && write_index < (destination_size - 1u)) {
      char ch = source[read_index++];
      if (isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.') {
        destination[write_index++] = ch;
        has_content = true;
      } else if (!isspace((unsigned char)ch)) {
        destination[write_index++] = '_';
        has_content = true;
      }
    }
  }

  if (!has_content) {
    ota_copy_string(destination, destination_size, "unspecified");
    return;
  }

  destination[write_index] = '\0';
}

static void ota_set_error_locked(const char *message) {
  g_context.state = OTA_UPDATE_STATE_ERROR;
  ota_copy_string(g_context.last_error, sizeof(g_context.last_error), message);
}

static bool ota_flash_verify_erased(uint32_t flash_offset_bytes,
                                    size_t verify_length) {
  const volatile uint8_t *flash_bytes =
      (const volatile uint8_t *)(XIP_BASE + flash_offset_bytes);
  size_t index = 0u;

  for (index = 0u; index < verify_length; ++index) {
    if (flash_bytes[index] != 0xffu) {
      return false;
    }
  }

  return true;
}

static bool ota_flash_verify_programmed(uint32_t flash_offset_bytes,
                                        const uint8_t *expected_data,
                                        size_t verify_length) {
  const volatile uint8_t *flash_bytes =
      (const volatile uint8_t *)(XIP_BASE + flash_offset_bytes);
  size_t index = 0u;

  if (expected_data == NULL) {
    return false;
  }

  for (index = 0u; index < verify_length; ++index) {
    if (flash_bytes[index] != expected_data[index]) {
      return false;
    }
  }

  return true;
}

static bool ota_flash_erase_sector(uint32_t flash_offset_bytes) {
  const uint32_t irq_state = save_and_disable_interrupts();
  flash_range_erase(flash_offset_bytes, FLASH_SECTOR_SIZE);
  restore_interrupts(irq_state);
  return ota_flash_verify_erased(flash_offset_bytes, FLASH_SECTOR_SIZE);
}

static bool ota_flash_program_page(uint32_t flash_offset_bytes,
                                   const uint8_t *page_data) {
  const uint32_t irq_state = save_and_disable_interrupts();
  flash_range_program(flash_offset_bytes, page_data, FLASH_PAGE_SIZE);
  restore_interrupts(irq_state);
  return ota_flash_verify_programmed(flash_offset_bytes, page_data,
                                     FLASH_PAGE_SIZE);
}

static bool ota_stage_program_current_page_locked(void) {
  const uint32_t page_offset = g_context.staged_programmed_size_bytes;
  const uint32_t staging_flash_offset = APP_OTA_STAGING_OFFSET_BYTES + page_offset;

  if ((page_offset + FLASH_PAGE_SIZE) > APP_OTA_STAGING_SIZE_BYTES) {
    ota_set_error_locked("staging_overflow");
    return false;
  }

  if ((page_offset % FLASH_SECTOR_SIZE) == 0u) {
    if (!ota_flash_erase_sector(staging_flash_offset)) {
      ota_set_error_locked("flash_erase_failed");
      return false;
    }
  }

  if (!ota_flash_program_page(staging_flash_offset, g_context.page_buffer)) {
    ota_set_error_locked("flash_program_failed");
    return false;
  }

  g_context.staged_programmed_size_bytes += FLASH_PAGE_SIZE;
  g_context.page_fill_bytes = 0u;
  return true;
}

static bool ota_stage_validate_image_locked(void) {
  const volatile uint32_t *vector_table =
      (const volatile uint32_t *)(XIP_BASE + APP_OTA_STAGING_OFFSET_BYTES);
  const uint32_t initial_stack_pointer = vector_table[0];
  const uint32_t reset_vector = vector_table[1];
  const uint32_t reset_vector_no_thumb = reset_vector & ~1u;
  const uint32_t xip_image_end = XIP_BASE + APP_OTA_TARGET_MAX_IMAGE_SIZE_BYTES;

  if (initial_stack_pointer < SRAM_BASE || initial_stack_pointer >= SRAM_END) {
    ota_set_error_locked("vector_sp_invalid");
    return false;
  }
  if ((reset_vector & 1u) == 0u) {
    ota_set_error_locked("vector_reset_thumb");
    return false;
  }
  if (reset_vector_no_thumb < XIP_BASE || reset_vector_no_thumb >= xip_image_end) {
    ota_set_error_locked("vector_reset_invalid");
    return false;
  }

  return true;
}

static uint32_t ota_round_up_to_page(uint32_t value) {
  return (value + (FLASH_PAGE_SIZE - 1u)) & ~(FLASH_PAGE_SIZE - 1u);
}

static void ota_context_reset_locked(void) {
  g_context.state = OTA_UPDATE_STATE_IDLE;
  g_context.expected_size_bytes = 0u;
  g_context.received_size_bytes = 0u;
  g_context.expected_crc32 = 0u;
  g_context.computed_crc32 = 0u;
  g_context.running_crc32 = 0xffffffffu;
  g_context.next_expected_offset = 0u;
  g_context.staged_programmed_size_bytes = 0u;
  g_context.page_fill_bytes = 0u;
  g_context.apply_task_handle = NULL;
  g_context.staged_version[0] = '\0';
  g_context.last_error[0] = '\0';
}

static void ota_copy_status_locked(ota_update_status_t *out_status) {
  if (out_status == NULL) {
    return;
  }

  *out_status = (ota_update_status_t){
      .state = g_context.state,
      .expected_size_bytes = g_context.expected_size_bytes,
      .received_size_bytes = g_context.received_size_bytes,
      .expected_crc32 = g_context.expected_crc32,
      .computed_crc32 = g_context.computed_crc32,
      .apply_task_active = g_context.apply_task_handle != NULL,
      .staged_version = {0},
      .last_error = {0},
  };

  ota_copy_string(out_status->staged_version, sizeof(out_status->staged_version),
                  g_context.staged_version);
  ota_copy_string(out_status->last_error, sizeof(out_status->last_error),
                  g_context.last_error);
}

static void __not_in_flash_func(ota_apply_staged_image_and_reboot)(
    uint32_t image_size_bytes) {
  uint32_t copied_bytes = 0u;
  uint32_t write_offset = 0u;

  if (image_size_bytes == 0u || image_size_bytes > APP_OTA_TARGET_MAX_IMAGE_SIZE_BYTES) {
    watchdog_reboot(0u, 0u, 10u);
    while (1) {
      tight_loop_contents();
    }
  }

  (void)save_and_disable_interrupts();

  while (copied_bytes < image_size_bytes) {
    uint32_t sector_copy_size = FLASH_SECTOR_SIZE;
    uint32_t index = 0u;
    const volatile uint8_t *source_sector =
        (const volatile uint8_t *)(XIP_BASE + APP_OTA_STAGING_OFFSET_BYTES + write_offset);

    if ((image_size_bytes - copied_bytes) < FLASH_SECTOR_SIZE) {
      sector_copy_size = image_size_bytes - copied_bytes;
    }

    memset(g_apply_sector_buffer, 0xffu, sizeof(g_apply_sector_buffer));
    for (index = 0u; index < sector_copy_size; ++index) {
      g_apply_sector_buffer[index] = source_sector[index];
    }

    flash_range_erase(write_offset, FLASH_SECTOR_SIZE);

    for (index = 0u; index < FLASH_SECTOR_SIZE; index += FLASH_PAGE_SIZE) {
      flash_range_program(write_offset + index, g_apply_sector_buffer + index,
                          FLASH_PAGE_SIZE);
    }

    copied_bytes += sector_copy_size;
    write_offset += FLASH_SECTOR_SIZE;
  }

  watchdog_reboot(0u, 0u, 10u);
  while (1) {
    tight_loop_contents();
  }
}

static void ota_apply_task_entry(void *params) {
  const uint32_t bytes_to_apply = g_apply_image_size_bytes;
  (void)params;

  vTaskDelay(pdMS_TO_TICKS(APP_OTA_APPLY_DELAY_MS));
  ota_apply_staged_image_and_reboot(bytes_to_apply);

  if (g_context.mutex != NULL) {
    if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) == pdTRUE) {
      g_context.apply_task_handle = NULL;
      ota_set_error_locked("apply_unexpected_return");
      xSemaphoreGive(g_context.mutex);
    }
  }

  vTaskDelete(NULL);
}

void ota_update_service_init(void) {
  SemaphoreHandle_t mutex = g_context.mutex;

  if (g_context.mutex == NULL) {
    g_context.mutex = xSemaphoreCreateMutex();
    mutex = g_context.mutex;
  }

  if (g_context.mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (!g_context.initialized) {
    memset(&g_context, 0, sizeof(g_context));
    g_context.mutex = mutex;
    ota_context_reset_locked();
    g_context.initialized = true;
  }

  if (!ota_layout_is_valid()) {
    ota_set_error_locked("layout_invalid");
  } else if (g_context.state == OTA_UPDATE_STATE_ERROR &&
             strcmp(g_context.last_error, "layout_invalid") == 0) {
    g_context.state = OTA_UPDATE_STATE_IDLE;
    g_context.last_error[0] = '\0';
  }

  xSemaphoreGive(g_context.mutex);
}

const char *ota_update_service_get_firmware_version(void) {
  return APP_FIRMWARE_VERSION;
}

ota_update_result_t ota_update_service_begin(uint32_t image_size_bytes,
                                             uint32_t expected_crc32,
                                             const char *staged_version) {
  ota_update_result_t result = OTA_UPDATE_RESULT_OK;

  ota_update_service_init();
  if (g_context.mutex == NULL) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (!ota_layout_is_valid()) {
    ota_set_error_locked("layout_invalid");
    result = OTA_UPDATE_RESULT_INTERNAL;
    goto finish;
  }

  if (g_context.state == OTA_UPDATE_STATE_RECEIVING ||
      g_context.state == OTA_UPDATE_STATE_APPLYING) {
    result = OTA_UPDATE_RESULT_BUSY;
    goto finish;
  }

  if (image_size_bytes == 0u ||
      image_size_bytes > APP_OTA_TARGET_MAX_IMAGE_SIZE_BYTES ||
      image_size_bytes > APP_OTA_STAGING_SIZE_BYTES) {
    ota_set_error_locked("size_out_of_range");
    result = OTA_UPDATE_RESULT_SIZE_OUT_OF_RANGE;
    goto finish;
  }

  ota_context_reset_locked();
  ota_sanitize_version_label(g_context.staged_version,
                             sizeof(g_context.staged_version), staged_version);

  g_context.state = OTA_UPDATE_STATE_RECEIVING;
  g_context.expected_size_bytes = image_size_bytes;
  g_context.expected_crc32 = expected_crc32;
  g_context.running_crc32 = 0xffffffffu;
  g_context.last_error[0] = '\0';

finish:
  xSemaphoreGive(g_context.mutex);
  return result;
}

ota_update_result_t ota_update_service_write_chunk(uint32_t offset,
                                                   const uint8_t *chunk_data,
                                                   size_t chunk_length) {
  ota_update_result_t result = OTA_UPDATE_RESULT_OK;
  size_t source_index = 0u;

  if (chunk_data == NULL || chunk_length == 0u) {
    return OTA_UPDATE_RESULT_INVALID_ARGUMENT;
  }

  ota_update_service_init();
  if (g_context.mutex == NULL) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (g_context.state != OTA_UPDATE_STATE_RECEIVING) {
    result = OTA_UPDATE_RESULT_INVALID_STATE;
    goto finish;
  }

  if (offset != g_context.next_expected_offset) {
    result = OTA_UPDATE_RESULT_OFFSET_MISMATCH;
    goto finish;
  }

  if ((g_context.received_size_bytes + chunk_length) >
      g_context.expected_size_bytes) {
    result = OTA_UPDATE_RESULT_INVALID_ARGUMENT;
    goto finish;
  }

  g_context.running_crc32 =
      ota_crc32_update(g_context.running_crc32, chunk_data, chunk_length);

  while (source_index < chunk_length) {
    size_t remaining_page = FLASH_PAGE_SIZE - g_context.page_fill_bytes;
    size_t copy_length = chunk_length - source_index;
    if (copy_length > remaining_page) {
      copy_length = remaining_page;
    }

    memcpy(g_context.page_buffer + g_context.page_fill_bytes,
           chunk_data + source_index, copy_length);

    g_context.page_fill_bytes += copy_length;
    source_index += copy_length;

    if (g_context.page_fill_bytes == FLASH_PAGE_SIZE) {
      if (!ota_stage_program_current_page_locked()) {
        result = OTA_UPDATE_RESULT_FLASH_IO;
        goto finish;
      }
    }
  }

  g_context.received_size_bytes += (uint32_t)chunk_length;
  g_context.next_expected_offset += (uint32_t)chunk_length;

finish:
  xSemaphoreGive(g_context.mutex);
  return result;
}

ota_update_result_t ota_update_service_finish(void) {
  ota_update_result_t result = OTA_UPDATE_RESULT_OK;

  ota_update_service_init();
  if (g_context.mutex == NULL) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (g_context.state != OTA_UPDATE_STATE_RECEIVING) {
    result = OTA_UPDATE_RESULT_INVALID_STATE;
    goto finish;
  }

  if (g_context.received_size_bytes != g_context.expected_size_bytes) {
    ota_set_error_locked("size_mismatch");
    result = OTA_UPDATE_RESULT_INVALID_STATE;
    goto finish;
  }

  if (g_context.page_fill_bytes > 0u) {
    memset(g_context.page_buffer + g_context.page_fill_bytes, 0xffu,
           FLASH_PAGE_SIZE - g_context.page_fill_bytes);

    if (!ota_stage_program_current_page_locked()) {
      result = OTA_UPDATE_RESULT_FLASH_IO;
      goto finish;
    }
  }

  g_context.computed_crc32 = ~g_context.running_crc32;
  if (g_context.computed_crc32 != g_context.expected_crc32) {
    ota_set_error_locked("crc_mismatch");
    result = OTA_UPDATE_RESULT_IMAGE_INVALID;
    goto finish;
  }

  if (!ota_stage_validate_image_locked()) {
    result = OTA_UPDATE_RESULT_IMAGE_INVALID;
    goto finish;
  }

  g_context.state = OTA_UPDATE_STATE_READY;
  g_context.last_error[0] = '\0';

finish:
  xSemaphoreGive(g_context.mutex);
  return result;
}

ota_update_result_t ota_update_service_request_apply_async(void) {
  ota_update_result_t result = OTA_UPDATE_RESULT_OK;
  TaskHandle_t task_handle = NULL;
  uint32_t apply_size = 0u;

  ota_update_service_init();
  if (g_context.mutex == NULL) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (g_context.state != OTA_UPDATE_STATE_READY) {
    result = OTA_UPDATE_RESULT_INVALID_STATE;
    goto finish_locked;
  }

  if (g_context.apply_task_handle != NULL) {
    result = OTA_UPDATE_RESULT_BUSY;
    goto finish_locked;
  }

  apply_size = ota_round_up_to_page(g_context.expected_size_bytes);
  if (apply_size == 0u || apply_size > APP_OTA_TARGET_MAX_IMAGE_SIZE_BYTES ||
      apply_size > g_context.staged_programmed_size_bytes) {
    ota_set_error_locked("apply_size_invalid");
    result = OTA_UPDATE_RESULT_INTERNAL;
    goto finish_locked;
  }

  g_apply_image_size_bytes = apply_size;
  g_context.state = OTA_UPDATE_STATE_APPLYING;
  g_context.last_error[0] = '\0';

finish_locked:
  xSemaphoreGive(g_context.mutex);
  if (result != OTA_UPDATE_RESULT_OK) {
    return result;
  }

  if (xTaskCreate(ota_apply_task_entry, "OTAApplyTask",
                  APP_OTA_APPLY_TASK_STACK_WORDS, NULL,
                  APP_OTA_APPLY_TASK_PRIORITY, &task_handle) != pdPASS) {
    if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) == pdTRUE) {
      g_context.apply_task_handle = NULL;
      ota_set_error_locked("apply_task_create_failed");
      xSemaphoreGive(g_context.mutex);
    }
    return OTA_UPDATE_RESULT_INTERNAL;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) == pdTRUE) {
    g_context.apply_task_handle = task_handle;
    xSemaphoreGive(g_context.mutex);
  }

  return OTA_UPDATE_RESULT_OK;
}

void ota_update_service_get_status(ota_update_status_t *out_status) {
  ota_update_service_init();
  if (out_status == NULL) {
    return;
  }

  memset(out_status, 0, sizeof(*out_status));

  if (g_context.mutex == NULL) {
    out_status->state = OTA_UPDATE_STATE_ERROR;
    ota_copy_string(out_status->last_error, sizeof(out_status->last_error),
                    "mutex_unavailable");
    return;
  }

  if (xSemaphoreTake(g_context.mutex, portMAX_DELAY) != pdTRUE) {
    out_status->state = OTA_UPDATE_STATE_ERROR;
    ota_copy_string(out_status->last_error, sizeof(out_status->last_error),
                    "mutex_timeout");
    return;
  }

  ota_copy_status_locked(out_status);
  xSemaphoreGive(g_context.mutex);
}

const char *ota_update_service_state_name(ota_update_state_t state) {
  switch (state) {
  case OTA_UPDATE_STATE_IDLE:
    return "idle";
  case OTA_UPDATE_STATE_RECEIVING:
    return "receiving";
  case OTA_UPDATE_STATE_READY:
    return "ready";
  case OTA_UPDATE_STATE_APPLYING:
    return "applying";
  case OTA_UPDATE_STATE_ERROR:
    return "error";
  default:
    return "unknown";
  }
}

const char *ota_update_result_name(ota_update_result_t result) {
  switch (result) {
  case OTA_UPDATE_RESULT_OK:
    return "ok";
  case OTA_UPDATE_RESULT_BUSY:
    return "busy";
  case OTA_UPDATE_RESULT_INVALID_ARGUMENT:
    return "invalid_argument";
  case OTA_UPDATE_RESULT_INVALID_STATE:
    return "invalid_state";
  case OTA_UPDATE_RESULT_SIZE_OUT_OF_RANGE:
    return "size_out_of_range";
  case OTA_UPDATE_RESULT_OFFSET_MISMATCH:
    return "offset_mismatch";
  case OTA_UPDATE_RESULT_FLASH_IO:
    return "flash_io";
  case OTA_UPDATE_RESULT_IMAGE_INVALID:
    return "image_invalid";
  case OTA_UPDATE_RESULT_INTERNAL:
    return "internal";
  default:
    return "unknown";
  }
}
