# JPEG Stream Receiver for ESP32-S3 AMOLED Display

This ESP32-S3 application receives JPEG stream data from an Android phone and displays it on a 466x466 AMOLED display.

## Features

- WiFi connectivity for receiving UDP packets
- Real-time JPEG frame reconstruction from fragmented UDP packets
- LVGL-based display interface
- Touch screen support
- Frame rate monitoring and display

## How It Works

The application implements the receiver side of the JPEG streaming protocol:

1. **UDP Reception**: Listens on port 5000 for incoming UDP packets containing JPEG fragments
2. **Frame Reconstruction**: Assembles fragmented JPEG data by looking for:
   - Start of Image (SOI) marker: `0xFF 0xD8` 
   - End of Image (EOI) marker: `0xFF 0xD9`
3. **Display**: Shows received frames on the AMOLED display using LVGL

## Android Phone Setup

The Android app should stream screen content as follows:
- **Resolution**: 466x466 pixels (matching ESP32 display)
- **Format**: JPEG compressed frames
- **Transport**: UDP packets (max 1024 bytes per packet)
- **Target Port**: 5000
- **Frame Rate**: ~20 FPS (50ms intervals)

## Configuration

### WiFi Setup

1. Open `main/stream_config.h`
2. Update your WiFi credentials:
```c
#define WIFI_SSID       "Your_WiFi_Network_Name"
#define WIFI_PASS       "Your_WiFi_Password"
```

### Network Configuration

The ESP32 will:
- Connect to your WiFi network
- Listen on UDP port 5000 for incoming JPEG streams
- Display its IP address in the serial console

## Build and Flash

1. Set up ESP-IDF environment
2. Configure WiFi credentials in `stream_config.h`
3. Build and flash:
```bash
idf.py build
idf.py flash monitor
```

## Usage

1. Flash the ESP32 with this application
2. Power on and wait for WiFi connection
3. Note the ESP32's IP address from serial console
4. Configure your Android streaming app to send JPEG data to the ESP32's IP on port 5000
5. Start streaming from your phone
6. The ESP32 display will show:
   - Connection status
   - Frame count and size information
   - Stream data (basic implementation)

## Protocol Details

### UDP Packet Format
- Standard UDP packets containing raw JPEG data fragments
- Maximum packet size: 1024 bytes
- No special headers - pure JPEG data

### Frame Detection
- Searches for JPEG Start of Image marker (0xFF 0xD8)
- Buffers data until End of Image marker (0xFF 0xD9)
- Handles out-of-order packets by continuously scanning for markers

### Memory Management
- 100KB circular buffer for incoming JPEG data
- Queue-based frame processing to prevent blocking
- Automatic cleanup of processed frames

## Performance Considerations

- **Frame Rate**: Supports up to 20 FPS (limited by processing time)
- **Latency**: ~100-200ms from phone to display
- **Memory**: Uses ~100KB for JPEG buffering plus LVGL display buffers
- **Network**: Requires stable WiFi connection for smooth streaming

## Known Limitations

1. **JPEG Decoding**: Current implementation shows frame metadata only
   - Full JPEG decoding integration needed for actual image display
   - Consider using ESP32 JPEG decoder component
2. **Error Recovery**: Basic packet loss handling
3. **Color Space**: May need RGB conversion depending on source format

## Next Steps for Full Implementation

1. **Integrate JPEG Decoder**: 
   - Add ESP32 JPEG hardware decoder support
   - Convert decoded images to LVGL-compatible format
2. **Optimize Performance**:
   - Implement double-buffering for smooth display
   - Add frame rate control
3. **Add Error Handling**:
   - Implement packet timeout detection
   - Add corruption detection and recovery

## Hardware Requirements

- ESP32-S3 with AMOLED display (466x466 resolution)
- WiFi connectivity
- Touch screen (optional)
- Sufficient RAM for buffering (PSRAM recommended)

## Components Used

- ESP-IDF networking stack (WiFi, UDP)
- LVGL graphics library
- SH8601/CO5300 LCD drivers
- Touch input handling
- FreeRTOS tasks for concurrent processing