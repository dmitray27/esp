#include "wifi_link.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "wifi_common.h"
#include "wifi_clients.h"
#include "wifi_http.h"
#include "wifi_ws.h"

static const char *TAG = "WIFI_LINK";

static QueueHandle_t s_tx_queue = NULL;
SemaphoreHandle_t s_ws_mutex = NULL;

// Собран в wifi_link_init, читается HTTP /info через extern в wifi_common.h
char s_ssid[33] = {0};

// К префиксу добавляются последние два байта MAC точки доступа:
// так несколько плат рядом не дают одинаковый SSID
#define WIFI_SSID_PREFIX "AFSK-TRX-"
// Пароль точки доступа задаётся в menuconfig (AFSK Wi-Fi Access Point),
// чтобы он не лежал в исходниках и не был одинаковым на всех платах
#define WIFI_PASS CONFIG_AFSK_AP_PASSWORD
#define WIFI_CHANNEL 1
#define WIFI_MAX_STA 4
#define WIFI_INACTIVE_TIME_S 30

_Static_assert(CONFIG_LWIP_MAX_SOCKETS >=
                   HTTP_MAX_CLIENTS + WS_MAX_CLIENTS + 2 * HTTPD_INTERNAL_SOCKETS + NETWORK_SOCKET_RESERVE,
               "CONFIG_LWIP_MAX_SOCKETS must cover both HTTP servers, their clients, and a reserve");

QueueHandle_t wifi_link_get_tx_queue(void) {
    return s_tx_queue;
}

bool wifi_link_message_fits(const char *from, const char *id, const char *text) {
    /* Тот же лимит считает клиент (messageFitsFrame в chat_protocol.dart) */
    size_t len = strlen(from) + 1 + strlen(text);
    if (id && id[0] != '\0') {
        len += strlen(id) + 1;
    }
    return len <= WS_MAX_FRAME_LEN;
}

// ============================
// Wi-Fi и запуск серверов
// ============================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected, AID=%d", MAC2STR(evt->mac), evt->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected, AID=%d", MAC2STR(evt->mac), evt->aid);
    }
}

void wifi_link_init(void) {
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_queue_item_t));
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return;
    }

    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex) {
        ESP_LOGE(TAG, "Failed to create WS mutex");
        return;
    }

    clients_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));

    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));

    wifi_config_t wifi_config = {0};
    int written = snprintf(s_ssid, sizeof(s_ssid), "%s%02X%02X",
                           WIFI_SSID_PREFIX, mac[4], mac[5]);
    size_t ssid_len = (written < 0) ? 0 : (size_t)written;
    if (ssid_len > sizeof(wifi_config.ap.ssid)) {
        ssid_len = sizeof(wifi_config.ap.ssid);
    }

    memcpy(wifi_config.ap.ssid, s_ssid, ssid_len);
    wifi_config.ap.ssid_len = ssid_len;
    strlcpy((char *)wifi_config.ap.password, WIFI_PASS,
            sizeof(wifi_config.ap.password));
    wifi_config.ap.channel = WIFI_CHANNEL;
    wifi_config.ap.max_connection = WIFI_MAX_STA;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_inactive_time(WIFI_IF_AP, WIFI_INACTIVE_TIME_S));

    ESP_LOGI(TAG, "Wi-Fi AP started: SSID=%s, IP=" WIFI_AP_IP, s_ssid);

    if (start_http_server() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
    }
    if (start_ws_server() != ESP_OK) {
        ESP_LOGE(TAG, "WS server failed to start");
    }
}
