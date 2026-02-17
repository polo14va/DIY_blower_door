#include "core0_app.h"

#include "FreeRTOS.h"
#include "task.h"

#include "shared_state.h"

#include <stdio.h>

void wifi_task_entry(void *params);

// NOTE: Future expansion point
// - Two differential pressure sensors over I2C
// - Additional peripherals / control loops
static void __unused i2c_sensors_task_entry(void *params) {
  (void)params;

  // TODO(core0): Add I2C init + periodic read of two differential pressure sensors.
  // Keep this task low priority so it never interferes with networking on Core0.

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void core0_start_tasks(void) {
  printf("Creating WiFiTask...\n");
  if (xTaskCreate(wifi_task_entry, "WiFiTask", 4096, NULL,
                  tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
    printf("FATAL: WiFiTask failed\n");
    for (;;)
      __asm("bkpt #0");
  }

  // For later: create additional tasks here (I2C sensors, etc).
  // xTaskCreate(i2c_sensors_task_entry, "I2C", 2048, NULL, tskIDLE_PRIORITY, NULL);
}
