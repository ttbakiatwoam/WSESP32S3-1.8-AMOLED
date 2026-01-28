#pragma once

// Shared types, enums, and statics for display_test.c/cpp
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_types.h"
#include "sdmmc_cmd.h"
#include "cst816t.h"

#define MAX_IMAGES 64
#define MAX_PATH_LEN 280
#define IMAGE_BUFFER_COUNT 2

// Display and hardware pin configuration (shared)

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/gpio.h"
#include "driver/i2c.h"

static const int PORTRAIT_WIDTH  = 368;
static const int PORTRAIT_HEIGHT = 448;
static const gpio_num_t PIN_SD_DATA  = GPIO_NUM_3;
static const gpio_num_t PIN_SD_CLK   = GPIO_NUM_14;
static const gpio_num_t PIN_SD_CMD   = GPIO_NUM_15;
static const gpio_num_t PIN_I2C_SDA  = GPIO_NUM_8;
static const gpio_num_t PIN_I2C_SCL  = GPIO_NUM_9;
static const gpio_num_t PIN_LCD_CS   = GPIO_NUM_6;
static const gpio_num_t PIN_LCD_SCK  = GPIO_NUM_7;
static const gpio_num_t PIN_LCD_D0   = GPIO_NUM_10;
static const gpio_num_t PIN_LCD_D1   = GPIO_NUM_11;
static const gpio_num_t PIN_LCD_D2   = GPIO_NUM_12;
static const gpio_num_t PIN_LCD_D3   = GPIO_NUM_13;
static const gpio_num_t PIN_LCD_RST  = GPIO_NUM_5;
static const i2c_port_t I2C_MASTER_NUM = I2C_NUM_0;
static const uint8_t TCA9554_ADDR = 0x20;
#define MOUNT_POINT     "/sdcard"
#define IMAGES_DIR      "/sdcard/images"

#ifdef __cplusplus
}
#endif

// Image types
typedef enum {
    IMG_TYPE_UNKNOWN,
    IMG_TYPE_BIN,
    IMG_TYPE_JPEG,
    IMG_TYPE_PNG,
    IMG_TYPE_GIF
} image_type_t;

// Touch gesture events
typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_TAP,
    TOUCH_EVENT_DOUBLE_TAP,
    TOUCH_EVENT_LONG_PRESS
} touch_event_t;

// Shared statics (extern for C/C++)
#ifdef __cplusplus
extern "C" {
#endif
extern char image_paths[MAX_IMAGES][MAX_PATH_LEN];
extern int num_images;
extern int current_image;
extern esp_lcd_panel_handle_t panel_handle;
extern uint16_t *draw_buffer;
extern sdmmc_card_t *sd_card;
extern bool use_images;
extern bool sd_mounted;
extern cst816t_handle_t global_touch_handle;
extern bool stop_animation;
extern bool animation_running;
extern uint8_t *image_buffers[IMAGE_BUFFER_COUNT];
extern int active_image_buffer;
extern int preload_image_index;
extern bool preload_ready;
extern SemaphoreHandle_t preload_mutex;
extern volatile touch_event_t pending_touch_event;
#ifdef __cplusplus
}
#endif
