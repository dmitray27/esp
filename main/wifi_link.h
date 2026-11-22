#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "wifi_common.h"

/* Элемент очереди на передачу в эфир.
   air_text — "<from>: <text>" для эфира.
   echo_text — "<id>:<text>" для WebSocket-рассылки в момент старта
   передачи; NULL — источник без эха (POST /send, консоль).
   from — имя отправителя для эха.
   Все строки выделены в куче, владение переходит к tx_task. */
typedef struct {
    char *air_text;
    char *echo_text;
    char from[WS_NAME_MAX + 1];
} tx_queue_item_t;

/* Initialize Wi-Fi AP, HTTP and WebSocket servers. */
void wifi_link_init(void);

/* Get the FreeRTOS queue used to feed messages to the AFSK transmitter. */
QueueHandle_t wifi_link_get_tx_queue(void);

/* Broadcast "from:<id>:text" (or legacy "from:text") to all connected
   WebSocket clients. The [text] argument may contain the id prefix. */
void wifi_link_broadcast(const char *from, const char *text);

/* Send a "System:<text>" notice to all clients without storing it in history. */
void wifi_link_notify_all(const char *text);

/* True if the resulting "from:id:text" (id may be NULL/empty) fits one WS frame.
   Single limit for HTTP, WS and the client (kMaxFrameBytes). */
bool wifi_link_message_fits(const char *from, const char *id, const char *text);
