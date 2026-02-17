#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool web_assets_get(const char *request_path, const char **content_type,
                    const uint8_t **body, size_t *body_length);

#endif
