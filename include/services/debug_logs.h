#ifndef DEBUG_LOGS_H
#define DEBUG_LOGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef DEBUG_LOG_BUFFER_SIZE
#define DEBUG_LOG_BUFFER_SIZE 1024u
#endif

#ifndef DEBUG_LOG_TAIL_CHARS
#define DEBUG_LOG_TAIL_CHARS 192u
#endif

void debug_logs_clear(void);
void debug_logs_append(const char *line);
void debug_logs_copy(char *out_buffer, size_t out_buffer_size);
bool debug_logs_enabled_get(void);
uint32_t debug_logs_generation_get(void);
void debug_logs_enabled_set(bool enabled);
void debug_logs_copy_tail(char *out_buffer, size_t out_buffer_size);

#endif
