#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Слоты WS-клиентов: регистрация по fd, имя из setName:, троттлинг.
// Инициализируется из wifi_link_init через clients_init().
void clients_init(void);
void client_register(int fd);
void client_set_name(int fd, const char *name);
bool client_get_name(int fd, char *out, size_t out_size);
void client_forget(int fd);
bool client_check_rate(int fd, uint32_t now_ms);

// Имя без ':' (разделитель кадра), не "System" и не префикс служебного
// кадра (hist/ping/pong): иначе клиент смог бы подделать служебные кадры
bool name_is_valid(const char *name);
