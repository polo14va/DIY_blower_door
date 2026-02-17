#include "FreeRTOS.h"
#include "app/task_bootstrap.h"
#include "pico/stdlib.h"
#include "platform/runtime_faults.h"
#include "task.h"
#include <stdio.h>

int main(void) {
  stdio_init_all();

  for (volatile int spin = 0; spin < 8000000; ++spin) {
    __asm("nop");
  }

  printf("\n\n--- Blower Pico (RP2350) Initializing ---\n");
  printf("Target: RP2350 (Cortex-M33)\n");

  runtime_install_fault_handlers();
  printf("Runtime handlers installed.\n");

  if (app_create_default_tasks() != pdPASS) {
    runtime_panic("Task creation failed");
  }

  printf("Starting FreeRTOS scheduler...\n");
  fflush(stdout);

  vTaskStartScheduler();

  runtime_panic("Scheduler returned unexpectedly");
  return 0;
}
