#include "app/task_bootstrap.h"

#include "app/app_config.h"
#include "tasks/task_entries.h"
#include "task.h"
#include <stddef.h>

typedef struct {
  TaskFunction_t entry_point;
  const char *task_name;
  configSTACK_DEPTH_TYPE stack_depth_words;
  UBaseType_t priority;
  void *parameters;
} app_task_definition_t;

static const app_task_definition_t k_default_tasks[] = {
#if APP_ENABLE_WIFI_TASK
    {
        .entry_point = wifi_task_entry,
        .task_name = "WiFiTask",
        .stack_depth_words = APP_WIFI_TASK_STACK_WORDS,
        .priority = APP_WIFI_TASK_PRIORITY,
        .parameters = NULL,
    },
#endif
#if APP_ENABLE_DIMMER_TASK
    {
        .entry_point = dimmer_task_entry,
        .task_name = "DimmerTask",
        .stack_depth_words = APP_DIMMER_TASK_STACK_WORDS,
        .priority = APP_DIMMER_TASK_PRIORITY,
        .parameters = NULL,
    },
#endif
#if APP_ENABLE_ADP910_TASK
    {
        .entry_point = adp910_sampling_task_entry,
        .task_name = "ADP910Task",
        .stack_depth_words = APP_ADP910_TASK_STACK_WORDS,
        .priority = APP_ADP910_TASK_PRIORITY,
        .parameters = NULL,
    },
#endif
};

BaseType_t app_create_default_tasks(void) {
  size_t index = 0;
  const size_t task_count = sizeof(k_default_tasks) / sizeof(k_default_tasks[0]);

  for (index = 0; index < task_count; ++index) {
    const app_task_definition_t *definition = &k_default_tasks[index];
    if (xTaskCreate(definition->entry_point, definition->task_name,
                    definition->stack_depth_words, definition->parameters,
                    definition->priority, NULL) != pdPASS) {
      return pdFAIL;
    }
  }

  return pdPASS;
}
