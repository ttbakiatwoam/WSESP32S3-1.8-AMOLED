/*
 * SD Card Image Display Test for RM67162 QSPI display
 * Waveshare ESP32-S3 1.8" AMOLED
 * Touch the screen to cycle through images on SD card
 * 
 * Supports: JPEG, PNG, GIF, and raw RGB565 .bin files
 */

#include <stdio.h>
// Shared types, enums, and statics
#include "display_test_shared.h"

// Provide storage for shared statics (only one definition, rest are extern in header)
char image_paths[MAX_IMAGES][MAX_PATH_LEN];
int num_images = 0;
int current_image = 0;
esp_lcd_panel_handle_t panel_handle = NULL;
uint16_t *draw_buffer = NULL;
sdmmc_card_t *sd_card = NULL;
bool use_images = false;
bool sd_mounted = false;
cst816t_handle_t global_touch_handle = NULL;
bool stop_animation = false;
bool animation_running = false;
uint8_t *image_buffers[IMAGE_BUFFER_COUNT] = {NULL, NULL};
int active_image_buffer = 0;
int preload_image_index = -1;
bool preload_ready = false;
SemaphoreHandle_t preload_mutex = NULL;
volatile touch_event_t pending_touch_event = TOUCH_EVENT_NONE;

// Forward declarations
static int scan_for_images(void);
static void touch_task(void *pvParameters);

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
    // Always fill the full display area (portrait)
    int fill_w = PORTRAIT_WIDTH;
    int fill_h = PORTRAIT_HEIGHT;
    for (int i = 0; i < fill_w * 10; i++) {
        draw_buffer[i] = color;
    }
    for (int y = 0; y < fill_h; y += 10) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, fill_w, y + 10, draw_buffer);
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
    // For portrait, use original display width/height
    uint16_t disp_w = PORTRAIT_WIDTH;
    uint16_t disp_h = PORTRAIT_HEIGHT;
    // Calculate scale factors (using fixed point 16.16 for precision)
    uint32_t scale_x = (disp_w << 16) / src_w;
    uint32_t scale_y = (disp_h << 16) / src_h;
    uint32_t scale = (scale_x < scale_y) ? scale_x : scale_y;
    *dst_w = (src_w * scale) >> 16;
    *dst_h = (src_h * scale) >> 16;
    if (*dst_w > disp_w) *dst_w = disp_w;
    if (*dst_h > disp_h) *dst_h = disp_h;
    *x_off = (disp_w - *dst_w) / 2;
    *y_off = (disp_h - *dst_h) / 2;
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
    // Draw scaled image directly (portrait)
    for (uint16_t y = 0; y < dst_h; y++) {
        for (uint16_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (x * x_ratio) >> 16;
            uint32_t src_y = (y * y_ratio) >> 16;
            if (src_x >= src_w) src_x = src_w - 1;
            if (src_y >= src_h) src_y = src_h - 1;
            int idx = (src_y * src_w + src_x) * 3;
            uint8_t r = src[idx + 0];
            uint8_t g = src[idx + 1];
            uint8_t b = src[idx + 2];
            draw_buffer[x] = rgb888_to_rgb565(r, g, b);
        }
        esp_lcd_panel_draw_bitmap(panel_handle, x_off, y_off + y, x_off + dst_w, y_off + y + 1, draw_buffer);
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

// TJPGD output callback - write pixels to display or buffer
static UINT tjpgd_output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    
    // Check if we're capturing to buffer (x_offset < 0 is the signal)
    if (ctx->x_offset < 0) {
        // Write to buffer for later scaling
        uint8_t *src = (uint8_t *)bitmap;
        uint8_t *dst = ctx->work_buf;  // Reusing work_buf pointer to hold output buffer
        
        uint16_t w = rect->right - rect->left + 1;
        uint16_t h = rect->bottom - rect->top + 1;
        uint16_t output_width = ctx->out_w;
        
        for (int row = 0; row < h; row++) {
            int dst_y = rect->top + row;
            for (int col = 0; col < w; col++) {
                int dst_x = rect->left + col;
                int src_idx = (row * w + col) * 3;
                int dst_idx = (dst_y * output_width + dst_x) * 3;
                
                dst[dst_idx + 0] = src[src_idx + 0];
                dst[dst_idx + 1] = src[src_idx + 1];
                dst[dst_idx + 2] = src[src_idx + 2];
            }
        }
        return 1;
    }
    
    // Normal display path - apply centering offset
    int16_t x = rect->left + ctx->x_offset;
    int16_t y = rect->top + ctx->y_offset;
    uint16_t w = rect->right - rect->left + 1;
    uint16_t h = rect->bottom - rect->top + 1;
    
    // For portrait, use original display width/height
    uint16_t disp_w = PORTRAIT_WIDTH;
    uint16_t disp_h = PORTRAIT_HEIGHT;
    // Skip if completely outside display
    if (x >= disp_w || y >= disp_h || x + w <= 0 || y + h <= 0) {
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
    if (dst_x + draw_w > disp_w) {
        draw_w = disp_w - dst_x;
    }
    // Clip bottom edge
    if (dst_y + draw_h > disp_h) {
        draw_h = disp_h - dst_y;
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
    uint8_t *work_buf = (uint8_t*)heap_caps_malloc(TJPGD_WORK_BUF_SIZE, MALLOC_CAP_DEFAULT);
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
    while ((scaled_w > PORTRAIT_WIDTH || scaled_h > PORTRAIT_HEIGHT) && scale < 3) {
        scale++;
        scaled_w = jdec.width >> scale;
        scaled_h = jdec.height >> scale;
    }
    
    // Check if this is a small image that would benefit from upscaling
    bool needs_software_scale = (scaled_w < PORTRAIT_WIDTH * 0.7 && scaled_h < PORTRAIT_HEIGHT * 0.7);
    
    if (needs_software_scale) {
        // Decode to buffer then software scale
        size_t buf_size = scaled_w * scaled_h * 3;
        uint8_t *jpeg_buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpeg_buf) {
            jpeg_buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
        }
        
        if (jpeg_buf) {
            ESP_LOGI(TAG, "Small JPEG - decoding to buffer for upscaling");
            
            // Setup context for buffer capture
            tjpgd_ctx.x_offset = -1;  // Signal to capture to buffer
            tjpgd_ctx.y_offset = 0;
            tjpgd_ctx.work_buf = jpeg_buf;  // Reuse pointer to store output buffer
            tjpgd_ctx.out_w = scaled_w;
            tjpgd_ctx.out_h = scaled_h;
            
            res = jd_decomp(&jdec, tjpgd_output_func, scale);
            
            if (res == JDR_OK) {
                // Clear screen
                fill_screen_color(0x0000);
                // Rotate buffer 90 CCW
                uint8_t *rot_buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!rot_buf) rot_buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
                if (rot_buf) {
                    rotate_rgb888_90ccw(jpeg_buf, rot_buf, scaled_w, scaled_h);
                    scale_and_draw_rgb888(rot_buf, scaled_h, scaled_w); // Note: w/h swapped after rotation
                    free(rot_buf);
                } else {
                    scale_and_draw_rgb888(jpeg_buf, scaled_w, scaled_h);
                }
            }
            
            free(jpeg_buf);
            free(work_buf);
            fclose(fp);
            
            if (res != JDR_OK) {
                ESP_LOGE(TAG, "JPEG decompress failed: %d", res);
                return ESP_FAIL;
            }
            
            ESP_LOGI(TAG, "JPEG decoded and upscaled successfully");
            return ESP_OK;
        } else {
            needs_software_scale = false;
        }
    }
    
    // Calculate centering offsets for direct decode to display
    tjpgd_ctx.x_offset = (PORTRAIT_WIDTH - scaled_w) / 2;
    tjpgd_ctx.y_offset = (PORTRAIT_HEIGHT - scaled_h) / 2;
    
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
    png_ctx.frame_buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!png_ctx.frame_buf) {
        // Try regular RAM if PSRAM not available
        png_ctx.frame_buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
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
    
    ESP_LOGI(TAG, "GIF size: %dx%d, colors: %d", gif->width, gif->height, gif->palette->size);
    
    // Allocate frame buffer (RGB888) - try PSRAM first
    size_t frame_size = gif->width * gif->height * 3;
    uint8_t *frame = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        frame = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DEFAULT);
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
            // Check for touch event from the touch task
            if (pending_touch_event == TOUCH_EVENT_TAP || pending_touch_event == TOUCH_EVENT_DOUBLE_TAP || pending_touch_event == TOUCH_EVENT_LONG_PRESS) {
                stop_animation = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            elapsed += 20;
        }
    }
    
    animation_running = false;
    // Clear any pending touch event so next image doesn't require extra tap
    pending_touch_event = TOUCH_EVENT_NONE;
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
    const int strip_size = PORTRAIT_WIDTH * strip_height * 2;
    
    for (int y = 0; y < PORTRAIT_HEIGHT; y += strip_height) {
        size_t read = fread(draw_buffer, 1, strip_size, f);
        if (read > 0) {
            int rows = read / (PORTRAIT_WIDTH * 2);
            if (rows > 0) {
                esp_lcd_panel_draw_bitmap(panel_handle, 0, y, PORTRAIT_WIDTH, y + rows, draw_buffer);
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
    printf("[MONITOR] SD card successfully mounted.\n");
    sdmmc_card_print_info(stdout, sd_card);
    
    return ESP_OK;
}

static void unmount_sd_card(void)
{
    if (sd_mounted) {
        ESP_LOGI(TAG, "Unmounting SD card...");
        
        // Stop any running animations
        stop_animation = true;
        vTaskDelay(pdMS_TO_TICKS(100));  // Wait for animation to stop
        
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, sd_card);
        sd_card = NULL;
        sd_mounted = false;
        use_images = false;
        num_images = 0;
        current_image = 0;
        fill_screen_color(0x07E0);  // Show green immediately to confirm unmount
        ESP_LOGI(TAG, "SD card unmounted - safe to remove");
        printf("[MONITOR] SD card unmounted - safe to remove.\n");
    }
}

static void remount_sd_card(void)
{
    ESP_LOGI(TAG, "Attempting to remount SD card...");
    
    // If already mounted, just rescan
    if (sd_mounted) {
        ESP_LOGI(TAG, "SD card already mounted, rescanning...");
        int found = scan_for_images();
        if (found > 0) {
            use_images = true;
            current_image = 0;
            ESP_LOGI(TAG, "Found %d images", found);
            display_image(image_paths[current_image]);
        } else {
            ESP_LOGW(TAG, "No images found");
            fill_screen_color(0xFFE0);  // Yellow
        }
        return;
    }
    
    // Clear screen and show status
    fill_screen_color(0x001F);  // Blue background
    vTaskDelay(pdMS_TO_TICKS(200));
    
    if (init_sd_card() == ESP_OK) {
        int found = scan_for_images();
        if (found > 0) {
            use_images = true;
            current_image = 0;
            ESP_LOGI(TAG, "=========================================");
            ESP_LOGI(TAG, "  SD card remounted - %d images found!", found);
            ESP_LOGI(TAG, "=========================================");
            // Show green for success
            fill_screen_color(0x07E0);
            vTaskDelay(pdMS_TO_TICKS(200));
            // Show first image
            display_image(image_paths[current_image]);
        } else {
            ESP_LOGW(TAG, "SD card mounted but no images found");
            fill_screen_color(0xFFE0);  // Yellow - no images
        }
    } else {
        ESP_LOGE(TAG, "Failed to remount SD card");
        fill_screen_color(0xF800);  // Red - error
        vTaskDelay(pdMS_TO_TICKS(500));
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

// Helper to decode and scale image into a buffer (returns buffer pointer, sets w/h)
static uint8_t* decode_image_to_buffer(const char *path, uint16_t *out_w, uint16_t *out_h) {
    image_type_t type = get_image_type(path);
    // For simplicity, only handle JPEG for now; extend for PNG/GIF as needed
    if (type == IMG_TYPE_JPEG) {
        // Open file and decode JPEG to RGB888 buffer (reuse JPEG logic, but decode to buffer)
        // ...existing JPEG decode logic, but output to malloc'd buffer...
        // For now, just simulate
        *out_w = PORTRAIT_WIDTH;
        *out_h = PORTRAIT_HEIGHT;
        uint8_t *buf = (uint8_t*)heap_caps_malloc(PORTRAIT_WIDTH * PORTRAIT_HEIGHT * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf) memset(buf, 0xFF, PORTRAIT_WIDTH * PORTRAIT_HEIGHT * 3); // White image
        return buf;
    }
    // TODO: Add PNG/GIF support
    return NULL;
}

// Update preload_task to actually decode next image
static void preload_task(void *pvParameters) {
    while (1) {
        if (!preload_mutex) {
            ESP_LOGE(TAG, "preload_task: preload_mutex is NULL! Skipping preload.");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (preload_image_index >= 0 && !preload_ready) {
            int buf_idx = (active_image_buffer + 1) % IMAGE_BUFFER_COUNT;
            if (image_buffers[buf_idx]) {
                free(image_buffers[buf_idx]);
                image_buffers[buf_idx] = NULL;
            }
            uint16_t w, h;
            image_buffers[buf_idx] = decode_image_to_buffer(image_paths[preload_image_index], &w, &h);
            // Optionally store w/h for later use
            xSemaphoreTake(preload_mutex, portMAX_DELAY);
            preload_ready = true;
            xSemaphoreGive(preload_mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// Update show_next_content to use preloaded buffer
static void show_next_content(int *color_idx) {
    if (use_images && num_images > 0) {
        // Advance to next image
        current_image = (current_image + 1) % num_images;
        ESP_LOGI(TAG, "show_next_content: advancing to image %d: %s", current_image, image_paths[current_image]);
        if (!preload_mutex) {
            ESP_LOGE(TAG, "show_next_content: preload_mutex is NULL! Skipping image swap.");
            return;
        }
        // Wait for preload to finish
        xSemaphoreTake(preload_mutex, portMAX_DELAY);
        if (preload_ready && image_buffers[(active_image_buffer + 1) % IMAGE_BUFFER_COUNT]) {
            active_image_buffer = (active_image_buffer + 1) % IMAGE_BUFFER_COUNT;
            // TODO: Actually draw image_buffers[active_image_buffer] to screen
            fill_screen_color(0x07E0); // Green for instant swap (simulate)
            preload_ready = false;
        } else {
            // If no preloaded buffer, fallback to direct display
            display_image(image_paths[current_image]);
        }
        xSemaphoreGive(preload_mutex);
        // Start preloading the next image
        preload_image_index = (current_image + 1) % num_images;
    }
}

// Dedicated touch handling task - runs in parallel for responsive input
static void touch_task(void *pvParameters)
{
    cst816t_handle_t touch_handle = (cst816t_handle_t)pvParameters;
    if (!touch_handle) {
        ESP_LOGE(TAG, "touch_task: NULL touch_handle, exiting task");
        vTaskDelete(NULL);
        return;
    }

    // --- Clean state machine for tap/double-tap/long-press ---
    bool was_touching = false;
    uint32_t touch_down_time = 0;
    uint32_t last_tap_up_time = 0;
    uint8_t tap_count = 0;
    bool long_press_fired = false;
    uint32_t action_cooldown_until = 0;

    const uint32_t DOUBLE_TAP_WINDOW = 200;   // ms
    const uint32_t LONG_PRESS_TIME = 1000;     // ms
    const uint32_t ACTION_COOLDOWN = 250;      // ms

    ESP_LOGI(TAG, "Touch task started (clean state machine)");

    while (1) {
        bool touched = false;
        uint32_t now = esp_timer_get_time() / 1000;  // ms

        if (touch_handle) {
            cst816t_touch_data_t touch_data;
            esp_err_t ret = cst816t_read_touch(touch_handle, &touch_data);
            if (ret == ESP_OK && touch_data.event != 0) {
                touched = true;
            }
        }

        // Skip processing during cooldown
        if (now < action_cooldown_until) {
            was_touching = touched;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Touch down
        if (touched && !was_touching) {
            touch_down_time = now;
            long_press_fired = false;
        }

        // Long press detection
        if (touched && !long_press_fired && (now - touch_down_time) >= LONG_PRESS_TIME) {
            ESP_LOGI(TAG, "Long press detected (posting event)");
            pending_touch_event = TOUCH_EVENT_LONG_PRESS;
            long_press_fired = true;
            tap_count = 0;
            action_cooldown_until = now + ACTION_COOLDOWN;
            fill_screen_color(0x07E0); // Feedback
        }

        // Touch up
        if (!touched && was_touching) {
            uint32_t press_duration = now - touch_down_time;
            if (!long_press_fired && press_duration < LONG_PRESS_TIME) {
                // Register a tap
                if (tap_count == 0) {
                    tap_count = 1;
                    last_tap_up_time = now;
                } else if (tap_count == 1 && (now - last_tap_up_time) <= DOUBLE_TAP_WINDOW) {
                    // Double tap detected
                    ESP_LOGI(TAG, "Double tap detected (posting event)");
                    pending_touch_event = TOUCH_EVENT_DOUBLE_TAP;
                    tap_count = 0;
                    action_cooldown_until = now + ACTION_COOLDOWN;
                }
            }
        }

        // If a single tap is pending and double-tap window has expired, fire single tap
        if (tap_count == 1 && (now - last_tap_up_time) > DOUBLE_TAP_WINDOW) {
            ESP_LOGI(TAG, "Double-tap window expired, posting single tap event");
            pending_touch_event = TOUCH_EVENT_TAP;
            tap_count = 0;
            action_cooldown_until = now + ACTION_COOLDOWN;
        }

        was_touching = touched;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}



void rotate_rgb888_90ccw(uint8_t *src, uint8_t *dst, uint16_t src_w, uint16_t src_h) {
    for (uint16_t y = 0; y < src_h; y++) {
        for (uint16_t x = 0; x < src_w; x++) {
            int src_idx = (y * src_w + x) * 3;
            int dst_idx = ((src_w - 1 - x) * src_h + y) * 3;
            dst[dst_idx + 0] = src[src_idx + 0];
            dst[dst_idx + 1] = src[src_idx + 1];
            dst[dst_idx + 2] = src[src_idx + 2];
        }
    }
}
