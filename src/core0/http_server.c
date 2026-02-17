#include "http_server.h"

#include "shared_state.h"

#include "lwip/api.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void http_write_simple(struct netconn *conn, const char *status,
                              const char *content_type, const char *body) {
  char header[256];
  const size_t body_len = strlen(body);
  const int header_len = snprintf(header, sizeof(header),
                                  "HTTP/1.1 %s\r\n"
                                  "Content-Type: %s\r\n"
                                  "Content-Length: %u\r\n"
                                  "Connection: close\r\n"
                                  "\r\n",
                                  status, content_type, (unsigned)body_len);

  if (header_len > 0) {
    netconn_write(conn, header, (size_t)header_len, NETCONN_COPY);
  }
  netconn_write(conn, body, body_len, NETCONN_COPY);
}

static void http_handle_request(struct netconn *conn, const char *buf,
                                uint16_t buflen) {
  // Minimal parser: only GET and only the request line.
  if (buflen < 5 || memcmp(buf, "GET ", 4) != 0) {
    http_write_simple(conn, "405 Method Not Allowed", "text/plain",
                      "Only GET supported\n");
    return;
  }

  const char *path_start = buf + 4;
  const char *path_end = memchr(path_start, ' ', (size_t)(buflen - 4));
  if (!path_end) {
    http_write_simple(conn, "400 Bad Request", "text/plain", "Bad request\n");
    return;
  }

  char path[128];
  size_t path_len = (size_t)(path_end - path_start);
  if (path_len >= sizeof(path)) {
    path_len = sizeof(path) - 1;
  }
  memcpy(path, path_start, path_len);
  path[path_len] = '\0';

  if (strcmp(path, "/hello") == 0) {
    http_write_simple(conn, "200 OK", "application/json",
                      "{\"hello\":\"world\"}\n");
    return;
  }

  // Example control endpoint:
  //   GET /power?value=NN
  if (strncmp(path, "/power", 6) == 0) {
    const char *q = strchr(path, '?');
    if (!q) {
      http_write_simple(conn, "400 Bad Request", "text/plain",
                        "Missing query (?value=NN)\n");
      return;
    }

    const char *value = strstr(q + 1, "value=");
    if (!value) {
      http_write_simple(conn, "400 Bad Request", "text/plain",
                        "Missing value=NN\n");
      return;
    }

    long v = strtol(value + 6, NULL, 10);
    if (v < 0) {
      v = 0;
    }
    if (v > 100) {
      v = 100;
    }

    shared_set_dimmer_power_percent((uint8_t)v);

    char body[64];
    snprintf(body, sizeof(body), "power=%ld\n", v);
    http_write_simple(conn, "200 OK", "text/plain", body);
    return;
  }

  http_write_simple(conn, "200 OK", "text/plain",
                    "Blower Pico C (RP2350)\n"
                    "Endpoints:\n"
                    "  GET /hello\n"
                    "  GET /power?value=NN   (0..100)\n");
}

static void http_server_serve(struct netconn *conn) {
  struct netbuf *inbuf = NULL;
  char *buf = NULL;
  uint16_t buflen = 0;

  err_t err = netconn_recv(conn, &inbuf);
  if (err == ERR_OK && inbuf) {
    netbuf_data(inbuf, (void **)&buf, &buflen);
    if (buf && buflen) {
      http_handle_request(conn, buf, buflen);
    }
  }

  netconn_close(conn);

  if (inbuf) {
    netbuf_delete(inbuf);
  }
}

void http_server_run(void) {
  struct netconn *listener = netconn_new(NETCONN_TCP);
  if (!listener) {
    printf("HTTP: netconn_new failed\n");
    vTaskDelete(NULL);
  }

  netconn_bind(listener, IP_ADDR_ANY, 80);
  netconn_listen(listener);
  printf("HTTP Server listening on port 80\n");

  for (;;) {
    struct netconn *client = NULL;
    err_t err = netconn_accept(listener, &client);
    if (err == ERR_OK && client) {
      http_server_serve(client);
      netconn_delete(client);
    }
  }
}
