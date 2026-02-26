/*
 * Quick hardware test for RM67162 QSPI display
 * Waveshare ESP32-S3 1.8" AMOLED
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

static const char *TAG = "display_test";

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

// I2C pins for TCA9554 GPIO expander
#define PIN_I2C_SCL     14
#define PIN_I2C_SDA     15
#define I2C_MASTER_NUM  I2C_NUM_0
#define I2C_FREQ_HZ     400000

// TCA9554 I2C address
#define TCA9554_ADDR    0x20

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

void app_main(void)
{
    ESP_LOGI(TAG, "=== RM67162 QSPI Display Test ===");
    ESP_LOGI(TAG, "Waveshare ESP32-S3 1.8\" AMOLED");
    
    // Initialize I2C for TCA9554 GPIO expander
    ESP_LOGI(TAG, "Initializing I2C...");
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
    ESP_LOGI(TAG, "Enabling AMOLED display via TCA9554...");
    ESP_ERROR_CHECK(tca9554_write_pin(1, 1));  // TFT_CDE = 1
    vTaskDelay(pdMS_TO_TICKS(10));
    
    void *qspi_ctx = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;
    
    // Configure RM67162 QSPI
    rm67162_qspi_config_t qspi_config = {
        .cs_gpio = PIN_LCD_CS,
        .sck_gpio = PIN_LCD_SCK,
        .d0_gpio = PIN_LCD_D0,
        .d1_gpio = PIN_LCD_D1,
        .d2_gpio = PIN_LCD_D2,
        .d3_gpio = PIN_LCD_D3,
        .reset_gpio = PIN_LCD_RST,
        .pclk_hz = 80 * 1000 * 1000,  // 80 MHz
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .spi_host = SPI2_HOST,
    };
    
    ESP_LOGI(TAG, "Initializing QSPI bus and creating panel...");
    ESP_ERROR_CHECK(rm67162_qspi_init(&qspi_config, &qspi_ctx, &panel_handle));
    
    ESP_LOGI(TAG, "Resetting panel...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    
    ESP_LOGI(TAG, "Initializing panel...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    ESP_LOGI(TAG, "Display initialized successfully!");
    
    // Fill screen with colors to test
    ESP_LOGI(TAG, "Testing display with color patterns...");
    
    // Allocate a small buffer for testing
    size_t buffer_size = DISPLAY_WIDTH * 10 * 2; // 10 lines at 16bpp
    uint16_t *buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    
    if (buffer) {
        // Red
        ESP_LOGI(TAG, "Filling with RED...");
        for (int i = 0; i < DISPLAY_WIDTH * 10; i++) {
            buffer[i] = 0xF800; // Red in RGB565
        }
        for (int y = 0; y < DISPLAY_HEIGHT; y += 10) {
            esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + 10, buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Green
        ESP_LOGI(TAG, "Filling with GREEN...");
        for (int i = 0; i < DISPLAY_WIDTH * 10; i++) {
            buffer[i] = 0x07E0; // Green in RGB565
        }
        for (int y = 0; y < DISPLAY_HEIGHT; y += 10) {
            esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + 10, buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Blue
        ESP_LOGI(TAG, "Filling with BLUE...");
        for (int i = 0; i < DISPLAY_WIDTH * 10; i++) {
            buffer[i] = 0x001F; // Blue in RGB565
        }
        for (int y = 0; y < DISPLAY_HEIGHT; y += 10) {
            esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + 10, buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // White
        ESP_LOGI(TAG, "Filling with WHITE...");
        for (int i = 0; i < DISPLAY_WIDTH * 10; i++) {
            buffer[i] = 0xFFFF; // White in RGB565
        }
        for (int y = 0; y < DISPLAY_HEIGHT; y += 10) {
            esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + 10, buffer);
        }
        
        free(buffer);
        ESP_LOGI(TAG, "Display test complete!");
    } else {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
    }
    
    ESP_LOGI(TAG, "Test finished. Display should be white.");
}
