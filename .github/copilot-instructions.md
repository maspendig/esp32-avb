# GitHub Copilot Instructions for ESP32-AVB Project

## Project Overview

This is an **ESP-IDF 5.5** based implementation of an **AVB (Audio Video Bridging)** client for the **ESP32-P4**
platform. The project implements IEEE 1722 (AVTP), IEEE 802.1AS (gPTP), and related AVB protocols to enable professional
audio/video streaming over Ethernet.

### Key Information

- **Framework**: ESP-IDF 5.5
- **Target Platform**: ESP32-P4 (specifically the ESP32-P4-Function-EV-Board nano dev kit)
- **Hardware Features**:
    - IEEE 1588 compatible NIC with hardware timestamping support
    - ES8311 audio codec (connected via I2S)
    - Ethernet MAC (EMAC) with PTP timestamp capabilities
- **Base Project**: Built upon the ESP-IDF PTP example
- **Primary Use Case**: AVB Listener (consuming audio streams and outputting to speakers)

## Architecture & Components

### Main Components

1. **AVB Component** (`components/avb/`):
    - `avtp.c/h`: AVTP (Audio Video Transport Protocol) main implementation
    - `adp.c/h`: AVTP Discovery Protocol (ADP) for entity discovery
    - `aecp.c/h`: AVTP Enumeration and Control Protocol (AECP)
    - `acmp.c/h`: AVTP Connection Management Protocol (ACMP) for stream connections
    - `msrp.c/h`: Multiple Stream Registration Protocol (MSRP) for stream reservation
    - `mvrp.c/h`: Multiple VLAN Registration Protocol (MVRP) for VLAN management
    - `types.h`: Common type definitions
    - `config.h`: Configuration constants

2. **PTP Component** (`components/ptpd/`):
    - PTPd implementation for IEEE 1588 / IEEE 802.1AS (gPTP) time synchronization

3. **Ethernet Time Component** (`components/esp_eth_time/`):
    - Hardware timestamp integration with ESP32-P4 Ethernet MAC

4. **Main Application** (`main/`):
    - `ptp_main.c`: Main application entry point

## Coding Guidelines

### Protocol Implementation Standards

1. **AVTP Protocol Handling**:
    - Use the L2 TAP interface (`/dev/net/tap`) for raw Ethernet access
    - Support both untagged (Ethertype 0x22F0) and VLAN-tagged (802.1Q) AVTP frames
    - Handle AVTP subtypes: ADP (0xFA), AECP (0xFB), ACMP (0xFC), MAAP (0xFE)
    - Stream data subtypes: AAF (0x02), 61883-IIDC (0x01)

2. **VLAN and QoS**:
    - Use 802.1Q VLAN tagging for AVB streams (Ethertype 0x8100)
    - Implement proper PCP (Priority Code Point) handling for QoS
    - Default VLAN ID: 2 (configurable)

3. **Multicast Addressing**:
    - MAAP range: `91:E0:F0:00:00:00` to `91:E0:F0:00:FD:FF`
    - AVDECC control: `91:E0:F0:01:00:00`
    - Always configure MAC filters for multicast addresses

### ESP-IDF Specific Patterns

1. **Error Handling**:
   ```c
   esp_err_t err = function_call();
   if (err != ESP_OK) {
       ESP_LOGE(TAG, "Error description: %s", esp_err_to_name(err));
       return err;
   }
   ```

2. **Logging**:
    - Use ESP_LOG macros: `ESP_LOGE`, `ESP_LOGW`, `ESP_LOGI`, `ESP_LOGD`
    - Define TAG at file scope: `static const char* TAG = "component_name";`

3. **Task Creation**:
    - Use FreeRTOS: `xTaskCreate(task_func, "TaskName", stack_size, param, priority, &handle);`
    - Use higher priorities (10+) for real-time AVB packet processing
    - Typical stack size: 8192 bytes for AVB tasks

4. **Socket Operations**:
    - Use `open("/dev/net/tap", 0)` for L2 TAP sockets
    - Configure with `ioctl()`: `L2TAP_S_INTF_DEVICE`, `L2TAP_S_RCV_FILTER`, `L2TAP_G_DEVICE_DRV_HNDL`
    - Use `select()` or `poll()` for multiplexing multiple sockets
    - Set non-blocking mode with `fcntl()` for stream data sockets

5. **Ethernet Driver Integration**:
   ```c
   esp_eth_handle_t eth_handle;
   esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, &mac_addr);
   esp_eth_ioctl(eth_handle, ETH_CMD_S_ALL_MULTICAST, &enable);
   esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, mac_addr);
   ```

### AVB-Specific Patterns

1. **Entity ID Generation**:
    - Derive from MAC address: `MAC[0:23] + 0xFFFE + MAC[24:47]`
    - Store as 64-bit unsigned integer

2. **Timestamp Handling**:
    - Use `struct timespec` for time values
    - Use `clock_gettime(CLOCK_MONOTONIC, &ts)` for monotonic time
    - Hardware PTP timestamps are provided via Ethernet MAC

3. **Stream Data Processing**:
    - Drain socket queues in non-blocking loops to prevent overflow
    - Track sequence numbers to detect packet loss
    - Parse VLAN headers: TCI at offset 14-15, inner Ethertype at 16-17

4. **State Machine Pattern**:
    - Maintain protocol state in dedicated state structures
    - Use periodic tasks for timer-driven operations (MSRP, MVRP, ADP)
    - Implement event-driven processing for received messages

## Audio Processing Guidelines

### I2S and ES8311 Codec

1. **Audio Format**:
    - Expected format: AAF (AVTP Audio Format) or 61883-IIDC
    - Configure I2S for the ES8311 codec on the dev kit
    - Typical sample rates: 48kHz, 96kHz
    - Typical bit depths: 16-bit, 24-bit, 32-bit

2. **Audio Pipeline**:
    - AVTP Stream → Packet Parser → Format Converter → I2S DMA Buffer → ES8311 DAC → Speaker Output
    - Implement jitter buffer for clock domain crossing
    - Use presentation time from AVTP headers for synchronization

3. **I2S Driver Usage**:
   ```c
   i2s_config_t i2s_config = {
       .mode = I2S_MODE_MASTER | I2S_MODE_TX,
       .sample_rate = 48000,
       .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
       .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
       // ... additional config
   };
   ```

## Memory and Performance Considerations

1. **Buffer Sizes**:
    - Ethernet frame buffer: 1518 bytes (max standard Ethernet frame)
    - AVTP stream packets: typically 1024-1500 bytes
    - Use unions for protocol message overlays to save stack space

2. **Real-Time Performance**:
    - Minimize processing in packet receive paths
    - Use DMA for I2S audio transfer
    - Consider using separate tasks for control (ADP/AECP/ACMP) and data (stream) paths

3. **FreeRTOS Integration**:
    - Use queue for inter-task communication
    - Use event groups for synchronization
    - Be mindful of ISR context vs task context

## Common Patterns and Idioms

### Packet Reception Pattern

```c
fd_set readfds;
FD_ZERO(&readfds);
FD_SET(socket_fd, &readfds);
int max_fd = socket_fd;

struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms
int ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

if (ret > 0 && FD_ISSET(socket_fd, &readfds)) {
    ssize_t len = read(socket_fd, buffer, sizeof(buffer));
    if (len > 0) {
        // Process packet
    }
}
```

### Periodic Task Pattern

```c
struct timespec now, delta;
clock_gettime(CLOCK_MONOTONIC, &now);
timespecsub(&now, &last_time, &delta);
int64_t ms_elapsed = delta.tv_sec * 1000 + (delta.tv_nsec / 1000000);
if (ms_elapsed > INTERVAL_MS) {
    // Perform periodic task
    last_time = now;
}
```

## Testing and Debugging

1. **Wireshark Integration**:
    - Use Wireshark with AVTP dissectors to analyze protocol traffic
    - Filter by ethertype 0x22F0 for AVTP or 0x8100 for VLAN frames

2. **Debug Logging**:
    - Use menuconfig to set log levels per component
    - Log critical protocol events (entity discovery, stream connections)
    - Log timing information for synchronization debugging

3. **Hardware Tools**:
    - Oscilloscope for verifying GPIO sync pulse alignment
    - Network tap or mirror port for traffic capture
    - Audio analyzer for measuring output quality

## Build System

- Use `idf.py build` to compile
- Use `idf.py -p PORT flash monitor` to flash and monitor
- Configure via `idf.py menuconfig`
- Dependencies managed via `idf_component.yml`

## Important Notes

- This is a **Listener-focused implementation** - prioritize receive path optimization
- Hardware timestamping is essential for AVB - always use MAC-level PTP timestamps
- The ES8311 codec must be initialized and configured before audio playback
- VLAN configuration is critical for AVB - ensure switches are properly configured
- gPTP synchronization must be established before stream reception begins

## References

- IEEE 1722-2016/2022: AVTP Protocol
- IEEE 802.1AS: Timing and Synchronization (gPTP)
- IEEE 802.1Q: VLAN Tagging
- IEEE 802.1BA: Audio Video Bridging (AVB) Systems
- ESP-IDF Programming Guide: https://docs.espressif.com/projects/esp-idf/
- ES8311 Datasheet: For I2S codec configuration

