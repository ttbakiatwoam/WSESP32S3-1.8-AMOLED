# Waveshare ESP32-S3 1.8" AMOLED Image Viewer

This project provides a full-featured image viewer for the Waveshare ESP32-S3 1.8" Touch AMOLED display (368x448 pixels).

## Hardware

- **Display**: RM67162 QSPI AMOLED (368x448 pixels)
- **Touch**: CST816T capacitive touch controller (I2C 0x38)
- **SD Card**: SDMMC 1-bit mode with hot-swap support
- **PSRAM**: 8MB Octal SPI @ 80MHz

## Features

### Image Format Support

- **JPEG**: Hardware-accelerated TJPGD decoder from ESP32-S3 ROM
  - Supports hardware scaling (1/2, 1/4, 1/8)
  - Software upscaling for images smaller than screen
- **PNG**: pngle library with 1200x1200 pixel limit
- **GIF**: Animated GIF support with gifdec library
  - Full animation playback with proper frame delays
  - Can be interrupted by touch during playback
- **RAW RGB565**: Binary `.bin` files (368x448x2 bytes)

### Touch Gestures

| Gesture | Action | Details |
|---------|--------|---------|
| **Single Tap** | Next Image | Advance to next image in gallery |
| **Long Press** (1.5s) | Unmount SD Card | Safely unmount SD - shows **green** screen when safe to remove |
| **Double Tap** (400ms) | Remount SD Card | Rescan SD card for new images after reinsertion |

### SD Card Hot-Swap

The SD card can be safely removed and reinserted without rebooting:

1. **Long press** (1.5 seconds) on the screen
2. Wait for **green screen** confirmation
3. Remove SD card safely
4. Insert new SD card (or same card with new images)
5. **Double tap** to remount and rescan

### Status Colors

The display uses color feedback to indicate SD card status:

| Color | Hex Value | Meaning |
|-------|-----------|---------|
| **🟢 Green** | `0x07E0` | SD card unmounted - **safe to remove** |
| **🔵 Blue** | `0x001F` | Attempting to mount SD card |
| **🟡 Yellow** | `0xFFE0` | SD card mounted but no images found |
| **🔴 Red** | `0xF800` | SD card mount failed |

## Image Requirements

### Supported Formats

- `.jpg`, `.jpeg` - JPEG images (any size, will be scaled)
- `.png` - PNG images (max 1200x1200 pixels)
- `.gif` - Animated GIF (will loop continuously)
- `.bin` - Raw RGB565 binary (exactly 368x448 pixels)

### Image Scaling

- **Larger than screen**: Images are scaled down to fit
- **Smaller than screen**: 
  - JPEGs: Software upscaling to fill screen
  - PNGs: Centered on screen (no upscaling)
  - GIFs: Centered on screen (no upscaling)

### Storage Location

Place images in the root directory of your FAT32-formatted SD card (32GB SDHC recommended).

## Building

### Prerequisites

- ESP-IDF v5.5.2 or later
- Configured for ESP32-S3

### Build Commands

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Technical Details

### Color Format

- Display uses RGB565 format (16-bit color)
- Standard RGB byte order with big-endian SPI transfer
- MADCTL = 0x00 (no hardware BGR conversion)
- Automatic byte-swapping for correct color rendering

### Memory Management

- PSRAM enabled for large image buffers
- JPEG decode buffer: Up to screen size in RAM
- PNG/GIF decode: Uses PSRAM for frame buffers
- Dynamic allocation prevents fragmentation

### Performance

- JPEG decode: Hardware-accelerated via TJPGD
- QSPI display: 80MHz transfer rate
- Touch polling: Event-driven with 50ms debounce
- GIF animation: Respects frame delays, can be interrupted by touch

## Pin Configuration

### Display (RM67162)

- **QSPI Mode**: 4-wire QSPI
- **Clock**: 80MHz
- **Interface**: Custom QSPI driver

### Touch (CST816T)

- **Interface**: I2C
- **Address**: 0x38
- **SCL**: GPIO 14
- **SDA**: GPIO 15
- **INT**: GPIO 21 (interrupt)

### SD Card (SDMMC)

- **Mode**: 1-bit SDMMC
- **CLK**: GPIO 2
- **CMD**: GPIO 1
- **DATA0**: GPIO 3
- **Speed**: Up to 40MHz

## Troubleshooting

### Colors appear wrong
- Ensure PSRAM is enabled in `sdkconfig`
- Verify MADCTL is set to 0x00 in driver
- Check for correct RGB565 byte-swap

### Touch not responding
- Check I2C address (should be 0x38)
- Verify touch interrupt GPIO (21)
- Ensure CST816T driver is initialized

### SD card won't mount
- Format as FAT32
- Check wiring: CLK=2, CMD=1, DATA=3
- Try a different SD card (32GB SDHC recommended)

### GIFs won't play
- Verify GIF is valid format
- Check file size (large GIFs may exceed memory)
- Ensure PSRAM is enabled

### Can't stop GIF animation
- This has been **fixed** - touch now works during GIF playback
- Touch detection happens during frame delays
- Any tap will stop animation and advance to next image

## License

This project is part of the Ghost ESP ecosystem. See main repository for license details.

## Credits

- Display driver based on Waveshare examples
- JPEG decoder: ESP32-S3 ROM TJPGD
- PNG decoder: pngle library
- GIF decoder: gifdec library
- Touch driver: CST816T reference implementation
