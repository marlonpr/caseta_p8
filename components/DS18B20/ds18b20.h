#pragma once
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin;
} ds18b20_t;


esp_err_t ds18b20_init(ds18b20_t *sensor, gpio_num_t pin);
esp_err_t ds18b20_read_temperature(ds18b20_t *sensor, float *temperature);
esp_err_t ds18b20_read_temperature_int(ds18b20_t *sensor, int16_t *temperature);

#ifdef __cplusplus
}
#endif
