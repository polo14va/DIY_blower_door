#include "FreeRTOS.h"
#include "lwip/api.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "task.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef WIFI_SSID
#define WIFI_SSID "WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "WIFI_PASSWORD"
#endif

void http_server_serve(struct netconn *conn) {
  struct netbuf *inbuf;
  char *buf;
  u16_t buflen;
  err_t err;

  /* Read the data from the port, blocking if nothing yet there.
     We assume the request (the part we care about) is in one netbuf */
  err = netconn_recv(conn, &inbuf);

  if (err == ERR_OK) {
    netbuf_data(inbuf, (void **)&buf, &buflen);

    // Simple check for GET request
    if (buflen >= 5 && buf[0] == 'G' && buf[1] == 'E' && buf[2] == 'T') {
      printf("HTTP Request received\n");
      const char *response = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 12\r\n"
                             "\r\n"
                             "Hello World!";

      netconn_write(conn, response, strlen(response), NETCONN_COPY);
    }
  }
  /* Close the connection (server closes in HTTP) */
  netconn_close(conn);

  /* Delete the buffer (netconn_recv gives us ownership) */
  if (inbuf != NULL) {
    netbuf_delete(inbuf);
  }
}

void wifi_task_entry(__unused void *params) {
  printf("Initializing WiFi...\n");
  if (cyw43_arch_init()) {
    printf("Failed to initialize cyw43_arch\n");
    vTaskDelete(NULL);
  }
  cyw43_arch_enable_sta_mode();

  printf("Connecting to WiFi...\n");
  while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                            CYW43_AUTH_WPA2_AES_PSK, 30000)) {
    printf("failed to connect.\n");
    vTaskDelay(1000);
  }
  printf("Connected.\n");

  // Performance power management
  cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
  printf("Power Management set to PERFORMANCE\n");

  // Start HTTP Server
  struct netconn *conn, *newconn;
  err_t err;

  conn = netconn_new(NETCONN_TCP);
  netconn_bind(conn, IP_ADDR_ANY, 80);
  netconn_listen(conn);

  printf("HTTP Server listening on port 80\n");

  bool led_state = false;
  while (1) {
    // Toggle LED to show life
    led_state = !led_state;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);

    err = netconn_accept(conn, &newconn);
    if (err == ERR_OK) {
      http_server_serve(newconn);
      netconn_delete(newconn);
    }
    // Yield and blink interval (100ms)
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
