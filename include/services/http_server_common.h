#ifndef HTTP_SERVER_COMMON_H
#define HTTP_SERVER_COMMON_H

#include "lwip/api.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef HTTP_REQUEST_LINE_BUFFER_SIZE
#define HTTP_REQUEST_LINE_BUFFER_SIZE 256u
#endif

#ifndef HTTP_REQUEST_BUFFER_SIZE
#define HTTP_REQUEST_BUFFER_SIZE 6144u
#endif

#ifndef HTTP_REQUEST_PATH_SIZE
#define HTTP_REQUEST_PATH_SIZE 96u
#endif

#ifndef HTTP_MAX_BODY_SIZE
#define HTTP_MAX_BODY_SIZE 4096u
#endif

#ifndef HTTP_RESPONSE_CHUNK_SIZE
#define HTTP_RESPONSE_CHUNK_SIZE 1024u
#endif

typedef enum {
  HTTP_METHOD_UNKNOWN = 0,
  HTTP_METHOD_GET,
  HTTP_METHOD_HEAD,
  HTTP_METHOD_POST,
} http_method_t;

typedef struct {
  http_method_t method;
  char path[HTTP_REQUEST_PATH_SIZE];
  char body[HTTP_MAX_BODY_SIZE + 1u];
  size_t body_length;
} http_request_t;

void http_send_response(struct netconn *connection, const char *status_line,
                        const char *content_type, const uint8_t *body,
                        size_t body_length);
void http_send_text_response(struct netconn *connection, const char *status_line,
                             const char *content_type, const char *body);
void http_send_headers_only(struct netconn *connection, const char *status_line,
                            const char *content_type, size_t content_length);
bool http_parse_request(struct netconn *connection, http_request_t *out_request);

#endif
