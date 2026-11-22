#include "wifi_history.h"

#include <stdio.h>
#include <string.h>

#include "wifi_common.h"

// Кольцевой буфер разосланных сообщений в виде "from:text".
// Доступ под s_ws_mutex
static char s_history[HISTORY_SIZE][HISTORY_MAX_LEN];
static uint32_t s_history_count = 0;

void history_add(const char *payload) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);

    char *slot = s_history[s_history_count % HISTORY_SIZE];
    if (strlcpy(slot, payload, HISTORY_MAX_LEN) >= HISTORY_MAX_LEN) {
        // strlcpy режет по байтам: оборванная UTF-8 последовательность
        // в хвосте сломает декодирование кадра на стороне приложения
        size_t end = strlen(slot);
        size_t start = end;
        while (start > 0 && ((unsigned char)slot[start - 1] & 0xC0) == 0x80) {
            start--;
        }
        if (start > 0) {
            unsigned char lead = (unsigned char)slot[start - 1];
            if (lead & 0x80) {
                size_t need = ((lead & 0xE0) == 0xC0) ? 2 : ((lead & 0xF0) == 0xE0) ? 3
                                                        : ((lead & 0xF8) == 0xF0)   ? 4
                                                                                    : 1;
                if (start - 1 + need > end) {
                    slot[start - 1] = '\0';
                }
            }
        }
    }
    s_history_count++;

    xSemaphoreGive(s_ws_mutex);
}

// Кадры истории отличаются префиксом "hist:": приложение показывает их
// в чате, но не проигрывает по ним звук и не показывает уведомление
void history_send_to(int fd, wifi_history_send_fn send_fn) {
    // Статический: буфер больше килобайта не для стека httpd, доступ под s_ws_mutex
    static char frame[HISTORY_MAX_LEN + 8];

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);

    uint32_t total = s_history_count;
    uint32_t start = (total > HISTORY_SIZE) ? (total - HISTORY_SIZE) : 0;
    for (uint32_t i = start; i < total; i++) {
        const char *saved = s_history[i % HISTORY_SIZE];
        if (saved[0] == '\0') {
            continue;
        }
        int len = snprintf(frame, sizeof(frame), "hist:%s", saved);
        if (len <= 5) {
            continue;
        }
        if (len > (int)sizeof(frame) - 1) {
            len = (int)sizeof(frame) - 1;
        }
        send_fn(fd, frame, (size_t)len);
    }

    xSemaphoreGive(s_ws_mutex);
}
