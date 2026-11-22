#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#include "afsk_common.h"

#include "wifi_common.h"
#include "wifi_clients.h"
#include "wifi_history.h"
#include "wifi_link.h"
#include "wifi_ws.h"

static const char *TAG = "WIFI_WS";

httpd_handle_t s_ws_server = NULL;

// ============================
// Отправка WebSocket-кадров
// ============================

typedef struct {
    httpd_handle_t server;
    int fd;
    char *payload;
    size_t len;
} ws_send_ctx_t;

// Выполняется в задаче httpd: только так запись в сокет не пересекается
// с ответами самого сервера. Вызывать httpd_ws_send_frame_async напрямую
// из чужой задачи (например из rx_task) нельзя — кадры перемешаются
static void ws_send_work(void *arg) {
    ws_send_ctx_t *ctx = (ws_send_ctx_t *)arg;

    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)ctx->payload;
    ws_pkt.len = ctx->len;

    esp_err_t ret = httpd_ws_send_frame_async(ctx->server, ctx->fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS send to fd %d failed: %d", ctx->fd, ret);
        httpd_sess_trigger_close(ctx->server, ctx->fd);
    }
    free(ctx->payload);
    free(ctx);
}

static void ws_queue_text(int fd, const char *payload, size_t len) {
    if (!s_ws_server || len == 0) {
        return;
    }

    ws_send_ctx_t *ctx = (ws_send_ctx_t *)calloc(1, sizeof(ws_send_ctx_t));
    if (!ctx) {
        return;
    }

    ctx->payload = (char *)malloc(len + 1);
    if (!ctx->payload) {
        free(ctx);
        return;
    }

    memcpy(ctx->payload, payload, len);
    ctx->payload[len] = '\0';
    ctx->server = s_ws_server;
    ctx->fd = fd;
    ctx->len = len;

    if (httpd_queue_work(s_ws_server, ws_send_work, ctx) != ESP_OK) {
        ESP_LOGW(TAG, "WS work queue full, frame to fd %d dropped", fd);
        free(ctx->payload);
        free(ctx);
    }
}

// Служебное уведомление одному клиенту: приложение показывает его как SnackBar
static void ws_notify(int fd, const char *text) {
    char payload[128];
    int len = snprintf(payload, sizeof(payload), "System:%s", text);
    if (len < 0) {
        return;
    }
    if (len > (int)sizeof(payload) - 1) {
        len = (int)sizeof(payload) - 1;
    }
    ws_queue_text(fd, payload, (size_t)len);
}

static void ws_send_to_all(const char *payload, size_t payload_len);

void wifi_link_broadcast(const char *from, const char *text) {
    if (!s_ws_server || !from || !text) {
        return;
    }

    size_t from_len = strlen(from);
    size_t text_len = strlen(text);
    if (from_len == 0 || text_len == 0) {
        return;
    }

    size_t payload_len = from_len + 1 + text_len;
    char *payload = (char *)malloc(payload_len + 1);
    if (!payload) {
        return;
    }
    memcpy(payload, from, from_len);
    payload[from_len] = ':';
    memcpy(payload + from_len + 1, text, text_len);
    payload[payload_len] = '\0';

    history_add(payload);
    ws_send_to_all(payload, payload_len);
    free(payload);
}

void wifi_link_notify_all(const char *text) {
    if (!s_ws_server || !text || text[0] == '\0') {
        return;
    }
    char payload[160];
    int len = snprintf(payload, sizeof(payload), "System:%s", text);
    if (len < 0) {
        return;
    }
    if (len > (int)sizeof(payload) - 1) {
        len = (int)sizeof(payload) - 1;
    }
    ws_send_to_all(payload, (size_t)len);
}

static void ws_send_to_all(const char *payload, size_t payload_len) {
    size_t client_count = WS_MAX_CLIENTS;
    int client_fds[WS_MAX_CLIENTS];
    if (httpd_get_client_list(s_ws_server, &client_count, client_fds) == ESP_OK) {
        for (size_t i = 0; i < client_count; i++) {
#ifdef CONFIG_HTTPD_WS_SUPPORT
            // В списке есть и сокеты, не прошедшие рукопожатие
            if (httpd_ws_get_fd_info(s_ws_server, client_fds[i]) !=
                HTTPD_WS_CLIENT_WEBSOCKET) {
                continue;
            }
#endif
            ws_queue_text(client_fds[i], payload, payload_len);
        }
    }
}

// ============================
// Разбор кадров
// ============================

/* id клиента — только латиница/цифры (та же проверка в flutter/lib/chat_protocol.dart);
   иначе сегмент — начало legacy-текста с двоеточием внутри */
static bool looks_like_id(const char *s, size_t len) {
    if (len == 0 || len > 32) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static void ws_handle_frame(httpd_req_t *req, char *payload) {
    int fd = httpd_req_to_sockfd(req);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!client_check_rate(fd, now_ms)) {
        ESP_LOGW(TAG, "WS rate limit for fd %d", fd);
        ws_notify(fd, "Слишком быстро");
        return;
    }

    if (strncmp(payload, "setName:", 8) == 0) {
        const char *name = payload + 8;
        if (!name_is_valid(name)) {
            ESP_LOGW(TAG, "Rejected name from fd %d: '%s'", fd, name);
            ws_notify(fd, "Недопустимое имя");
            return;
        }
        client_set_name(fd, name);
        ESP_LOGI(TAG, "Client %d set name to %s", fd, name);
        return;
    }

    if (strncmp(payload, "msg:", 4) != 0) {
        return;
    }

    char *frame_name = payload + 4;
    char *colon1 = strchr(frame_name, ':');
    if (!colon1) {
        ESP_LOGW(TAG, "Malformed msg frame, no name separator: %s", payload);
        return;
    }

    /* Новый кадр: msg:<имя>:<id>:<текст>.
       Старый кадр msg:<имя>:<текст> (без id) оставлен для совместимости. */
    char *id_start = colon1 + 1;
    char *colon2 = strchr(id_start, ':');
    char *text;
    char *broadcast_text;

    if (colon2 && looks_like_id(id_start, (size_t)(colon2 - id_start))) {
        *colon1 = '\0';
        *colon2 = '\0';
        text = colon2 + 1;
        if (strlen(text) == 0) {
            return;
        }
        /* Восстанавливаем двоеточие между id и текстом: id_start теперь
           указывает на строку "<id>:<текст>", которую можно разослать. */
        *colon2 = ':';
        broadcast_text = id_start;
    } else {
        *colon1 = '\0';
        text = id_start;
        if (strlen(text) == 0) {
            return;
        }
        broadcast_text = text;
    }

    // Имя из setName: надёжнее того, что пришло в кадре
    char from[WS_NAME_MAX] = {0};
    if (!client_get_name(fd, from, sizeof(from))) {
        strlcpy(from, frame_name, sizeof(from));
    }
    if (!name_is_valid(from)) {
        strlcpy(from, "Unknown", sizeof(from));
    }

    // broadcast_text уже содержит "<id>:" (если был) — это кадр WebSocket-рассылки
    if (!wifi_link_message_fits(from, NULL, broadcast_text)) {
        ws_notify(fd, "Сообщение слишком длинное для передачи");
        return;
    }
    // В эфир уходит "<имя>: <текст>" — проверяем и его
    if (!air_message_fits(from, text)) {
        ws_notify(fd, "Сообщение слишком длинное для передачи");
        return;
    }

    QueueHandle_t tx_queue = wifi_link_get_tx_queue();
    if (!tx_queue) {
        ws_notify(fd, "Передатчик недоступен");
        return;
    }

    size_t air_len = strlen(from) + 2 + strlen(text); // 2 = ": "
    tx_queue_item_t item = {0};
    item.air_text = malloc(air_len + 1);
    if (!item.air_text) {
        ws_notify(fd, "Недостаточно памяти");
        return;
    }
    snprintf(item.air_text, air_len + 1, "%s: %s", from, text);
    strlcpy(item.from, from, sizeof(item.from));

    /* broadcast_text уже содержит "<id>:<text>" — эхо уйдёт в момент
       старта передачи из tx_task, когда BCL уже отработал. Копия
       обязательна: payload буфер освобождается при выходе отсюда. */
    item.echo_text = strdup(broadcast_text);
    if (!item.echo_text) {
        free(item.air_text);
        ws_notify(fd, "Недостаточно памяти");
        return;
    }

    if (xQueueSend(tx_queue, &item, pdMS_TO_TICKS(100)) != pdPASS) {
        free(item.air_text);
        free(item.echo_text);
        ESP_LOGW(TAG, "TX queue full, WS message dropped");
        ws_notify(fd, "Очередь передачи занята, сообщение не отправлено");
        return;
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS handshake done, fd=%d", fd);
        client_register(fd);
        history_send_to(fd, ws_queue_text);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Первый вызов без буфера возвращает длину кадра
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS frame header recv failed: %d", ret);
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    if (ws_pkt.len > WS_HARD_MAX_LEN) {
        ESP_LOGE(TAG, "WS frame of %u bytes, closing session",
                 (unsigned)ws_pkt.len);
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "WS buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;

    // Кадр вычитываем целиком даже если он слишком длинный:
    // иначе остаток тела примется за заголовок следующего кадра
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS recv failed: %d", ret);
        free(buf);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf[ws_pkt.len] = '\0';

        if (ws_pkt.len > WS_MAX_FRAME_LEN) {
            ESP_LOGW(TAG, "WS frame too long (%u bytes), ignored",
                     (unsigned)ws_pkt.len);
            ws_notify(httpd_req_to_sockfd(req), "Сообщение слишком длинное");
        } else {
            // Полный кадр (до 1 КБ ≈ 90 мс UART) блокирует httpd-таск и других
            // клиентов — в тихой сборке логируем только длину и начало
            if (AFSK_VERBOSE) {
                ESP_LOGI(TAG, "WS text from fd %d (%u bytes): %s",
                         httpd_req_to_sockfd(req), (unsigned)ws_pkt.len, (char *)buf);
            } else {
                ESP_LOGD(TAG, "WS text from fd %d (%u bytes): %.64s",
                         httpd_req_to_sockfd(req), (unsigned)ws_pkt.len, (char *)buf);
            }
            ws_handle_frame(req, (char *)buf);
        }
    }

    free(buf);
    return ESP_OK;
}

// Сокет закрывает httpd, но имя клиента нужно снять самим:
// номера дескрипторов переиспользуются
static void ws_close_fn(httpd_handle_t hd, int sockfd) {
    (void)hd;
    client_forget(sockfd);
    close(sockfd);
}

// ============================
// Запуск сервера
// ============================

esp_err_t start_ws_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 3;
    config.keep_alive_interval = 2;
    config.keep_alive_count = 2;
    config.ctrl_port = 32769;
    config.core_id = 0;
    config.max_open_sockets = WS_MAX_CLIENTS;
    config.max_uri_handlers = 2;
    config.lru_purge_enable = true;
    config.close_fn = ws_close_fn;

    if (httpd_start(&s_ws_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "WS server start failed");
        return ESP_FAIL;
    }

    httpd_uri_t ws_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
#ifdef CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = true,
        .handle_ws_control_frames = false,
#endif
    };

    httpd_register_uri_handler(s_ws_server, &ws_uri);

    ESP_LOGI(TAG, "WS server started on port 81 (Core 0)");
    return ESP_OK;
}
