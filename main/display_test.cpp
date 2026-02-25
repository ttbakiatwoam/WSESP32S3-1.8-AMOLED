
#include <stdio.h>
#include <cmath>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp32s3/rom/uart.h"

extern "C" void esp_early_logi_global(const char* tag, const char* msg) {
    ets_printf("[EARLY][%s] %s\n", tag, msg);
}

__attribute__((constructor)) void early_global_ctor() {
    esp_early_logi_global("GLOBAL", "==== ENTERED global static constructor ====");
}
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "rm67162_qspi.h"
#include "esp_lcd_rm67162.h"
#include "esp_lcd_panel_ops.h"
#include "cst816t.h"
#include "esp32s3/rom/tjpgd.h"
#include "pngle.h"
#include "gifdec.h"
#include "esp_timer.h"

// Provide Arduino-compat millis() and delay() for SensorLib (ESP-IDF only)
#ifndef ARDUINO
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static inline unsigned long millis() {
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}
static inline void delay(unsigned long ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
#endif

#include "SensorQMI8658.hpp"

// Debug logging config (set in sdkconfig, default off)
#ifdef CONFIG_APP_DEBUG_LOGGING
#define DEBUG_LOGGING 1
#else
#define DEBUG_LOGGING 0
#endif

#include "display_test_shared.h"

// Missing defines from original C app
#define I2C_FREQ_HZ     400000

// C++ globals: IMU
static SensorQMI8658 imu;
static int last_orientation = BASE_ORIENTATION;

// IMU orientation task (C++ - uses imu object)
// MADCTL is kept fixed; rotation is done in software when rendering images.
static void imu_orientation_task(void *pvParameters) {
    float acc_x = 0, acc_y = 0, acc_z = 0;
    const float THRESHOLD = 0.4f;  // Hysteresis threshold to prevent jitter
    int debounce_count = 0;
    int pending_orientation = last_orientation;
    const int DEBOUNCE_REQUIRED = 3;  // Need 3 consecutive consistent readings (~600ms)
    while (1) {
        imu.getAccelerometer(acc_x, acc_y, acc_z);
        int orientation = last_orientation;  // Default: keep current
        float ax = fabs(acc_x);
        float ay = fabs(acc_y);
        // Only change orientation if dominant axis exceeds threshold
        if (ax > ay + THRESHOLD) {
            orientation = (acc_x > 0) ? 1 : 3;
        } else if (ay > ax + THRESHOLD) {
            orientation = (acc_y > 0) ? 2 : 0;
        }
        // Debounce: require multiple consistent readings before committing change
        if (orientation != last_orientation) {
            if (orientation == pending_orientation) {
                debounce_count++;
            } else {
                pending_orientation = orientation;
                debounce_count = 1;
            }
            if (debounce_count >= DEBOUNCE_REQUIRED) {
                last_orientation = orientation;
                current_orientation = orientation;
                orientation_changed = true;
                debounce_count = 0;
                ESP_LOGI("imu", "Orientation changed: %d (rotation=%d)", 
                         orientation, (orientation - BASE_ORIENTATION + 4) % 4);
            }
        } else {
            debounce_count = 0;
            pending_orientation = last_orientation;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// --- C function implementations (included from display_test.c) ---
#define TAG "display_test"
#include "display_test.c"

// --- END FULL MIGRATION ---

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  SD Card Image Display Test");
    ESP_LOGI(TAG, "  Supports: JPEG, PNG, GIF, BIN");
    ESP_LOGI(TAG, "  Waveshare ESP32-S3 1.8\" AMOLED");
    ESP_LOGI(TAG, "=========================================");
    esp_log_level_set("*", ESP_LOG_VERBOSE);

    // Initialize I2C (will be configured by SensorLib's imu.begin(), 
    // but we set up the bus first for TCA9554 and other I2C devices)
    ESP_LOGI(TAG, "Initializing I2C...");
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = I2C_FREQ_HZ },
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0));
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow I2C bus to stabilize

    // I2C bus scan for debugging
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  Found I2C device at 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan complete.");

    // TCA9554 pin masks (must init BEFORE IMU — TCA9554 controls display power)
    #define PIN_MASK_0  (1 << 0)
    #define PIN_MASK_1  (1 << 1)
    #define PIN_MASK_2  (1 << 2)
    #define PIN_MASK_7  (1 << 7)

    // Configure TCA9554 I/O expander first — it controls display power/reset
    ESP_LOGI(TAG, "Configuring TCA9554 at I2C addr 0x%02X...", TCA9554_ADDR);
    uint8_t output_pins = PIN_MASK_0 | PIN_MASK_1 | PIN_MASK_2 | PIN_MASK_7;
    esp_err_t tca_ret = tca9554_set_pin_direction(output_pins, true);
    if (tca_ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 set_pin_direction failed: 0x%x (%s)", tca_ret, esp_err_to_name(tca_ret));
        ESP_LOGE(TAG, "Cannot initialize display without TCA9554, aborting");
        return;
    }
    tca_ret = tca9554_set_pin_level(output_pins, false);
    if (tca_ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 set_pin_level(false) failed: 0x%x (%s)", tca_ret, esp_err_to_name(tca_ret));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    tca_ret = tca9554_set_pin_level(output_pins, true);
    if (tca_ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 set_pin_level(true) failed: 0x%x (%s)", tca_ret, esp_err_to_name(tca_ret));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    // Initialize QMI8658 IMU (optional — won't block display if absent)
    // Note: imu.begin() will try to re-install I2C driver (which is already installed)
    // The SensorLib ignores the re-install error internally, so this is safe.
    if (!imu.begin(I2C_MASTER_NUM, 0x6B, PIN_I2C_SDA, PIN_I2C_SCL)) {
        ESP_LOGW(TAG, "QMI8658 IMU not found (may be normal if not connected)");
    } else {
        ESP_LOGI(TAG, "QMI8658 IMU initialized, chip ID: 0x%02X", imu.getChipID());
        imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_250Hz, SensorQMI8658::LPF_MODE_0, true);
        imu.configGyroscope(SensorQMI8658::GYR_RANGE_256DPS, SensorQMI8658::GYR_ODR_224_2Hz, SensorQMI8658::LPF_MODE_0, true);
        imu.enableAccelerometer();
        imu.enableGyroscope();
        ESP_LOGI(TAG, "IMU accel enabled: %d, gyro enabled: %d",
                 imu.isEnableAccelerometer(), imu.isEnableGyroscope());
        xTaskCreatePinnedToCore(imu_orientation_task, "imu_orientation_task", 4096, NULL, 5, NULL, 0);
    }

    // Initialize display
    void *qspi_ctx = NULL;
    rm67162_qspi_config_t qspi_config = {
        .cs_gpio = PIN_LCD_CS,
        .sck_gpio = PIN_LCD_SCK,
        .d0_gpio = PIN_LCD_D0,
        .d1_gpio = PIN_LCD_D1,
        .d2_gpio = PIN_LCD_D2,
        .d3_gpio = PIN_LCD_D3,
        .reset_gpio = PIN_LCD_RST,
        .pclk_hz = 80 * 1000 * 1000,
        .width = PORTRAIT_WIDTH,
        .height = PORTRAIT_HEIGHT,
        .spi_host = SPI2_HOST,
    };

    ESP_LOGI(TAG, "Initializing display...");
    ESP_ERROR_CHECK(rm67162_qspi_init(&qspi_config, &qspi_ctx, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Set display rotation (90° counter-clockwise, alternate mirror)
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));

    // Allocate draw buffer — use max dimension for landscape support
    int max_dim = (PORTRAIT_WIDTH > PORTRAIT_HEIGHT) ? PORTRAIT_WIDTH : PORTRAIT_HEIGHT;
    draw_buffer = (uint16_t*)heap_caps_malloc(max_dim * 16 * 2, MALLOC_CAP_DMA);
    if (!draw_buffer) {
        ESP_LOGE(TAG, "Failed to allocate draw buffer!");
        return;
    }

    ESP_LOGI(TAG, "Display initialized! Forcing white fill to test display...");
    fill_screen_color(0xFFFF);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Touch controller
    ESP_LOGI(TAG, "Waiting 200ms before initializing touch controller...");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Initializing touch...");
    cst816t_config_t touch_config = {
        .i2c_port = I2C_MASTER_NUM,
        .i2c_addr = 0x38,
        .int_gpio = PIN_TOUCH_INT,
        .rst_gpio = (gpio_num_t)-1,
        .width = PORTRAIT_WIDTH,
        .height = PORTRAIT_HEIGHT,
        .swap_xy = false,
        .invert_x = false,
        .invert_y = false,
    };

    if (touch_config.rst_gpio == (gpio_num_t)-1) {
        ESP_LOGW(TAG, "Touch config: rst_gpio is -1, hardware reset will NOT be performed.");
    }

    cst816t_handle_t touch_handle = NULL;
    esp_err_t touch_ret = cst816t_init(&touch_config, &touch_handle);
    if (touch_ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed (err=0x%x), continuing without touch", touch_ret);
    } else {
        uint8_t chip_id = 0;
        if (cst816t_get_chip_id(touch_handle, &chip_id) == ESP_OK) {
            ESP_LOGI(TAG, "CST816T chip ID: 0x%02X", chip_id);
        } else {
            ESP_LOGW(TAG, "Failed to read CST816T chip ID after init");
        }
    }

    global_touch_handle = touch_handle;

    if (touch_handle) {
        xTaskCreatePinnedToCore(
            touch_task, "touch_task", 8192, touch_handle, 5, NULL, 0
        );
        ESP_LOGI(TAG, "Touch task created");
    }

    // Initialize SD card
    bool sd_ok = (init_sd_card() == ESP_OK);

    if (sd_ok) {
        int found = scan_for_images();
        if (found > 0) {
            use_images = true;
            ESP_LOGI(TAG, "=========================================");
            ESP_LOGI(TAG, "  Found %d images!", found);
            ESP_LOGI(TAG, "  Tap: next image | Long press: unmount SD");
            ESP_LOGI(TAG, "  Double-tap: remount SD card");
            ESP_LOGI(TAG, "=========================================");
            current_image = 0;
            display_image(image_paths[current_image]);
        }
    }

    if (!use_images) {
        ESP_LOGI(TAG, "No images found on SD card");
        fill_screen_color(0xFFE0);  // Yellow - no images
    }

    // Create mutex for double-buffering
    preload_mutex = xSemaphoreCreateMutex();
    if (!preload_mutex) {
        ESP_LOGE(TAG, "Failed to create preload_mutex!");
    }
    xTaskCreatePinnedToCore(
        preload_task, "preload_task", 8192, NULL, 5, NULL, 0
    );
    ESP_LOGI(TAG, "Preload task created");

    // Main loop - handle touch events and orientation changes
    while (1) {
        // Check for orientation change — redraw current image
        if (orientation_changed) {
            orientation_changed = false;
            ESP_LOGI(TAG, "Orientation changed — redrawing current content");
            if (use_images && num_images > 0 && !animation_running) {
                display_image(image_paths[current_image]);
            }
        }

        touch_event_t event = pending_touch_event;
        if (event != TOUCH_EVENT_NONE) {
            ESP_LOGI(TAG, "Main loop: received event %d", event);
            pending_touch_event = TOUCH_EVENT_NONE;
            switch (event) {
                case TOUCH_EVENT_TAP:
                    ESP_LOGI(TAG, "Main loop: handling TAP");
                    show_next_content(NULL);
                    break;
                case TOUCH_EVENT_DOUBLE_TAP:
                    ESP_LOGI(TAG, "Main loop: handling DOUBLE TAP");
                    remount_sd_card();
                    break;
                case TOUCH_EVENT_LONG_PRESS:
                    ESP_LOGI(TAG, "Main loop: handling LONG PRESS");
                    unmount_sd_card();
                    break;
                default:
                    break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
