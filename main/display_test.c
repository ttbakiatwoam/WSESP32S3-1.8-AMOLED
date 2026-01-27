/*
 * Touch-triggered color cycle test for RM67162 QSPI display
 * Waveshare ESP32-S3 1.8" AMOLED
 * Touch the screen to cycle through colors
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "rm67162_qspi.h"
#include "esp_lcd_rm67162.h"
#include "esp_lcd_panel_ops.h"
#include "cst816t.h"

static const char *TAG = "touch_color_test";

// Waveshare 1.8" AMOLED pin definitions
#define PIN_LCD_CS      12
#define PIN_LCD_SCK     11
#define PIN_LCD_D0      4
#define PIN_LCD_D1      5
#define PIN_LCD_D2      6
#define PIN_LCD_D3      7
#define PIN_LCD_RST     13

#define DISPLAY_WIDTH   368
#define DISPLAY_HEIGHT  448

// I2C pins for TCA9554 GPIO expander and CST816T touch
#define PIN_I2C_SCL     14
#define PIN_I2C_SDA     15
#define I2C_MASTER_NUM  I2C_NUM_0
#define I2C_FREQ_HZ     400000

// Touch pins
#define PIN_TOUCH_INT   21

// TCA9554 I2C address
#define TCA9554_ADDR    0x20

// Color definitions (RGB565)
static const uint16_t colors[] = {
    0xF800, // Red
    0x07E0, // Green
    0x001F, // Blue
    0xFFFF, // White
    0xF81F, // Magenta
    0x07FF, // Cyan
    0xFFE0, // Yellow
};
static const char *color_names[] = {"RED", "GREEN", "BLUE", "WHITE", "MAGENTA", "CYAN", "YELLOW"};
#define NUM_COLORS (sizeof(colors) / sizeof(colors[0]))

static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t *draw_buffer = NULL;

static esp_err_t tca9554_write_pin(uint8_t pin, uint8_t value)
{
    uint8_t config_reg = 0x03;  // Configuration register
    uint8_t output_reg = 0x01;  // Output port register
    uint8_t config_data, output_data;
    
    // Read current configuration
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, config_reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &config_data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;
    
    // Set pin as output
    config_data &= ~(1 << pin);
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, config_reg, true);
    i2c_master_write_byte(cmd, config_data, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;
    
    // Read current output
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, output_reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &output_data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;
    
    // Set pin value
    if (value) {
        output_data |= (1 << pin);
    } else {
        output_data &= ~(1 << pin);
    }
    
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, output_reg, true);
    i2c_master_write_byte(cmd, output_data, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    
    return ret;
}

static void fill_screen_color(uint16_t color)
{
    if (!draw_buffer || !panel_handle) return;
    
    for (int i = 0; i < DISPLAY_WIDTH * 10; i++) {
        draw_buffer[i] = color;
    }
    for (int y = 0; y < DISPLAY_HEIGHT; y += 10) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + 10, draw_buffer);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  Touch-Triggered Color Cycle Test");
    ESP_LOGI(TAG, "  Waveshare ESP32-S3 1.8\" AMOLED");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Touch the screen to cycle through colors!");
    
    // Initialize I2C for TCA9554 GPIO expander
    ESP_LOGI(TAG, "Initializing I2C on SCL=%d, SDA=%d...", PIN_I2C_SCL, PIN_I2C_SDA);
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0));
    
    // Enable AMOLED display via TCA9554 pin 1 (AMOLED_EN)
    ESP_LOGI(TAG, "Enabling AMOLED display via TCA9554 pin 1...");
    ESP_ERROR_CHECK(tca9554_write_pin(1, 1));
    vTaskDelay(pdMS_TO_TICKS(10));
    
    void *qspi_ctx = NULL;
    
    // Configure RM67162 QSPI
    rm67162_qspi_config_t qspi_config = {
        .cs_gpio = PIN_LCD_CS,
        .sck_gpio = PIN_LCD_SCK,
        .d0_gpio = PIN_LCD_D0,
        .d1_gpio = PIN_LCD_D1,
        .d2_gpio = PIN_LCD_D2,
        .d3_gpio = PIN_LCD_D3,
        .reset_gpio = PIN_LCD_RST,
        .pclk_hz = 80 * 1000 * 1000,
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .spi_host = SPI2_HOST,
    };
    
    ESP_LOGI(TAG, "Initializing QSPI display...");
    ESP_ERROR_CHECK(rm67162_qspi_init(&qspi_config, &qspi_ctx, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    ESP_LOGI(TAG, "Display initialized!");
    
    // Allocate draw buffer
    draw_buffer = heap_caps_malloc(DISPLAY_WIDTH * 10 * 2, MALLOC_CAP_DMA);
    if (!draw_buffer) {
        ESP_LOGE(TAG, "Failed to allocate draw buffer!");
        return;
    }
    
    // Reset touch controller via TCA9554 pin 2
    ESP_LOGI(TAG, "Resetting touch controller via TCA9554 pin 2...");
    tca9554_write_pin(2, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    tca9554_write_pin(2, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Scan I2C to verify devices
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
        }
    }
    
    // Initialize touch controller
    ESP_LOGI(TAG, "Initializing touch controller at 0x38...");
    cst816t_config_t touch_config = {
        .i2c_port = I2C_MASTER_NUM,
        .i2c_addr = 0x38,
        .int_gpio = PIN_TOUCH_INT,
        .rst_gpio = -1,  // Reset done via TCA9554
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .swap_xy = false,
        .invert_x = false,
        .invert_y = false,
    };
    
    cst816t_handle_t touch_handle = NULL;
    esp_err_t touch_ret = cst816t_init(&touch_config, &touch_handle);
    
    if (touch_ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch init FAILED! Error: 0x%x", touch_ret);
        ESP_LOGI(TAG, "Display will auto-cycle colors instead.");
        
        // Fall back to auto-cycling
        int color_idx = 0;
        while (1) {
            ESP_LOGI(TAG, "Auto: %s", color_names[color_idx]);
            fill_screen_color(colors[color_idx]);
            color_idx = (color_idx + 1) % NUM_COLORS;
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  TOUCH INITIALIZED SUCCESSFULLY!");
    ESP_LOGI(TAG, "  Touch the screen to change colors");
    ESP_LOGI(TAG, "=========================================");
    
    // Start with first color
    int current_color = 0;
    ESP_LOGI(TAG, "Starting color: %s", color_names[current_color]);
    fill_screen_color(colors[current_color]);
    
    bool was_touching = false;
    
    // Main loop - change color on touch
    while (1) {
        cst816t_touch_data_t touch_data;
        esp_err_t ret = cst816t_read_touch(touch_handle, &touch_data);
        
        if (ret == ESP_OK && touch_data.event == 0) {  // Touch down
            if (!was_touching) {
                // New touch - cycle color
                current_color = (current_color + 1) % NUM_COLORS;
                ESP_LOGI(TAG, "TOUCH at (%d, %d) -> %s", 
                         touch_data.x, touch_data.y, color_names[current_color]);
                fill_screen_color(colors[current_color]);
                was_touching = true;
            }
        } else {
            was_touching = false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
