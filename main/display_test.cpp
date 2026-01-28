
#include <stdio.h>
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
#include "SensorQMI8658.hpp"

// Debug logging config (set in sdkconfig, default off)
#ifdef CONFIG_APP_DEBUG_LOGGING
#define DEBUG_LOGGING 1
#else
#define DEBUG_LOGGING 0
#endif

#include "display_test_shared.h"

// --- BEGIN RESTORED FUNCTION PROTOTYPES, HELPERS, TASKS, ETC. ---
// Prototypes only, no type/struct/enum or static/global variable definitions here
static int scan_for_images(void);
static void touch_task(void *pvParameters);
static esp_err_t tca9554_write_reg(uint8_t reg, uint8_t value);
static esp_err_t tca9554_read_reg(uint8_t reg, uint8_t *value);
static esp_err_t tca9554_set_pin_direction(uint8_t pin_mask, bool output);
static esp_err_t tca9554_set_pin_level(uint8_t pin_mask, bool high);
static void fill_screen_color(uint16_t color);
static inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b);
static void calc_fit_scale(uint16_t src_w, uint16_t src_h, uint16_t *dst_w, uint16_t *dst_h, int16_t *x_off, int16_t *y_off);
static void scale_and_draw_rgb888(uint8_t *src, uint16_t src_w, uint16_t src_h);
static unsigned int tjpgd_input_func(JDEC *jd, uint8_t *buff, unsigned int nbyte);
static UINT tjpgd_output_func(JDEC *jd, void *bitmap, JRECT *rect);
static image_type_t get_image_type(const char *filename);
static esp_err_t display_jpeg(const char *path);
static void pngle_draw_callback(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *rgba);
static void pngle_init_callback(pngle_t *pngle, uint32_t w, uint32_t h);
static esp_err_t display_png(const char *path);
static esp_err_t display_gif(const char *path);
static esp_err_t display_bin(const char *path);
static esp_err_t display_image(const char *path);
static esp_err_t init_sd_card(void);
static void unmount_sd_card(void);
static void remount_sd_card(void);
static uint8_t* decode_image_to_buffer(const char *path, uint16_t *out_w, uint16_t *out_h);
static void preload_task(void *pvParameters);
static void show_next_content(int *color_idx);
void rotate_rgb888_90ccw(uint8_t *src, uint8_t *dst, uint16_t src_w, uint16_t src_h);
// --- END RESTORED FUNCTION PROTOTYPES, HELPERS, TASKS, ETC. ---

// --- FUNCTION IMPLEMENTATIONS (from display_test.c) ---

// --- BEGIN RESTORED FUNCTION IMPLEMENTATIONS, HELPERS, TASKS, LOGIC ---
#define TAG "display_test"
#define SKIP_APP_MAIN
#include "display_test.c" // For migration, include the C file directly (C++ compatible code only)
#undef SKIP_APP_MAIN
// --- END RESTORED FUNCTION IMPLEMENTATIONS, HELPERS, TASKS, LOGIC ---

// --- END FULL MIGRATION ---

extern "C" void app_main(void)
{
    ESP_EARLY_LOGI("APP", "==== ENTERED app_main() (first line) ====");
    ESP_EARLY_LOGI("APP", "==== app_main() after first line ====");
    ESP_EARLY_LOGI("APP", "Checkpoint 1");
    // Step 1: I2C init
    ESP_EARLY_LOGI("APP", "Checkpoint 2");
    // Step 2: IMU init
    ESP_EARLY_LOGI("APP", "Checkpoint 3");
    // Step 3: TCA9554 init
    ESP_EARLY_LOGI("APP", "Checkpoint 4");
    // Step 4: Display init
    ESP_EARLY_LOGI("APP", "Checkpoint 5");
    // Step 5: Touch init
    ESP_EARLY_LOGI("APP", "Checkpoint 6");
    // Step 6: SD card init
    ESP_EARLY_LOGI("APP", "Checkpoint 7");
    // Step 7: Preload/double-buffering init
    ESP_EARLY_LOGI("APP", "Checkpoint 8");
    // Step 8: Main loop start
    ESP_EARLY_LOGI("APP", "Checkpoint 9");
}
