#pragma once
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
using gpio_num_t = int;
enum { GPIO_INTR_DISABLE, GPIO_MODE_OUTPUT, GPIO_MODE_INPUT,
       GPIO_PULLDOWN_DISABLE, GPIO_PULLUP_ENABLE };
struct gpio_config_t {
  int intr_type, mode;
  uint64_t pin_bit_mask;
  int pull_down_en, pull_up_en;
};
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
