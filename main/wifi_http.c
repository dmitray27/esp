#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#include "wifi_common.h"
#include "wifi_clients.h"
#include "wifi_link.h"
#include "wifi_http.h"

static const char *TAG = "WIFI_HTTP";

httpd_handle_t s_http_server = NULL;

// POST /send общий для всех клиентов: троттлим глобально одним счётчиком,
// потому что радиоканал и s_tx_queue одни на всех. Доступ под s_ws_mutex.
static uint32_t s_last_post_ms = 0;

// ============================
// HTTP
// ============================

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static void url_decode(char *out, size_t out_size, const char *in, const char *end) {
    size_t i = 0;
    while (i < out_size - 1 && in && *in && in < end) {
        if (*in == '+') {
            out[i++] = ' ';
        } else if (*in == '%' && (in + 2) < end &&
                   hex_val(in[1]) >= 0 && hex_val(in[2]) >= 0) {
            char decoded = (char)((hex_val(in[1]) << 4) | hex_val(in[2]));
            // Встроенный '\0' обрезал бы текст при strlen ниже
            if (decoded != '\0') {
                out[i++] = decoded;
            }
            in += 2;
        } else {
            out[i++] = *in;
        }
        in++;
    }
    out[i] = '\0';
}

// Ищет значение параметра key ("from=") в теле формы. Совпадение считается
// только в начале тела или после '&', иначе "myfrom=" сойдёт за "from="
static char *find_param(char *body, const char *key) {
    size_t klen = strlen(key);
    for (char *p = strstr(body, key); p; p = strstr(p + 1, key)) {
        if (p == body || p[-1] == '&') {
            return p + klen;
        }
    }
    return NULL;
}

static esp_err_t ping_get_handler(httpd_req_t *req) {
    const char *resp = "pong";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// Имя сети и адрес устройства: приложение показывает SSID, полученный отсюда
static esp_err_t info_get_handler(httpd_req_t *req) {
    char resp[96];
    int len = snprintf(resp, sizeof(resp), "{\"ssid\":\"%s\",\"ip\":\"%s\"}",
                       s_ssid, WIFI_AP_IP);
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Format error");
        return ESP_FAIL;
    }
    if (len > (int)sizeof(resp) - 1) {
        len = (int)sizeof(resp) - 1;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* Сколько таймаутов подряд терпим при чтении тела POST. */
#define POST_RECV_MAX_TIMEOUTS 5

static esp_err_t send_post_handler(httpd_req_t *req) {
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    // Раньше длинное тело молча обрезалось и клиент получал 200 OK
    // на текст, который в эфир уходил не целиком
    if (req->content_len > POST_BUF_SIZE - 1) {
        ESP_LOGW(TAG, "POST body of %d bytes rejected", (int)req->content_len);
        // httpd_err_code_t не содержит 413, поэтому статус ставим строкой
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Body too large");
        return ESP_FAIL;
    }

    size_t body_len = req->content_len;

    esp_err_t result = ESP_FAIL;
    char from[64] = {0};

    // Оба буфера в куче: стек задачи httpd всего несколько килобайт
    char *body = (char *)malloc(POST_BUF_SIZE);
    char *text = (char *)calloc(1, POST_BUF_SIZE);
    char *id_text = (char *)malloc(POST_BUF_SIZE + 32);
    if (!body || !text || !id_text) {
        free(body);
        free(text);
        free(id_text);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    // Лимит повторов при таймауте: на краю зоны Wi-Fi клиент может замолчать,
    // и бесконечный continue держал бы поток httpd занятым
    int total = 0;
    int timeouts = 0;
    while (total < (int)body_len) {
        int ret = httpd_req_recv(req, body + total, body_len - total);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > POST_RECV_MAX_TIMEOUTS) {
                break;
            }
            continue;
        }
        if (ret <= 0) {
            break;
        }
        timeouts = 0;  // пришли данные: клиент жив, счётчик подряд идущих таймаутов сбрасываем
        total += ret;
    }
    body[total] = '\0';

    // Неполное тело не разбираем: иначе в эфир ушёл бы обрезанный текст
    if (total != (int)body_len) {
        ESP_LOGW(TAG, "POST body incomplete: %d of %d bytes", total, (int)body_len);
        httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Incomplete body");
        goto cleanup;
    }

    char *p_from = find_param(body, "from=");
    if (!p_from) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'from'");
        goto cleanup;
    }

    char *p_text = find_param(body, "text=");
    if (!p_text) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'text'");
        goto cleanup;
    }

    const char *body_end = body + total;
    const char *p_from_end = strchr(p_from, '&');
    const char *p_text_end = strchr(p_text, '&');

    url_decode(from, sizeof(from), p_from, p_from_end ? p_from_end : body_end);
    url_decode(text, POST_BUF_SIZE, p_text, p_text_end ? p_text_end : body_end);

    if (!name_is_valid(from)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid 'from'");
        goto cleanup;
    }

    if (strlen(text) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty 'text'");
        goto cleanup;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    bool allowed = (s_last_post_ms == 0) ||
                   (now_ms - s_last_post_ms >= WS_MIN_MSG_INTERVAL_MS);
    if (allowed) {
        s_last_post_ms = now_ms;
    }
    xSemaphoreGive(s_ws_mutex);

    if (!allowed) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Too fast", HTTPD_RESP_USE_STRLEN);
        result = ESP_OK;
        goto cleanup;
    }

    /* id генерируем до проверки размера: лимит считается для итогового кадра from:id:text.
       Префикс 'p' отличает id от счётчиков rx_task и клиентов: приложение
       дедуплицирует историю по id, одинаковые номера теряли бы сообщения */
    static uint32_t post_id = 0;
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "p%lx", (unsigned long)++post_id);

    if (!wifi_link_message_fits(from, id_buf, text)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Message too long");
        goto cleanup;
    }
    if (!air_message_fits(from, text)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Message too long");
        goto cleanup;
    }

    QueueHandle_t tx_queue = wifi_link_get_tx_queue();
    if (!tx_queue) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No TX queue");
        goto cleanup;
    }

    size_t air_len = strlen(from) + 2 + strlen(text); // "<имя>: <текст>"
    tx_queue_item_t item = {0};
    item.air_text = malloc(air_len + 1);
    if (!item.air_text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        goto cleanup;
    }
    snprintf(item.air_text, air_len + 1, "%s: %s", from, text);
    strlcpy(item.from, from, sizeof(item.from));

    // В чат сообщение попадает только после того, как встало в очередь
    // на передачу, иначе клиент видел бы "отправлено" для того,
    // что в эфир не ушло
    if (xQueueSend(tx_queue, &item, pdMS_TO_TICKS(100)) != pdPASS) {
        free(item.air_text);
        ESP_LOGW(TAG, "TX queue full, POST message dropped");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "TX queue full", HTTPD_RESP_USE_STRLEN);
        result = ESP_OK;
        goto cleanup;
    }

    snprintf(id_text, POST_BUF_SIZE + 32, "%s:%s", id_buf, text);

    wifi_link_broadcast(from, id_text);

    const char *resp = "OK";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));
    result = ESP_OK;

cleanup:
    free(body);
    free(text);
    free(id_text);
    return result;
}

// ============================
// Запуск сервера
// ============================

esp_err_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 3;     // сек тишины до первой probe
    config.keep_alive_interval = 2; // интервал между probe
    config.keep_alive_count = 2;    // probe до признания мёртвым (~6 с)
    config.core_id = 0;
    config.max_open_sockets = HTTP_MAX_CLIENTS;
    config.max_uri_handlers = 5;
    config.lru_purge_enable = true;

    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return ESP_FAIL;
    }

    httpd_uri_t ping_uri = {
        .uri = "/ping",
        .method = HTTP_GET,
        .handler = ping_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t send_uri = {
        .uri = "/send",
        .method = HTTP_POST,
        .handler = send_post_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_http_server, &ping_uri);
    httpd_register_uri_handler(s_http_server, &info_uri);
    httpd_register_uri_handler(s_http_server, &send_uri);

    ESP_LOGI(TAG, "HTTP server started on port 80 (Core 0)");
    return ESP_OK;
}
