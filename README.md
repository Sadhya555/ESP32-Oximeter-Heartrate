# ESP32 Pulse Oximeter & Heart Rate Monitor

A professional-grade, non-blocking Pulse Oximeter built with an ESP32, MAX30102, and a 0.96" OLED display. This project uses digital signal processing to separate AC/DC optical signals for accurate SpO2 and Heart Rate tracking.

## Features
* **State Machine Architecture:** Clean transitions between waiting, measuring, and grace-period states.
* **Digital Signal Processing:** Implements an Exponential Moving Average (EMA) filter to isolate the AC pulse wave from the DC tissue baseline.
* **Non-Blocking UI:** 30 FPS OLED graphing runs independently of the 100Hz I2C sensor polling rate, preventing communication bottlenecks.
* **Grace Period Memory:** A 5-second visual countdown freezes the last known data and graph position if the finger is briefly removed.
* **Auto-Scaling Graph:** Dynamically adjusts the Y-axis bounds to fit the user's specific pulse amplitude.

## Hardware Required
* ESP32 Development Board (e.g., DOIT DevKit V1)
* MAX30102 Pulse Oximeter Sensor (Note: Ensure your breakout board has proper 3.3V pull-up resistors for the I2C lines).
* 0.96" SSD1306 OLED Display (I2C)

## Wiring Diagram
![Pulse Oximeter Wiring Diagram](docs/wiring_diagram.svg)
| ESP32 Pin | MAX30102 | OLED (SSD1306) | Description |
| :--- | :--- | :--- | :--- |
| **3.3V** | VIN | VCC | Power |
| **GND** | GND | GND | Ground |
| **GPIO 21** | SDA | SDA | I2C Data |
| **GPIO 22** | SCL | SCL | I2C Clock |

## Installation & Flashing
This project is built using PlatformIO. 
1. Clone this repository.
2. Open the project folder in VSCode with the PlatformIO extension installed.
3. The `platformio.ini` file will automatically download the required Adafruit and SparkFun libraries.
4. Click **Build** and **Upload**.
