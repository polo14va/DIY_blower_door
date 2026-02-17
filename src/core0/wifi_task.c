#include "FreeRTOS.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "task.h"

#include "http_server.h"
#include "shared_state.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include <stdint.h>
#include <stdio.h>

// Defaults (overridable via CMake compile definitions WIFI_SSID/WIFI_PASSWORD)
#ifndef WIFI_SSID
#define WIFI_SSID "WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "WIFI_PASSWORD"
#endif

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
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  printf("Connected.\n");

  // Performance power management
  cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
  printf("Power Management set to PERFORMANCE\n");

  // Print assigned IP (DHCP)
  cyw43_arch_lwip_begin();
  const struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
  char ip_buf[IP4ADDR_STRLEN_MAX];
  ip4addr_ntoa_r(netif_ip4_addr(n), ip_buf, sizeof(ip_buf));
  cyw43_arch_lwip_end();
  printf("IP: %s\n", ip_buf);

  // Default motor power (can be changed via HTTP /power?value=NN)
  shared_set_dimmer_power_percent(0);

  // Run the web server (blocking loop)
  http_server_run();
}
