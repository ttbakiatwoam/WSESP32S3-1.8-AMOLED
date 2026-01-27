/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RM67162 QSPI Bus Implementation
 * Ported from Lilygo MicroPython AMOLED driver
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "rm67162_qspi.h"

static const char *TAG = "RM67162_QSPI";

#define SEND_BUF_SIZE (32 * 1024)  // 32KB max transfer

typedef struct {
    spi_device_handle_t spi_dev;
    int cs_gpio;
    int reset_gpio;
    uint16_t width;
    uint16_t height;
} rm67162_qspi_ctx_t;

/**
 * @brief Send command with parameters to RM67162
 */
static esp_err_t rm67162_qspi_tx_param(rm67162_qspi_ctx_t *ctx, uint8_t cmd, const void *param, size_t param_size)
{
    ESP_LOGD(TAG, "TX param - cmd: 0x%02x, param_size: %u", cmd, param_size);
    
    spi_transaction_t t = {
        .flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR,
        .cmd = 0x02,
        .addr = cmd << 8,
        .length = param_size * 8,
        .tx_buffer = param,
    };
    
    gpio_set_level(ctx->cs_gpio, 0);
    esp_err_t ret = spi_device_polling_transmit(ctx->spi_dev, &t);
    gpio_set_level(ctx->cs_gpio, 1);
    
    return ret;
}

/**
 * @brief Send color data to RM67162 display memory
 */
static esp_err_t rm67162_qspi_tx_color(rm67162_qspi_ctx_t *ctx, const void *color, size_t color_size)
{
    ESP_LOGD(TAG, "TX color - size: %u", color_size);
    
    esp_err_t ret = ESP_OK;
    spi_transaction_ext_t t = {0};
    
    // Activate CS
    gpio_set_level(ctx->cs_gpio, 0);
    
    // Prepare write transaction - send RAMWR command
    t.base.flags = SPI_TRANS_MODE_QIO;
    t.base.cmd = 0x32;
    t.base.addr = 0x002C00;  // RAMWR command
    ret = spi_device_polling_transmit(ctx->spi_dev, (spi_transaction_t *)&t);
    if (ret != ESP_OK) {
        gpio_set_level(ctx->cs_gpio, 1);
        return ret;
    }
    
    // Send color data in chunks
    const uint8_t *p_color = (const uint8_t *)color;
    size_t remaining = color_size;
    
    memset(&t, 0, sizeof(t));
    t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | 
                   SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    t.command_bits = 0;
    t.address_bits = 0;
    t.dummy_bits = 0;
    
    while (remaining > 0) {
        size_t chunk_size = (remaining > SEND_BUF_SIZE) ? SEND_BUF_SIZE : remaining;
        
        t.base.tx_buffer = p_color;
        t.base.length = chunk_size * 8;
        
        ret = spi_device_polling_transmit(ctx->spi_dev, (spi_transaction_t *)&t);
        if (ret != ESP_OK) {
            break;
        }
        
        remaining -= chunk_size;
        p_color += chunk_size;
    }
    
    // Deactivate CS
    gpio_set_level(ctx->cs_gpio, 1);
    
    return ret;
}

/**
 * @brief Panel IO interface for RM67162 QSPI
 * This structure provides the interface that esp_lcd expects
 */
typedef struct {
    esp_err_t (*del)(esp_lcd_panel_io_handle_t io);
    esp_err_t (*tx_param)(esp_lcd_panel_io_handle_t io, int lcd_cmd, int lcd_cmd_bits, const void *param, size_t param_size);
    esp_err_t (*tx_color)(esp_lcd_panel_io_handle_t io, int lcd_cmd, int lcd_cmd_bits, const void *color, size_t color_size);
    esp_err_t (*register_event_callbacks)(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cbs, void *user_ctx);
} panel_io_vtable_t;

typedef struct {
    const panel_io_vtable_t *vtable;
    rm67162_qspi_ctx_t *ctx;
} rm67162_panel_io_t;

static esp_err_t rm67162_io_del(esp_lcd_panel_io_handle_t io)
{
    if (io) {
        free(io);
    }
    return ESP_OK;
}

static esp_err_t rm67162_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, 
                                     int lcd_cmd_bits, const void *param, size_t param_size)
{
    rm67162_panel_io_t *panel_io = (rm67162_panel_io_t *)io;
    return rm67162_qspi_tx_param(panel_io->ctx, (uint8_t)lcd_cmd, param, param_size);
}

static esp_err_t rm67162_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, 
                                     int lcd_cmd_bits, const void *color, size_t color_size)
{
    rm67162_panel_io_t *panel_io = (rm67162_panel_io_t *)io;
    return rm67162_qspi_tx_color(panel_io->ctx, color, color_size);
}

static esp_err_t rm67162_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, 
                                                     const esp_lcd_panel_io_callbacks_t *cbs, 
                                                     void *user_ctx)
{
    // Not implemented for now
    return ESP_OK;
}

static const panel_io_vtable_t rm67162_io_vtable = {
    .del = rm67162_io_del,
    .tx_param = rm67162_io_tx_param,
    .tx_color = rm67162_io_tx_color,
    .register_event_callbacks = rm67162_io_register_event_callbacks,
};

esp_err_t rm67162_qspi_init(const rm67162_qspi_config_t *config,
                            esp_lcd_panel_io_handle_t *io_handle,
                            esp_lcd_panel_handle_t *panel_handle)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "invalid config");
    ESP_RETURN_ON_FALSE(io_handle, ESP_ERR_INVALID_ARG, TAG, "invalid io_handle");
    ESP_RETURN_ON_FALSE(panel_handle, ESP_ERR_INVALID_ARG, TAG, "invalid panel_handle");
    
    esp_err_t ret = ESP_OK;
    
    // Allocate context
    rm67162_qspi_ctx_t *ctx = heap_caps_calloc(1, sizeof(rm67162_qspi_ctx_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no memory for context");
    
    ctx->cs_gpio = config->cs_gpio;
    ctx->reset_gpio = config->reset_gpio;
    ctx->width = config->width;
    ctx->height = config->height;
    
    // Configure CS GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&io_conf), cleanup, TAG, "CS GPIO config failed");
    gpio_set_level(config->cs_gpio, 1);
    
    // Configure reset GPIO if provided
    if (config->reset_gpio >= 0) {
        io_conf.pin_bit_mask = (1ULL << config->reset_gpio);
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), cleanup, TAG, "Reset GPIO config failed");
        gpio_set_level(config->reset_gpio, 1);
    }
    
    // Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .data0_io_num = config->d0_gpio,
        .data1_io_num = config->d1_gpio,
        .sclk_io_num = config->sck_gpio,
        .data2_io_num = config->d2_gpio,
        .data3_io_num = config->d3_gpio,
        .max_transfer_sz = (SEND_BUF_SIZE * 16) + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS | SPICOMMON_BUSFLAG_QUAD,
    };
    
    ESP_GOTO_ON_ERROR(spi_bus_initialize(config->spi_host, &bus_cfg, SPI_DMA_CH_AUTO),
                      cleanup, TAG, "SPI bus init failed");
    
    // Add SPI device
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = 0,  // SPI mode 0
        .clock_speed_hz = config->pclk_hz,
        .spics_io_num = -1,  // We manage CS manually
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 10,
    };
    
    ESP_GOTO_ON_ERROR(spi_bus_add_device(config->spi_host, &dev_cfg, &ctx->spi_dev),
                      cleanup_bus, TAG, "SPI device add failed");
    
    // Hardware reset
    if (config->reset_gpio >= 0) {
        gpio_set_level(config->reset_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(config->reset_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    
    // Create panel IO structure
    rm67162_panel_io_t *panel_io = heap_caps_calloc(1, sizeof(rm67162_panel_io_t), MALLOC_CAP_DEFAULT);
    ESP_GOTO_ON_FALSE(panel_io, ESP_ERR_NO_MEM, cleanup_dev, TAG, "no memory for panel IO");
    
    panel_io->vtable = &rm67162_io_vtable;
    panel_io->ctx = ctx;
    
    *io_handle = (esp_lcd_panel_io_handle_t)panel_io;
    
    ESP_LOGI(TAG, "RM67162 QSPI initialized - %dx%d @ %lu Hz", 
             config->width, config->height, config->pclk_hz);
    
    return ESP_OK;

cleanup_dev:
    spi_bus_remove_device(ctx->spi_dev);
cleanup_bus:
    spi_bus_free(config->spi_host);
cleanup:
    heap_caps_free(ctx);
    return ret;
}

esp_err_t rm67162_qspi_deinit(esp_lcd_panel_io_handle_t io_handle,
                              esp_lcd_panel_handle_t panel_handle)
{
    if (!io_handle) {
        return ESP_OK;
    }
    
    rm67162_panel_io_t *panel_io = (rm67162_panel_io_t *)io_handle;
    rm67162_qspi_ctx_t *ctx = panel_io->ctx;
    
    if (ctx) {
        if (ctx->spi_dev) {
            spi_bus_remove_device(ctx->spi_dev);
            // Note: We don't free the bus as other devices might be using it
        }
        heap_caps_free(ctx);
    }
    
    free(panel_io);
    
    return ESP_OK;
}
