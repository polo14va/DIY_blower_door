#ifndef BLOWER_HTTP_SERVER_H
#define BLOWER_HTTP_SERVER_H

// Blocking HTTP server loop (lwIP netconn API).
// Must be called from a FreeRTOS task (Core0).
void http_server_run(void);

#endif // BLOWER_HTTP_SERVER_H
