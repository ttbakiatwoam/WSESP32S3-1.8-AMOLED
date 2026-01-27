/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RM67162 QSPI Display Driver for Waveshare ESP32-S3 1.8" AMOLED
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RM67162 QSPI panel configuration
 */
typedef struct {
    int cs_gpio;                    /*!< Chip select GPIO */
    int sck_gpio;                   /*!< QSPI clock GPIO */
    int d0_gpio;                    /*!< QSPI data 0 GPIO */
    int d1_gpio;                    /*!< QSPI data 1 GPIO */
    int d2_gpio;                    /*!< QSPI data 2 GPIO */
    int d3_gpio;                    /*!< QSPI data 3 GPIO */
    int reset_gpio;                 /*!< Reset GPIO, -1 if not used */
    uint32_t pclk_hz;               /*!< Pixel clock frequency in Hz */
    uint16_t width;                 /*!< Display width in pixels */
    uint16_t height;                /*!< Display height in pixels */
    spi_host_device_t spi_host;     /*!< SPI host device */
} rm67162_qspi_config_t;

/**
 * @brief Initialize RM67162 QSPI display
 *
 * @param config Pointer to configuration structure
 * @param io_handle Pointer to store the panel IO handle
 * @param panel_handle Pointer to store the panel handle
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL
 *      - ESP_ERR_NO_MEM if out of memory
 *      - ESP_FAIL if initialization failed
 */
esp_err_t rm67162_qspi_init(const rm67162_qspi_config_t *config,
                            esp_lcd_panel_io_handle_t *io_handle,
                            esp_lcd_panel_handle_t *panel_handle);

/**
 * @brief Deinitialize RM67162 QSPI display
 *
 * @param io_handle Panel IO handle
 * @param panel_handle Panel handle
 * @return
 *      - ESP_OK on success
 */
esp_err_t rm67162_qspi_deinit(esp_lcd_panel_io_handle_t io_handle,
                              esp_lcd_panel_handle_t panel_handle);

#ifdef __cplusplus
}
#endif
