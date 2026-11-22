#pragma once

#include <stddef.h>
#include <stdint.h>

// Доставка одного кадра клиенту: реализуется в wifi_ws (ws_queue_text),
// чтобы модуль истории не зависел от WebSocket-сервера.
typedef void (*wifi_history_send_fn)(int fd, const char *payload, size_t len);

// Кольцевой буфер разосланных сообщений в виде "from:text".
// Доступ под общим s_ws_mutex.
void history_add(const char *payload);

// Досылает всю историю клиенту с префиксом "hist:" через send_fn.
// Вызывается из wifi_ws после рукопожатия.
void history_send_to(int fd, wifi_history_send_fn send_fn);
