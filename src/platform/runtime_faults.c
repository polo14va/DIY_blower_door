#include "platform/runtime_faults.h"

#include "FreeRTOS.h"
#include "hardware/exception.h"
#include "hardware/structs/scb.h"
#include "task.h"
#include <stdint.h>
#include <stdio.h>

uint32_t SystemCoreClock = 150000000;

extern void SVC_Handler(void);
extern void PendSV_Handler(void);
extern void SysTick_Handler(void);

static void runtime_dump_fault_registers(const char *fault_name) {
  printf("\n[FAULT] %s\n", fault_name);
  printf("  VTOR=0x%08lx\n", (unsigned long)scb_hw->vtor);
  printf("  CFSR=0x%08lx HFSR=0x%08lx DFSR=0x%08lx\n",
         (unsigned long)scb_hw->cfsr, (unsigned long)scb_hw->hfsr,
         (unsigned long)scb_hw->dfsr);
  printf("  MMFAR=0x%08lx BFAR=0x%08lx\n", (unsigned long)scb_hw->mmfar,
         (unsigned long)scb_hw->bfar);
  fflush(stdout);
}

static void runtime_fault_and_halt(const char *fault_name) {
  runtime_dump_fault_registers(fault_name);
  while (1) {
    __asm("bkpt #0");
  }
}

static void runtime_hardfault_handler(void) {
  runtime_fault_and_halt("HardFault");
}

static void runtime_memmanage_handler(void) {
  runtime_fault_and_halt("MemManage");
}

static void runtime_busfault_handler(void) {
  runtime_fault_and_halt("BusFault");
}

static void runtime_usagefault_handler(void) {
  runtime_fault_and_halt("UsageFault");
}

static void runtime_securefault_handler(void) {
  runtime_fault_and_halt("SecureFault");
}

void runtime_install_fault_handlers(void) {
  scb_hw->shcsr |= (1u << 16) | (1u << 17) | (1u << 18);

  exception_set_exclusive_handler(HARDFAULT_EXCEPTION,
                                  runtime_hardfault_handler);
  exception_set_exclusive_handler(MEMMANAGE_EXCEPTION,
                                  runtime_memmanage_handler);
  exception_set_exclusive_handler(BUSFAULT_EXCEPTION, runtime_busfault_handler);
  exception_set_exclusive_handler(USAGEFAULT_EXCEPTION,
                                  runtime_usagefault_handler);
  exception_set_exclusive_handler(SECUREFAULT_EXCEPTION,
                                  runtime_securefault_handler);

  exception_set_exclusive_handler(SVCALL_EXCEPTION, SVC_Handler);
  exception_set_exclusive_handler(PENDSV_EXCEPTION, PendSV_Handler);
  exception_set_exclusive_handler(SYSTICK_EXCEPTION, SysTick_Handler);
}

void runtime_panic(const char *message) {
  printf("\n[!! PANIC !!] %s\n", message);
  fflush(stdout);
  while (1) {
    __asm("bkpt #0");
  }
}

void vApplicationStackOverflowHook(TaskHandle_t task_handle,
                                   char *task_name) {
  (void)task_handle;
  printf("\nFATAL: Stack overflow in task %s\n", task_name);
  while (1) {
    __asm("bkpt #0");
  }
}

void vApplicationMallocFailedHook(void) {
  printf("\nFATAL: Malloc failed\n");
  while (1) {
    __asm("bkpt #0");
  }
}

void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
