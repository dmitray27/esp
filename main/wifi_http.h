#pragma once

#include "esp_err.h"

// HTTP-сервер на :80 (/ping, /info, POST /send).
// Запускается из wifi_link_init через start_http_server().
esp_err_t start_http_server(void);
