#include "wifi_clients.h"

#include <string.h>

#include "esp_log.h"

#include "wifi_common.h"

static const char *TAG = "WIFI_CLIENTS";

// Дескриптор 0 — валидный номер сокета, поэтому пустой слот помечаем -1
#define WS_FD_NONE (-1)

typedef struct {
    int fd;
    char name[WS_NAME_MAX];
    uint32_t last_msg_ms;
} ws_client_t;

// Состояние клиентов: имя из setName: и время последнего сообщения.
// Индекс — просто слот, ищем по fd
static ws_client_t s_ws_clients[WS_MAX_CLIENTS];

void clients_init(void) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_ws_clients[i].fd = WS_FD_NONE;
        s_ws_clients[i].name[0] = '\0';
        s_ws_clients[i].last_msg_ms = 0;
    }
}

// Слот клиента по fd; при отсутствии занимает свободный.
// Вызывать под s_ws_mutex
static ws_client_t *client_slot_locked(int fd) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            return &s_ws_clients[i];
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == WS_FD_NONE) {
            s_ws_clients[i].fd = fd;
            s_ws_clients[i].name[0] = '\0';
            s_ws_clients[i].last_msg_ms = 0;
            return &s_ws_clients[i];
        }
    }
    return NULL;
}

// Слот заводится сразу после рукопожатия: троттлинг работает по слоту,
// поэтому клиент, не присылающий setName:, иначе не был бы ограничен
void client_register(int fd) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (!client_slot_locked(fd)) {
        ESP_LOGW(TAG, "No free client slot for fd %d", fd);
    }
    xSemaphoreGive(s_ws_mutex);
}

void client_set_name(int fd, const char *name) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);

    ws_client_t *slot = client_slot_locked(fd);
    if (slot) {
        strlcpy(slot->name, name, sizeof(slot->name));
    } else {
        ESP_LOGW(TAG, "No free name slot for fd %d", fd);
    }

    xSemaphoreGive(s_ws_mutex);
}

// Копирует имя клиента в out. Возвращает false, если setName: не приходил
bool client_get_name(int fd, char *out, size_t out_size) {
    bool found = false;

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd && s_ws_clients[i].name[0] != '\0') {
            strlcpy(out, s_ws_clients[i].name, out_size);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);

    return found;
}

void client_forget(int fd) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            s_ws_clients[i].fd = WS_FD_NONE;
            s_ws_clients[i].name[0] = '\0';
            s_ws_clients[i].last_msg_ms = 0;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

bool client_check_rate(int fd, uint32_t now_ms) {
    bool allowed = true;

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    ws_client_t *slot = client_slot_locked(fd);
    if (slot) {
        if (slot->last_msg_ms != 0 &&
            (now_ms - slot->last_msg_ms) < WS_MIN_MSG_INTERVAL_MS) {
            allowed = false;
        } else {
            slot->last_msg_ms = now_ms;
        }
    } else {
        // Слотов нет — считаем, что клиентов и так больше, чем нужно
        allowed = false;
    }
    xSemaphoreGive(s_ws_mutex);

    return allowed;
}

bool name_is_valid(const char *name) {
    if (name[0] == '\0' || strchr(name, ':') != NULL) {
        return false;
    }
    if (strncmp(name, "System", 6) == 0) {
        return false;
    }
    static const char *reserved[] = {"hist", "ping", "pong"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(name, reserved[i]) == 0) {
            return false;
        }
    }
    return true;
}
