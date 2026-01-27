/*
 * SD Card Image Display Test for RM67162 QSPI display
 * Waveshare ESP32-S3 1.8" AMOLED
 * Touch the screen to cycle through images on SD card
 * 
 * Supports: JPEG, PNG, GIF, and raw RGB565 .bin files
 */

#include <stdio.h>
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

static const char *TAG = "sd_image_test";

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

// SD Card SDMMC 1-bit mode pins
#define PIN_SD_CLK      2
#define PIN_SD_CMD      1
#define PIN_SD_DATA     3

// Mount point for SD card
#define MOUNT_POINT     "/sdcard"
#define IMAGES_DIR      "/sdcard/images"

// Maximum number of images to track
#define MAX_IMAGES      64
#define MAX_PATH_LEN    280

// Image types
typedef enum {
    IMG_TYPE_UNKNOWN,
    IMG_TYPE_BIN,
    IMG_TYPE_JPEG,
    IMG_TYPE_PNG,
    IMG_TYPE_GIF
} image_type_t;

// Image list
static char image_paths[MAX_IMAGES][MAX_PATH_LEN];
static int num_images = 0;
static int current_image = 0;

static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t *draw_buffer = NULL;
static sdmmc_card_t *sd_card = NULL;
static bool use_images = false;
static bool sd_mounted = false;
static cst816t_handle_t global_touch_handle = NULL;  // For GIF animation touch detection
static bool stop_animation = false;
static bool animation_running = false;

// TCA9554 register addresses
#define TCA9554_REG_OUTPUT   0x01
#define TCA9554_REG_CONFIG   0x03

static esp_err_t tca9554_write_reg(uint8_t reg, uint8_t value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t tca9554_read_reg(uint8_t reg, uint8_t *value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, value, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t tca9554_set_pin_direction(uint8_t pin_mask, bool output)
{
    uint8_t config;
    esp_err_t ret = tca9554_read_reg(TCA9554_REG_CONFIG, &config);
    if (ret != ESP_OK) return ret;
    
    if (output) {
        config &= ~pin_mask;
    } else {
        config |= pin_mask;
    }
    return tca9554_write_reg(TCA9554_REG_CONFIG, config);
}

static esp_err_t tca9554_set_pin_level(uint8_t pin_mask, bool high)
{
    uint8_t output;
    esp_err_t ret = tca9554_read_reg(TCA9554_REG_OUTPUT, &output);
    if (ret != ESP_OK) return ret;
    
    if (high) {
        output |= pin_mask;
    } else {
        output &= ~pin_mask;
    }
    return tca9554_write_reg(TCA9554_REG_OUTPUT, output);
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

// Convert RGB888 to RGB565 for display
// Display with MADCTL=0x00 expects standard RGB565, byte-swapped for big-endian interface
static inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    // Standard RGB565: RRRRRGGG GGGBBBBB
    uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    // Byte-swap for big-endian SPI interface
    return (color >> 8) | (color << 8);
}

// Calculate scale to fit image in display while filling as much as possible
// Returns scaled dimensions and offsets for centering
static void calc_fit_scale(uint16_t src_w, uint16_t src_h, 
                           uint16_t *dst_w, uint16_t *dst_h,
                           int16_t *x_off, int16_t *y_off)
{
    // Calculate scale factors (using fixed point 16.16 for precision)
    uint32_t scale_x = (DISPLAY_WIDTH << 16) / src_w;
    uint32_t scale_y = (DISPLAY_HEIGHT << 16) / src_h;
    
    // Use smaller scale to fit within display (preserving aspect ratio)
    uint32_t scale = (scale_x < scale_y) ? scale_x : scale_y;
    
    // Calculate output dimensions
    *dst_w = (src_w * scale) >> 16;
    *dst_h = (src_h * scale) >> 16;
    
    // Clamp to display size
    if (*dst_w > DISPLAY_WIDTH) *dst_w = DISPLAY_WIDTH;
    if (*dst_h > DISPLAY_HEIGHT) *dst_h = DISPLAY_HEIGHT;
    
    // Center on display
    *x_off = (DISPLAY_WIDTH - *dst_w) / 2;
    *y_off = (DISPLAY_HEIGHT - *dst_h) / 2;
}

// Scale and draw RGB888 image to display with bilinear-ish interpolation
static void scale_and_draw_rgb888(uint8_t *src, uint16_t src_w, uint16_t src_h)
{
    uint16_t dst_w, dst_h;
    int16_t x_off, y_off;
    
    calc_fit_scale(src_w, src_h, &dst_w, &dst_h, &x_off, &y_off);
    
    ESP_LOGI(TAG, "Scaling %dx%d -> %dx%d, offset (%d,%d)", 
             src_w, src_h, dst_w, dst_h, x_off, y_off);
    
    // Fixed-point scale factors (16.16)
    uint32_t x_ratio = ((src_w - 1) << 16) / dst_w;
    uint32_t y_ratio = ((src_h - 1) << 16) / dst_h;
    
    // Draw line by line
    for (uint16_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (y * y_ratio) >> 16;
        
        for (uint16_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (x * x_ratio) >> 16;
            int idx = (src_y * src_w + src_x) * 3;
            
            uint8_t r = src[idx + 0];
            uint8_t g = src[idx + 1];
            uint8_t b = src[idx + 2];
            draw_buffer[x] = rgb888_to_rgb565(r, g, b);
        }
        
        esp_lcd_panel_draw_bitmap(panel_handle, x_off, y_off + y, 
                                  x_off + dst_w, y_off + y + 1, draw_buffer);
    }
}

// TJPGD decoder context
typedef struct {
    FILE *fp;
    uint8_t *work_buf;
    size_t work_buf_size;
    int16_t x_offset;  // For centering
    int16_t y_offset;  // For centering
    float scale_x;     // For software scaling
    float scale_y;
    uint16_t out_w;    // Output dimensions
    uint16_t out_h;
} tjpgd_ctx_t;

static tjpgd_ctx_t tjpgd_ctx;

// TJPGD input callback - read from file
static unsigned int tjpgd_input_func(JDEC *jd, uint8_t *buff, unsigned int nbyte)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    if (buff) {
        // Read data into buffer
        return fread(buff, 1, nbyte, ctx->fp);
    } else {
        // Skip data
        if (fseek(ctx->fp, nbyte, SEEK_CUR) == 0) {
            return nbyte;
        }
        return 0;
    }
}

// TJPGD output callback - write pixels to display
static UINT tjpgd_output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    
    // Apply centering offset
    int16_t x = rect->left + ctx->x_offset;
    int16_t y = rect->top + ctx->y_offset;
    uint16_t w = rect->right - rect->left + 1;
    uint16_t h = rect->bottom - rect->top + 1;
    
    // Skip if completely outside display
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT || x + w <= 0 || y + h <= 0) {
        return 1;  // Continue decoding
    }
    
    // Calculate source and destination clipping
    int src_x_start = 0, src_y_start = 0;
    int dst_x = x, dst_y = y;
    int draw_w = w, draw_h = h;
    
    // Clip left edge
    if (x < 0) {
        src_x_start = -x;
        draw_w += x;
        dst_x = 0;
    }
    // Clip top edge  
    if (y < 0) {
        src_y_start = -y;
        draw_h += y;
        dst_y = 0;
    }
    // Clip right edge
    if (dst_x + draw_w > DISPLAY_WIDTH) {
        draw_w = DISPLAY_WIDTH - dst_x;
    }
    // Clip bottom edge
    if (dst_y + draw_h > DISPLAY_HEIGHT) {
        draw_h = DISPLAY_HEIGHT - dst_y;
    }
    
    if (draw_w <= 0 || draw_h <= 0) {
        return 1;
    }
    
    // Convert RGB888 to RGB565 and draw
    uint8_t *src = (uint8_t *)bitmap;
    
    for (int row = 0; row < draw_h; row++) {
        for (int col = 0; col < draw_w; col++) {
            int src_idx = ((src_y_start + row) * w + (src_x_start + col)) * 3;
            uint8_t r = src[src_idx + 0];
            uint8_t g = src[src_idx + 1];
            uint8_t b = src[src_idx + 2];
            draw_buffer[col] = rgb888_to_rgb565(r, g, b);
        }
        esp_lcd_panel_draw_bitmap(panel_handle, dst_x, dst_y + row, dst_x + draw_w, dst_y + row + 1, draw_buffer);
    }
    
    return 1;  // Continue decoding
}

static image_type_t get_image_type(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return IMG_TYPE_UNKNOWN;
    
    // Skip macOS resource fork files
    if (filename[0] == '.' && filename[1] == '_') {
        return IMG_TYPE_UNKNOWN;
    }
    
    ext++;  // Skip the dot
    
    if (strcasecmp(ext, "bin") == 0) return IMG_TYPE_BIN;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return IMG_TYPE_JPEG;
    if (strcasecmp(ext, "png") == 0) return IMG_TYPE_PNG;
    if (strcasecmp(ext, "gif") == 0) return IMG_TYPE_GIF;
    
    return IMG_TYPE_UNKNOWN;
}

static esp_err_t display_jpeg(const char *path)
{
    ESP_LOGI(TAG, "Decoding JPEG: %s", path);
    
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open JPEG file");
        return ESP_FAIL;
    }
    
    // Get file size for logging
    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    ESP_LOGI(TAG, "JPEG file size: %d bytes", (int)file_size);
    
    // Allocate work buffer for TJPGD (needs about 3100 bytes minimum)
    #define TJPGD_WORK_BUF_SIZE 4096
    uint8_t *work_buf = heap_caps_malloc(TJPGD_WORK_BUF_SIZE, MALLOC_CAP_DEFAULT);
    if (!work_buf) {
        ESP_LOGE(TAG, "Failed to allocate TJPGD work buffer");
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    
    // Setup context
    tjpgd_ctx.fp = fp;
    tjpgd_ctx.work_buf = work_buf;
    tjpgd_ctx.work_buf_size = TJPGD_WORK_BUF_SIZE;
    
    // Prepare the decoder
    JDEC jdec;
    JRESULT res = jd_prepare(&jdec, tjpgd_input_func, work_buf, TJPGD_WORK_BUF_SIZE, &tjpgd_ctx);
    
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG prepare failed: %d", res);
        free(work_buf);
        fclose(fp);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "JPEG original: %dx%d", jdec.width, jdec.height);
    
    // For large images: use TJPGD scaling (1/2, 1/4, 1/8) to fit in display
    // For small images: decode at full size, then we'll need software upscaling
    uint8_t scale = 0;
    uint16_t scaled_w = jdec.width;
    uint16_t scaled_h = jdec.height;
    
    // Scale down if too large for display
    while ((scaled_w > DISPLAY_WIDTH || scaled_h > DISPLAY_HEIGHT) && scale < 3) {
        scale++;
        scaled_w = jdec.width >> scale;
        scaled_h = jdec.height >> scale;
    }
    
    // Check if this is a small image that would benefit from upscaling
    bool needs_software_scale = (scaled_w < DISPLAY_WIDTH * 0.7 && scaled_h < DISPLAY_HEIGHT * 0.7);
    
    if (needs_software_scale) {
        // Decode to buffer then software scale
        size_t buf_size = scaled_w * scaled_h * 3;
        uint8_t *jpeg_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpeg_buf) {
            jpeg_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
        }
        
        if (jpeg_buf) {
            // We need to modify the decoder to write to buffer instead of display
            // For now, just decode directly and center (simpler approach)
            ESP_LOGI(TAG, "Small JPEG - decoding at native size for software scaling");
            
            // Store buffer info in context for the output callback to use
            tjpgd_ctx.x_offset = 0;  // Will write to top-left of output buffer
            tjpgd_ctx.y_offset = 0;
            
            // Actually for simplicity, let's just decode directly to display with centering
            // Full software scaling would require modifying the output callback significantly
            free(jpeg_buf);
            needs_software_scale = false;
        } else {
            needs_software_scale = false;
        }
    }
    
    // Calculate centering offsets for direct decode to display
    tjpgd_ctx.x_offset = (DISPLAY_WIDTH - scaled_w) / 2;
    tjpgd_ctx.y_offset = (DISPLAY_HEIGHT - scaled_h) / 2;
    
    ESP_LOGI(TAG, "JPEG scaled: %dx%d (scale=1/%d, offset=%d,%d)", 
             scaled_w, scaled_h, 1 << scale, tjpgd_ctx.x_offset, tjpgd_ctx.y_offset);
    
    // Clear screen before drawing (black background)
    fill_screen_color(0x0000);
    
    // Decompress the image with scaling
    res = jd_decomp(&jdec, tjpgd_output_func, scale);
    
    free(work_buf);
    fclose(fp);
    
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG decompress failed: %d", res);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "JPEG decoded successfully");
    return ESP_OK;
}

// PNG decoder context
typedef struct {
    int16_t x_offset;
    int16_t y_offset;
    uint16_t img_width;
    uint16_t img_height;
    uint8_t *frame_buf;    // RGB888 frame buffer for scaling
    bool too_large;        // Flag if image exceeds memory limits
} png_ctx_t;

static png_ctx_t png_ctx;

// Maximum image dimension we can handle (considering 8MB PSRAM minus other uses)
// 2000x2000 * 3 = 12MB - too big. Let's cap at ~1200x1200 for safety
#define PNG_MAX_DIMENSION 1200
#define PNG_MAX_PIXELS (PNG_MAX_DIMENSION * PNG_MAX_DIMENSION)

// pngle draw callback - store pixels to frame buffer for later scaling
static void pngle_draw_callback(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *rgba)
{
    if (!png_ctx.frame_buf || png_ctx.too_large) return;
    
    // Handle transparency - blend with black background
    uint8_t r = rgba[0];
    uint8_t g = rgba[1];
    uint8_t b = rgba[2];
    uint8_t a = rgba[3];
    
    if (a < 255) {
        r = (r * a) / 255;
        g = (g * a) / 255;
        b = (b * a) / 255;
    }
    
    // Store to frame buffer
    int idx = (y * png_ctx.img_width + x) * 3;
    png_ctx.frame_buf[idx + 0] = r;
    png_ctx.frame_buf[idx + 1] = g;
    png_ctx.frame_buf[idx + 2] = b;
}

// pngle init callback - allocate frame buffer when size is known
static void pngle_init_callback(pngle_t *pngle, uint32_t w, uint32_t h)
{
    png_ctx.img_width = w;
    png_ctx.img_height = h;
    png_ctx.too_large = false;
    
    // Check if image is too large
    size_t num_pixels = (size_t)w * h;
    if (num_pixels > PNG_MAX_PIXELS || w > PNG_MAX_DIMENSION || h > PNG_MAX_DIMENSION) {
        ESP_LOGW(TAG, "PNG too large: %lux%lu (max %dx%d)", w, h, PNG_MAX_DIMENSION, PNG_MAX_DIMENSION);
        png_ctx.too_large = true;
        return;
    }
    
    // Calculate buffer size
    size_t buf_size = num_pixels * 3;
    ESP_LOGI(TAG, "PNG size: %lux%lu (%d bytes needed)", w, h, (int)buf_size);
    ESP_LOGI(TAG, "Free PSRAM: %d, Free Internal: %d", 
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    
    // Allocate frame buffer (RGB888)
    png_ctx.frame_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!png_ctx.frame_buf) {
        // Try regular RAM if PSRAM not available
        png_ctx.frame_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    if (png_ctx.frame_buf) {
        memset(png_ctx.frame_buf, 0, buf_size);  // Black background
        ESP_LOGI(TAG, "PNG frame buffer allocated: %d bytes", (int)buf_size);
    } else {
        ESP_LOGE(TAG, "Failed to allocate PNG frame buffer (%d bytes)", (int)buf_size);
        png_ctx.too_large = true;  // Treat allocation failure same as too large
    }
}

static esp_err_t display_png(const char *path)
{
    ESP_LOGI(TAG, "Decoding PNG: %s", path);
    
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open PNG file");
        return ESP_FAIL;
    }
    
    // Get file size
    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    ESP_LOGI(TAG, "PNG file size: %d bytes", (int)file_size);
    
    // Reset context
    png_ctx.frame_buf = NULL;
    png_ctx.img_width = 0;
    png_ctx.img_height = 0;
    png_ctx.too_large = false;
    
    // Create pngle instance
    pngle_t *pngle = pngle_new();
    if (!pngle) {
        ESP_LOGE(TAG, "Failed to create pngle instance");
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    
    // Set callbacks
    pngle_set_init_callback(pngle, pngle_init_callback);
    pngle_set_draw_callback(pngle, pngle_draw_callback);
    
    // Read and decode in chunks
    uint8_t buf[1024];
    size_t remain = file_size;
    esp_err_t result = ESP_OK;
    
    while (remain > 0) {
        size_t to_read = (remain > sizeof(buf)) ? sizeof(buf) : remain;
        size_t read_bytes = fread(buf, 1, to_read, fp);
        if (read_bytes == 0) {
            break;
        }
        
        int fed = pngle_feed(pngle, buf, read_bytes);
        if (fed < 0) {
            ESP_LOGE(TAG, "PNG decode error: %s", pngle_error(pngle));
            result = ESP_FAIL;
            break;
        }
        
        // If image is too large, stop early
        if (png_ctx.too_large) {
            ESP_LOGW(TAG, "Skipping oversized PNG");
            result = ESP_ERR_NO_MEM;
            break;
        }
        
        remain -= read_bytes;
    }
    
    pngle_destroy(pngle);
    fclose(fp);
    
    // Scale and draw if we have a valid frame buffer
    if (result == ESP_OK && png_ctx.frame_buf && png_ctx.img_width > 0 && png_ctx.img_height > 0) {
        scale_and_draw_rgb888(png_ctx.frame_buf, png_ctx.img_width, png_ctx.img_height);
        ESP_LOGI(TAG, "PNG displayed successfully");
    }
    
    // Free frame buffer
    if (png_ctx.frame_buf) {
        free(png_ctx.frame_buf);
        png_ctx.frame_buf = NULL;
    }
    
    return result;
}

static esp_err_t display_gif(const char *path)
{
    ESP_LOGI(TAG, "Decoding GIF: %s", path);
    
    gd_GIF *gif = gd_open_gif(path);
    if (!gif) {
        ESP_LOGE(TAG, "Failed to open GIF file");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "GIF size: %dx%d, colors: %d, frames: %d", gif->width, gif->height, gif->palette->size, gif->nframes);
    
    // Allocate frame buffer (RGB888) - try PSRAM first
    size_t frame_size = gif->width * gif->height * 3;
    uint8_t *frame = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        frame = heap_caps_malloc(frame_size, MALLOC_CAP_DEFAULT);
    }
    if (!frame) {
        ESP_LOGE(TAG, "Failed to allocate GIF frame buffer");
        gd_close_gif(gif);
        return ESP_ERR_NO_MEM;
    }
    
    // Animate GIF
    animation_running = true;
    stop_animation = false;
    
    while (!stop_animation) {
        // Get frame
        int ret = gd_get_frame(gif);
        if (ret <= 0) {
            // Loop back to start
            gd_rewind(gif);
            ret = gd_get_frame(gif);
            if (ret <= 0) break;
        }
        
        // Render frame to buffer
        gd_render_frame(gif, frame);
        
        // Scale and draw to display
        scale_and_draw_rgb888(frame, gif->width, gif->height);
        
        // Wait for frame delay, but check for touch every 20ms
        uint32_t delay_ms = gif->gce.delay ? gif->gce.delay * 10 : 100;  // GIF delay is in centiseconds
        uint32_t elapsed = 0;
        
        while (elapsed < delay_ms && !stop_animation) {
            // Check for touch event during GIF animation
            if (global_touch_handle) {
                cst816t_touch_data_t touch_data;
                esp_err_t touch_ret = cst816t_read_touch(global_touch_handle, &touch_data);
                if (touch_ret == ESP_OK && touch_data.event != 0) {
                    stop_animation = true;
                    break;
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(20));
            elapsed += 20;
        }
    }
    
    animation_running = false;
    free(frame);
    gd_close_gif(gif);
    
    ESP_LOGI(TAG, "GIF animation finished");
    return ESP_OK;
}

static esp_err_t display_bin(const char *path)
{
    ESP_LOGI(TAG, "Loading BIN: %s", path);
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file");
        return ESP_FAIL;
    }
    
    const int strip_height = 10;
    const int strip_size = DISPLAY_WIDTH * strip_height * 2;
    
    for (int y = 0; y < DISPLAY_HEIGHT; y += strip_height) {
        size_t read = fread(draw_buffer, 1, strip_size, f);
        if (read > 0) {
            int rows = read / (DISPLAY_WIDTH * 2);
            if (rows > 0) {
                esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + rows, draw_buffer);
            }
        }
        if (read < strip_size) break;
    }
    
    fclose(f);
    return ESP_OK;
}

static esp_err_t display_image(const char *path)
{
    image_type_t type = get_image_type(path);
    
    // Clear screen first
    fill_screen_color(0x0000);  // Black
    
    switch (type) {
        case IMG_TYPE_JPEG:
            return display_jpeg(path);
        case IMG_TYPE_BIN:
            return display_bin(path);
        case IMG_TYPE_PNG:
            return display_png(path);
        case IMG_TYPE_GIF:
            return display_gif(path);
        default:
            ESP_LOGE(TAG, "Unknown image type");
            return ESP_FAIL;
    }
}

static esp_err_t init_sd_card(void)
{
    ESP_LOGI(TAG, "Initializing SD card in SDMMC 1-bit mode...");
    
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_DATA;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        sd_mounted = false;
        return ret;
    }
    
    sd_mounted = true;
    ESP_LOGI(TAG, "SD card mounted!");
    sdmmc_card_print_info(stdout, sd_card);
    
    return ESP_OK;
}

static void unmount_sd_card(void)
{
    if (sd_mounted) {
        ESP_LOGI(TAG, "Unmounting SD card...");
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, sd_card);
        sd_card = NULL;
        sd_mounted = false;
        use_images = false;
        num_images = 0;
        current_image = 0;
        fill_screen_color(0x07E0);  // Show green immediately to confirm unmount
        ESP_LOGI(TAG, "SD card unmounted - safe to remove");
    }
}

static void remount_sd_card(void)
{
    ESP_LOGI(TAG, "Attempting to remount SD card...");
    
    // Clear screen and show status
    fill_screen_color(0x001F);  // Blue background
    
    if (init_sd_card() == ESP_OK) {
        int found = scan_for_images();
        if (found > 0) {
            use_images = true;
            current_image = 0;
            
            ESP_LOGI(TAG, "=========================================");
            ESP_LOGI(TAG, "  SD card remounted - %d images found!", found);
            ESP_LOGI(TAG, "=========================================");
            
            // Show first image
            display_image(image_paths[current_image]);
        } else {
            ESP_LOGW(TAG, "SD card mounted but no images found");
            fill_screen_color(0xFFE0);  // Yellow - no images
        }
    } else {
        ESP_LOGE(TAG, "Failed to remount SD card");
        fill_screen_color(0xF800);  // Red - error
    }
}

static int scan_for_images(void)
{
    num_images = 0;
    
    DIR *dir = opendir(IMAGES_DIR);
    const char *search_path = IMAGES_DIR;
    
    if (!dir) {
        ESP_LOGW(TAG, "Could not open %s, trying root...", IMAGES_DIR);
        dir = opendir(MOUNT_POINT);
        search_path = MOUNT_POINT;
    }
    
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open SD card directory");
        return 0;
    }
    
    ESP_LOGI(TAG, "Scanning: %s", search_path);
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && num_images < MAX_IMAGES) {
        // Skip hidden files and macOS resource forks
        if (entry->d_name[0] == '.') {
            continue;
        }
        
        image_type_t type = get_image_type(entry->d_name);
        if (type != IMG_TYPE_UNKNOWN) {
            snprintf(image_paths[num_images], sizeof(image_paths[0]), 
                     "%s/%s", search_path, entry->d_name);
            
            ESP_LOGI(TAG, "Found [%d]: %s (type=%d)", num_images, entry->d_name, type);
            num_images++;
        }
    }
    
    closedir(dir);
    
    ESP_LOGI(TAG, "Found %d image(s)", num_images);
    return num_images;
}

static void show_next_content(int *color_idx)
{
    if (use_images && num_images > 0) {
        current_image = (current_image + 1) % num_images;
        display_image(image_paths[current_image]);
        ESP_LOGI(TAG, "Image %d/%d: %s", current_image + 1, num_images, image_paths[current_image]);
    }
    // No color cycling - this is a dedicated image viewer
}

void app_main(void)
{
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  SD Card Image Display Test");
    ESP_LOGI(TAG, "  Supports: JPEG, PNG, GIF, BIN");
    ESP_LOGI(TAG, "  Waveshare ESP32-S3 1.8\" AMOLED");
    ESP_LOGI(TAG, "=========================================");
    
    // Initialize I2C
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
    
    // TCA9554 pin masks
    #define PIN_MASK_0  (1 << 0)
    #define PIN_MASK_1  (1 << 1)
    #define PIN_MASK_2  (1 << 2)
    #define PIN_MASK_7  (1 << 7)
    
    // Configure TCA9554
    ESP_LOGI(TAG, "Configuring TCA9554...");
    uint8_t output_pins = PIN_MASK_0 | PIN_MASK_1 | PIN_MASK_2 | PIN_MASK_7;
    ESP_ERROR_CHECK(tca9554_set_pin_direction(output_pins, true));
    ESP_ERROR_CHECK(tca9554_set_pin_level(output_pins, false));
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(tca9554_set_pin_level(output_pins, true));
    vTaskDelay(pdMS_TO_TICKS(50));
    
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
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .spi_host = SPI2_HOST,
    };
    
    ESP_LOGI(TAG, "Initializing display...");
    ESP_ERROR_CHECK(rm67162_qspi_init(&qspi_config, &qspi_ctx, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // Allocate draw buffer (needs to be big enough for JPEG MCU blocks)
    draw_buffer = heap_caps_malloc(DISPLAY_WIDTH * 16 * 2, MALLOC_CAP_DMA);
    if (!draw_buffer) {
        ESP_LOGE(TAG, "Failed to allocate draw buffer!");
        return;
    }
    
    ESP_LOGI(TAG, "Display initialized!");
    
    // Initialize touch
    ESP_LOGI(TAG, "Initializing touch...");
    cst816t_config_t touch_config = {
        .i2c_port = I2C_MASTER_NUM,
        .i2c_addr = 0x38,
        .int_gpio = PIN_TOUCH_INT,
        .rst_gpio = -1,
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .swap_xy = false,
        .invert_x = false,
        .invert_y = false,
    };
    
    cst816t_handle_t touch_handle = NULL;
    esp_err_t touch_ret = cst816t_init(&touch_config, &touch_handle);
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed, continuing without touch");
    }
    
    // Set global touch handle for use in animations
    global_touch_handle = touch_handle;
    
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
    
    bool was_touching = false;
    uint32_t last_touch_time = 0;
    uint32_t last_touch_end_time = 0;
    uint32_t double_tap_window = 400;  // 400ms for double tap
    uint32_t long_press_threshold = 1500;  // 1.5s for long press
    
    // Main loop
    while (1) {
        bool touched = false;
        uint32_t current_time = esp_timer_get_time() / 1000;  // Convert to ms
        
        if (touch_handle) {
            cst816t_touch_data_t touch_data;
            esp_err_t ret = cst816t_read_touch(touch_handle, &touch_data);
            if (ret == ESP_OK && touch_data.event != 0) {  // FIXED: event != 0 detects touch
                touched = true;
            }
        }
        
        // Detect single tap (short press then release)
        if (touched && !was_touching) {
            last_touch_time = current_time;
        }
        
        // Detect release
        if (!touched && was_touching) {
            last_touch_end_time = current_time;
            uint32_t press_duration = last_touch_end_time - last_touch_time;
            
            // Single tap: short press (< 1.5s) and not double tapping
            if (press_duration < long_press_threshold) {
                show_next_content(NULL);
            }
        }
        
        // Detect long press (1.5+ seconds)
        if (touched && (current_time - last_touch_time) >= long_press_threshold) {
            unmount_sd_card();
        }
        
        // Detect double tap (two taps within 400ms)
        if (touched && !was_touching && (current_time - last_touch_end_time) < double_tap_window) {
            remount_sd_card();
        }
        
        was_touching = touched;
        vTaskDelay(pdMS_TO_TICKS(10));  // 10ms polling for responsive detection
    }
}
