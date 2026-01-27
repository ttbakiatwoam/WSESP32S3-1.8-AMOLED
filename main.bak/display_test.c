/*
 * Quick hardware test for RM67162 QSPI display
 * Waveshare ESP32-S3 1.8" AMOLED
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
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

void app_main(void)
{
    ESP_LOGI(TAG, "=== RM67162 QSPI Display Test ===");
    ESP_LOGI(TAG, "Waveshare ESP32-S3 1.8\" AMOLED");
    
    esp_lcd_panel_io_handle_t io_handle = NULL;
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
    
    ESP_LOGI(TAG, "Initializing QSPI bus...");
    ESP_ERROR_CHECK(rm67162_qspi_init(&qspi_config, &io_handle, &panel_handle));
    
    // Create LCD panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    
    ESP_LOGI(TAG, "Creating RM67162 panel...");
    ESP_ERROR_CHECK(esp_lcd_new_panel_rm67162(io_handle, &panel_config, &panel_handle));
    
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
