#include "esp_log.h"
#include "board.h"

static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t board_i2c(void)
{
    if (!s_bus) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0, .sda_io_num = BOARD_I2C_SDA, .scl_io_num = BOARD_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    }
    return s_bus;
}
