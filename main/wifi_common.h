#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_http_server.h"

#include "afsk_common.h"

#define WS_NAME_MAX 32

#define TX_QUEUE_LEN 4
#define POST_BUF_SIZE 1024
#define WS_MAX_CLIENTS 4
#define HTTP_MAX_CLIENTS 4
#define HTTPD_INTERNAL_SOCKETS 3
#define NETWORK_SOCKET_RESERVE 2

// Кадр длиннее WS_MAX_FRAME_LEN вычитывается и игнорируется: на 200 бод
// столько данных всё равно уходит в эфир десятки секунд.
// Кадр длиннее WS_HARD_MAX_LEN вычитывать не пытаемся — рвём сессию,
// иначе придётся выделять произвольный объём памяти по запросу клиента.
#define WS_MAX_FRAME_LEN 1024
#define WS_HARD_MAX_LEN 8192

// Последние сообщения хранятся на плате: клиент, у которого соединение
// оборвалось (например при засыпании телефона), получает пропущенное
// сразу после рукопожатия
// Слот вмещает любой кадр, который проходит WS_MAX_FRAME_LEN, плюс имя
#define HISTORY_SIZE 20
#define HISTORY_MAX_LEN (WS_MAX_FRAME_LEN + WS_NAME_MAX + 8)

// Общий минимум между сообщениями одного клиента (WS-кадры и POST /send)
#define WS_MIN_MSG_INTERVAL_MS 100

#define WIFI_AP_IP "192.168.4.1"

// Один мьютекс на всё Wi-Fi состояние: слоты клиентов, история и
// s_last_post. Делить его нельзя: broadcast держит мьютекс и пишет
// в историю, разделение потребовало бы аудита порядка захвата.
extern SemaphoreHandle_t s_ws_mutex;
extern httpd_handle_t s_http_server;
extern httpd_handle_t s_ws_server;

// SSID точки доступа собирается в wifi_link_init и отдаётся клиенту по /info
extern char s_ssid[33];

// "<имя>: <текст>" должно влезать в одно сообщение в эфир
static inline bool air_message_fits(const char *from, const char *text) {
    size_t len = strlen(from) + 2 + strlen(text); // 2 = ": "
    return len <= (size_t)(AFSK_MAX_BLOCKS * MAX_BLOCK_LEN);
}
