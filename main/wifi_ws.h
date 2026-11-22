#pragma once

#include "esp_err.h"

// WebSocket-сервер на :81: рукопожатие, разбор кадров, рассылка.
// Запускается из wifi_link_init через start_ws_server().
esp_err_t start_ws_server(void);
