#include "power_button.h"

#include <stdint.h>

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"

#define PMU_ADDRESS 0x34
#define PMU_ID_REG 0x03
#define PMU_ID_AXP2101 0x4A
#define PMU_IRQ_ENABLE_2 0x41
#define PMU_IRQ_STATUS_2 0x49
#define PMU_SHORT_PRESS_BIT 0x08

static bool available;

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(I2C_NUM_0, PMU_ADDRESS, &reg, 1,
                                        value, 1, pdMS_TO_TICKS(20));
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    return i2c_master_write_to_device(I2C_NUM_0, PMU_ADDRESS, command,
                                      sizeof(command), pdMS_TO_TICKS(20));
}

esp_err_t power_button_init(void)
{
    uint8_t value;
    esp_err_t err = read_register(PMU_ID_REG, &value);
    if (err != ESP_OK) return err;
    if (value != PMU_ID_AXP2101) return ESP_ERR_INVALID_RESPONSE;

    // AXP2101 short-press event is IRQ bit 11, in the second enable/status byte.
    err = write_register(PMU_IRQ_STATUS_2, PMU_SHORT_PRESS_BIT);
    if (err != ESP_OK) return err;
    err = read_register(PMU_IRQ_ENABLE_2, &value);
    if (err != ESP_OK) return err;
    err = write_register(PMU_IRQ_ENABLE_2, value | PMU_SHORT_PRESS_BIT);
    available = err == ESP_OK;
    return err;
}

bool power_button_take_short_press(void)
{
    if (!available) return false;
    uint8_t status;
    if (read_register(PMU_IRQ_STATUS_2, &status) != ESP_OK ||
        !(status & PMU_SHORT_PRESS_BIT)) return false;
    return write_register(PMU_IRQ_STATUS_2, PMU_SHORT_PRESS_BIT) == ESP_OK;
}
