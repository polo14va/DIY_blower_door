#include "services/http_server_common.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void http_send_response(struct netconn *connection, const char *status_line,
                        const char *content_type, const uint8_t *body,
                        size_t body_length) {
  char header[192];
  size_t offset = 0u;
  const int header_length = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %lu\r\n"
      "Connection: close\r\n"
      "\r\n",
      status_line, content_type, (unsigned long)body_length);

  if (header_length <= 0 || (size_t)header_length >= sizeof(header)) {
    return;
  }

  if (netconn_write(connection, header, (size_t)header_length, NETCONN_COPY) !=
      ERR_OK) {
    return;
  }

  while (body != NULL && offset < body_length) {
    const size_t remaining = body_length - offset;
    const size_t chunk_size =
        remaining > HTTP_RESPONSE_CHUNK_SIZE ? HTTP_RESPONSE_CHUNK_SIZE
                                             : remaining;
    if (netconn_write(connection, body + offset, chunk_size, NETCONN_COPY) !=
        ERR_OK) {
      return;
    }
    offset += chunk_size;
  }
}

void http_send_text_response(struct netconn *connection, const char *status_line,
                             const char *content_type, const char *body) {
  http_send_response(connection, status_line, content_type, (const uint8_t *)body,
                     strlen(body));
}

void http_send_headers_only(struct netconn *connection, const char *status_line,
                            const char *content_type, size_t content_length) {
  char header[192];
  const int header_length = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %lu\r\n"
      "Connection: close\r\n"
      "\r\n",
      status_line, content_type, (unsigned long)content_length);

  if (header_length <= 0 || (size_t)header_length >= sizeof(header)) {
    return;
  }

  netconn_write(connection, header, (size_t)header_length, NETCONN_COPY);
}

static bool http_parse_request_path_and_method(const char *request_data,
                                               size_t request_length,
                                               http_method_t *out_method,
                                               char *out_path,
                                               size_t out_path_size) {
  char request_line[HTTP_REQUEST_LINE_BUFFER_SIZE];
  const char *method_prefix = NULL;
  char *path_begin = NULL;
  char *path_end = NULL;
  char *absolute_path_start = NULL;
  char *query_separator = NULL;
  size_t copy_length = request_length;
  size_t path_length = 0u;

  if (request_data == NULL || out_method == NULL || out_path == NULL ||
      out_path_size == 0u || request_length < 5u) {
    return false;
  }

  if (copy_length >= sizeof(request_line)) {
    copy_length = sizeof(request_line) - 1u;
  }

  memcpy(request_line, request_data, copy_length);
  request_line[copy_length] = '\0';

  if (strncmp(request_line, "GET ", 4) == 0) {
    *out_method = HTTP_METHOD_GET;
    method_prefix = "GET ";
  } else if (strncmp(request_line, "HEAD ", 5) == 0) {
    *out_method = HTTP_METHOD_HEAD;
    method_prefix = "HEAD ";
  } else if (strncmp(request_line, "POST ", 5) == 0) {
    *out_method = HTTP_METHOD_POST;
    method_prefix = "POST ";
  } else {
    *out_method = HTTP_METHOD_UNKNOWN;
    return false;
  }

  path_begin = request_line + strlen(method_prefix);
  path_end = strchr(path_begin, ' ');
  if (path_end == NULL || path_end == path_begin) {
    return false;
  }

  path_length = (size_t)(path_end - path_begin);
  if (path_length >= out_path_size) {
    path_length = out_path_size - 1u;
  }

  memcpy(out_path, path_begin, path_length);
  out_path[path_length] = '\0';

  if (strncmp(out_path, "http://", 7) == 0 ||
      strncmp(out_path, "https://", 8) == 0) {
    absolute_path_start = strstr(out_path, "://");
    if (absolute_path_start != NULL) {
      absolute_path_start += 3;
      absolute_path_start = strchr(absolute_path_start, '/');
      if (absolute_path_start != NULL) {
        memmove(out_path, absolute_path_start, strlen(absolute_path_start) + 1u);
      } else {
        strcpy(out_path, "/");
      }
    }
  }

  query_separator = strchr(out_path, '?');
  if (query_separator != NULL) {
    *query_separator = '\0';
  }

  if (out_path[0] == '\0') {
    strcpy(out_path, "/");
  }

  return true;
}

static size_t http_extract_content_length(const char *buffer, size_t header_size) {
  const char *cursor = buffer;
  const char *end = buffer + header_size;

  while (cursor < end) {
    const char *line_end = strstr(cursor, "\r\n");
    size_t line_length = 0u;

    if (line_end == NULL || line_end > end) {
      break;
    }

    line_length = (size_t)(line_end - cursor);
    if (line_length == 0u) {
      break;
    }

    if (line_length > 15u && strncasecmp(cursor, "Content-Length:", 15u) == 0) {
      const char *value_start = cursor + 15u;
      while (value_start < line_end && isspace((unsigned char)*value_start)) {
        value_start++;
      }
      return (size_t)strtoul(value_start, NULL, 10);
    }

    cursor = line_end + 2;
  }

  return 0u;
}

static bool http_receive_request(struct netconn *connection, char *request_buffer,
                                 size_t request_buffer_size, size_t *out_total_size,
                                 size_t *out_header_size,
                                 size_t *out_content_length) {
  size_t total_size = 0u;
  size_t header_size = 0u;
  size_t content_length = 0u;
  bool headers_ready = false;
  bool request_complete = false;

  if (connection == NULL || request_buffer == NULL || out_total_size == NULL ||
      out_header_size == NULL || out_content_length == NULL ||
      request_buffer_size < 8u) {
    return false;
  }

  request_buffer[0] = '\0';
  *out_total_size = 0u;
  *out_header_size = 0u;
  *out_content_length = 0u;

  while (!request_complete && total_size < (request_buffer_size - 1u)) {
    struct netbuf *input_buffer = NULL;
    err_t receive_status = netconn_recv(connection, &input_buffer);

    if (receive_status != ERR_OK || input_buffer == NULL) {
      break;
    }

    do {
      char *chunk_data = NULL;
      u16_t chunk_length = 0u;
      size_t writable_length = 0u;

      netbuf_data(input_buffer, (void **)&chunk_data, &chunk_length);
      if (chunk_data == NULL || chunk_length == 0u) {
        continue;
      }

      writable_length = request_buffer_size - 1u - total_size;
      if (writable_length == 0u) {
        break;
      }

      if ((size_t)chunk_length > writable_length) {
        chunk_length = (u16_t)writable_length;
      }

      memcpy(request_buffer + total_size, chunk_data, (size_t)chunk_length);
      total_size += (size_t)chunk_length;
      request_buffer[total_size] = '\0';

      if (!headers_ready) {
        char *header_end = strstr(request_buffer, "\r\n\r\n");
        if (header_end != NULL) {
          headers_ready = true;
          header_size = (size_t)(header_end - request_buffer) + 4u;
          content_length = http_extract_content_length(request_buffer, header_size);

          if (header_size + content_length > request_buffer_size - 1u) {
            netbuf_delete(input_buffer);
            return false;
          }
        }
      }

      if (headers_ready && total_size >= header_size + content_length) {
        request_complete = true;
        break;
      }
    } while (netbuf_next(input_buffer) >= 0);

    netbuf_delete(input_buffer);
  }

  if (!headers_ready || total_size < header_size + content_length) {
    return false;
  }

  *out_total_size = header_size + content_length;
  *out_header_size = header_size;
  *out_content_length = content_length;
  return true;
}

bool http_parse_request(struct netconn *connection, http_request_t *out_request) {
  char request_buffer[HTTP_REQUEST_BUFFER_SIZE];
  size_t total_size = 0u;
  size_t header_size = 0u;
  size_t content_length = 0u;
  http_method_t method = HTTP_METHOD_UNKNOWN;
  char path[HTTP_REQUEST_PATH_SIZE];

  if (out_request == NULL) {
    return false;
  }

  memset(out_request, 0, sizeof(*out_request));

  if (!http_receive_request(connection, request_buffer, sizeof(request_buffer),
                            &total_size, &header_size, &content_length)) {
    return false;
  }

  if (!http_parse_request_path_and_method(request_buffer, header_size, &method,
                                          path, sizeof(path))) {
    return false;
  }

  out_request->method = method;
  strncpy(out_request->path, path, sizeof(out_request->path) - 1u);

  if (content_length > 0u) {
    const size_t body_copy_length =
        content_length > HTTP_MAX_BODY_SIZE ? HTTP_MAX_BODY_SIZE : content_length;
    memcpy(out_request->body, request_buffer + header_size, body_copy_length);
    out_request->body[body_copy_length] = '\0';
    out_request->body_length = body_copy_length;

    if (content_length > HTTP_MAX_BODY_SIZE) {
      return false;
    }
  }

  return true;
}
