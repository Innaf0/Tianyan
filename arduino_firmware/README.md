# Arduino firmware

Arduino port of the firmware for the Raspberry Pi Pico 2 W. It reads
`CONFIG.TXT` from the SD card, connects to Wi-Fi, fetches the latest Valorant
match from HenrikDev, and displays the result on the ILI9341. XPT2046 touches
are reported over Serial.

## Requirements

- Raspberry Pi Pico/RP2040 Arduino core with Pico 2 W support
- Adafruit GFX Library
- Adafruit ILI9341
- ArduinoJson 7
- PNGdec

The core supplies `WiFi`, `WiFiClientSecure`, `HTTPClient`, `SPI`, and `SD`.

## Configuration

Format the SD card as FAT and place a file named `CONFIG.TXT` in its root.
Use `CONFIG.TXT.example` as the template. The API key is sent in the
`Authorization` header, matching the original Rust firmware.

Open `arduino_firmware.ino` in the Arduino IDE, select the Raspberry Pi Pico 2
W board, install the libraries above, and upload it. Serial logging runs at
115200 baud.
