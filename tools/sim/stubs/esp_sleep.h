#pragma once
#include <stdint.h>
typedef int gpio_num_t;
#define ESP_SLEEP_WAKEUP_ALL 0
#define ESP_SLEEP_WAKEUP_EXT0 2
inline void esp_sleep_disable_wakeup_source(int) {}
inline void esp_sleep_enable_timer_wakeup(uint64_t) {}
inline void esp_sleep_enable_ext0_wakeup(gpio_num_t, int) {}
inline int esp_light_sleep_start() { return 0; }
inline int esp_sleep_get_wakeup_cause() { return 0; }
