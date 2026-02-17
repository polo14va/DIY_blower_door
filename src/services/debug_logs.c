#include "services/debug_logs.h"

#include "app/app_config.h"
#include "hardware/sync.h"
#include <string.h>

#if APP_ENABLE_DEBUG_HTTP_ROUTES
static volatile bool g_debug_logs_enabled = false;
static volatile uint32_t g_debug_logs_generation = 0u;
static char g_debug_log_buffer[DEBUG_LOG_BUFFER_SIZE];
static size_t g_debug_log_length = 0u;
#endif

void debug_logs_clear(void) {
#if APP_ENABLE_DEBUG_HTTP_ROUTES
  const uint32_t irq_state = save_and_disable_interrupts();
  g_debug_log_length = 0u;
  g_debug_log_buffer[0] = '\0';
  g_debug_logs_generation += 1u;
  restore_interrupts(irq_state);
#endif
}

void debug_logs_append(const char *line) {
#if APP_ENABLE_DEBUG_HTTP_ROUTES
  size_t line_length = 0u;
  size_t overflow = 0u;
  const uint32_t irq_state = save_and_disable_interrupts();

  if (line == NULL || !g_debug_logs_enabled) {
    restore_interrupts(irq_state);
    return;
  }

  line_length = strlen(line);
  if (line_length + 1u >= DEBUG_LOG_BUFFER_SIZE) {
    line += line_length - (DEBUG_LOG_BUFFER_SIZE - 2u);
    line_length = DEBUG_LOG_BUFFER_SIZE - 2u;
  }

  if (g_debug_log_length + line_length + 1u >= DEBUG_LOG_BUFFER_SIZE) {
    overflow = g_debug_log_length + line_length + 1u - DEBUG_LOG_BUFFER_SIZE;
    if (overflow > g_debug_log_length) {
      overflow = g_debug_log_length;
    }
    if (overflow > 0u) {
      memmove(g_debug_log_buffer, g_debug_log_buffer + overflow,
              g_debug_log_length - overflow);
      g_debug_log_length -= overflow;
    }
  }

  memcpy(g_debug_log_buffer + g_debug_log_length, line, line_length);
  g_debug_log_length += line_length;
  g_debug_log_buffer[g_debug_log_length++] = '\n';
  g_debug_log_buffer[g_debug_log_length] = '\0';
  g_debug_logs_generation += 1u;

  restore_interrupts(irq_state);
#else
  (void)line;
#endif
}

void debug_logs_copy(char *out_buffer, size_t out_buffer_size) {
#if APP_ENABLE_DEBUG_HTTP_ROUTES
  const uint32_t irq_state = save_and_disable_interrupts();
  size_t copy_length = 0u;

  if (out_buffer == NULL || out_buffer_size == 0u) {
    restore_interrupts(irq_state);
    return;
  }

  copy_length = g_debug_log_length;
  if (copy_length >= out_buffer_size) {
    copy_length = out_buffer_size - 1u;
  }

  memcpy(out_buffer, g_debug_log_buffer, copy_length);
  out_buffer[copy_length] = '\0';
  restore_interrupts(irq_state);
#else
  if (out_buffer != NULL && out_buffer_size > 0u) {
    out_buffer[0] = '\0';
  }
#endif
}

bool debug_logs_enabled_get(void) {
#if APP_ENABLE_DEBUG_HTTP_ROUTES
  const uint32_t irq_state = save_and_disable_interrupts();
  const bool enabled = g_debug_logs_enabled;
  restore_interrupts(irq_state);
  return enabled;
#else
  return false;
#endif
}

uint32_t debug_logs_generation_get(void) {
#if APP_ENABLE_DEBUG_HTTP_ROUTES
  const uint32_t irq_state = save_and_disable_interrupts();
  const uint32_t generation = g_debug_logs_generation;
  restore_interrupts(irq_state);
  return generation;
#else
  return 0u;
#endif
}

void debug_logs_enabled_set(bool enabled) {
#if APP_ENABLE_DEBUG_HTTP_ROUTES
  const uint32_t irq_state = save_and_disable_interrupts();
  g_debug_logs_enabled = enabled;
  restore_interrupts(irq_state);
#else
  (void)enabled;
#endif
}

void debug_logs_copy_tail(char *out_buffer, size_t out_buffer_size) {
#if APP_ENABLE_DEBUG_HTTP_ROUTES
  const uint32_t irq_state = save_and_disable_interrupts();
  size_t copy_length = 0u;
  size_t start_index = 0u;

  if (out_buffer == NULL || out_buffer_size == 0u) {
    restore_interrupts(irq_state);
    return;
  }

  if (g_debug_log_length > 0u) {
    const size_t max_copy = out_buffer_size - 1u;
    if (g_debug_log_length > max_copy) {
      start_index = g_debug_log_length - max_copy;
      copy_length = max_copy;
    } else {
      copy_length = g_debug_log_length;
    }
    memcpy(out_buffer, g_debug_log_buffer + start_index, copy_length);
  }

  out_buffer[copy_length] = '\0';
  restore_interrupts(irq_state);
#else
  if (out_buffer != NULL && out_buffer_size > 0u) {
    out_buffer[0] = '\0';
  }
#endif
}
