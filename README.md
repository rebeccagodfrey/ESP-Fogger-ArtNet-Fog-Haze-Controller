# ESP-Fogger: ArtNet Fog/Haze Controller

This project converts a standard **ESP32 Dev Module** into an ArtNet-controlled node for operating a Fog machine and a Fan via relays. It attempts to mimic a "Hazer" by pulsing the fog machine based on DMX values.

## Warning: Start Here
> [!WARNING]
> **MAINS VOLTAGE HAZARD**
> This project involves switching mains voltage (110V/240V) to control the Fogger and Fan. 
> - **DO NOT ATTEMPT** this if you are inexperienced with mains wiring.
> - Ensure all AC wiring is properly insulated, enclosed, and fused.
> - Improper wiring can result in fire, electric shock, or death.

## Features
- **ArtNet Control**: Receives DMX data via WiFi (ArtNet protocol).
- **Fog Control (Ch 1)**: Manual Fog ON/OFF (Relay 1).
- **Fan Control (Ch 2)**: Fan ON/OFF (Relay 2).
- **Hazer Mode (Ch 3)**: Automated pulsing of the Fog relay to create a haze effect.
    - Low DMX value: Short pulses, long wait (Low density).
    - High DMX value: Long pulses, short wait (High density).
- **OLED Display**: Shows WiFi status, IP, Universe, and Relay states.
- **Web UI**: Configure Node Name and ArtNet Universe settings.
- **WiFi Manager**: Creates a captive portal for easy WiFi configuration.

## Hardware Required
- **ESP32 Development Board** (e.g., ESP32 DevKit V1)
- **SSD1306 OLED Display** (128x64, I2C)
- **2-Channel Relay Module** (HL-52S Twin Relay Module or similar)
- **Fog Machine** (with wired remote functionality tailored for relay control)
- **Fan**

## Wiring Diagram

### Relay Connections
| ESP32 Pin | Component | Description |
|-----------|-----------|-------------|
| **GPIO 26** | Relay 1 (IN) | Fogger Control |
| **GPIO 27** | Relay 2 (IN) | Fan Control |
| **GND** | Relay GND | Common Ground |
| **VIN/5V** | Relay VCC | Power for Relays (if 5V module) |

### SSD1306 Display (I2C)
| ESP32 Pin | Component |
|-----------|-----------|
| **GPIO 21** | SDA (Standard ESP32 I2C SDA) *Check your board pinout* |
| **GPIO 22** | SCL (Standard ESP32 I2C SCL) *Check your board pinout* |
| **3.3V** | VCC |
| **GND** | GND |

*Note: The code uses `Wire.begin()` default pins. For many ESP32 boards, this is SDA=21, SCL=22. If your SSD1306 relies on specific pins defined in code, verify `Wire.begin(SDA, SCL)` calls or standard pinouts.*

## Software Installation (Arduino IDE)

### 1. Install ESP32 Board Support
- Open Arduino IDE -> File -> Preferences.
- Add to "Additional Board Manager URLs": `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- Go to Tools -> Board -> Boards Manager -> Search "esp32" -> Install "esp32 by Espressif Systems".

### 2. Install Required Libraries
Go to **Tools -> Manage Libraries** and install the following (specific versions used in development):

| Library Name | Version | Author |
|--------------|---------|--------|
| **WiFiManager** | 2.0.16-rc.2 (or latest) | tzapu |
| **ArduinoJson** | 6.21.0 (or latest) | Benoit Blanchon |
| **AsyncUDP** | 1.2.4 | Hristo Gochkov |
| **Adafruit GFX Library** | 1.11.0 | Adafruit |
| **Adafruit SSD1306** | 2.5.0 | Adafruit |

### 3. Configure & Upload
1. Open `DMX-Fogger.ino`.
2. Select your Board: **Tools -> Board -> DOIT ESP32 DEVKIT V1** (or matching your board).
3. Connect ESP32 via USB.
4. Click **Upload**.

## Usage Guide

### First Time Setup
1. Power on the device.
2. It will create a WiFi Hotspot named **SGP-Fogger-Setup**.
3. Connect to it with your phone/laptop.
4. A captive portal (login page) should open. If not, go to `192.168.4.1`.
5. Select your WiFi network and enter the password.
6. The device will reboot and connect to your WiFi.

### Configuration (Web UI)
1. Read the IP address from the OLED display.
2. Enter the IP in a web browser.
3. **Settings**:
    - **Node Name**: Name of the device.
    - **Art-Net Settings**: Configure Net, Subnet, and Universe to match your DMX console.
4. Click **Save Configuration**.

### DMX Control
Configure your DMX Console/Software (e.g., QLC+, Onyx, Resolume) to broadcast ArtNet to the device's Universe.

| DMX Channel (Relative to Base Address) | Function | Values |
|----------------------------------------|----------|--------|
| **1** | Fog (Manual) | 0-127: OFF<br>128-255: ON (Relay 1) |
| **2** | Fan | 0-127: OFF<br>128-255: ON (Relay 2) |
| **3** | Hazer Mode | 0-10: OFF<br>11-255: Variable Duty Cycle (Relay 1)<br>Low (11): ~0.5s ON, ~60s OFF<br>High (255): ~10s ON, ~2s OFF |

**Note**: Channel 1 (Manual Fog) overrides standard behavior. It operates in parallel with Channel 3 logic (OR logic). If either calls for Fog, Fog turns ON.
