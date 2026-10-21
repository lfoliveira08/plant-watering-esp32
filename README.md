# Automatic Plant Watering System (ESP32) - Smart Greenhouse, Stage 1

ESP32-based automatic plant waterer with a weather-aware web dashboard.
Built by **Luis de Oliveira** and **Uriel Garcia** for ENGR 4399 ST: Cyber-Physical and IoT Systems (Fall 2026, University of the Incarnate Word).

The basic idea (soil sensor + relay + pump) is inspired by the Instagram video
"Automatic Plant Watering System" by **eazytronic3795**.

## Features
- Soil moisture and tank-level sensing (ADC1, averaged, calibrated)
- State machine: IDLE / WATERING / SOAKING / FAULT with hysteresis, 8 s run limit, soak delay, response check, latching fault
- Tank-low interlock and rain block from a live forecast (Open-Meteo, no API key)
- Temperature judgement vs. 3 plant profiles (Basil, Aloe, Fern) with buzzer alert
- OLED status screen and a web dashboard served by the ESP32 (`http://plantwater.local`) with simulation buttons
- Fallback Wi-Fi access point `PlantWater` if the router is unreachable

## Hardware
ESP32 dev board (30-pin), Keyestudio soil moisture + water level sensors, 1-ch 5 V relay (active-HIGH), submersible DC pump, SSD1306 OLED (I2C 0x3C), active buzzer.

| ESP32 pin | Part |
|---|---|
| D34 | Soil sensor S |
| D35 | Water level sensor S |
| D26 | Relay signal |
| D27 | Buzzer |
| D21 / D22 | OLED SDA / SCL |
| 3V3 / VIN / GND | sensors+OLED / relay / common ground |

Pump supply: + to relay COM, NO to pump red, pump black to supply -. Full wiring and report in `docs/`.

## Build and run
1. Arduino IDE, board **ESP32 Dev Module**; install **Adafruit SSD1306** and **Adafruit GFX**.
2. Open `firmware/RealHardware_Dashboard/RealHardware_Dashboard.ino` and set `WIFI_SSID` and `WIFI_PASS` (2.4 GHz). Never commit your real credentials.
3. Calibrate `SOIL_RAW_DRY`, `SOIL_RAW_WET`, `LEVEL_LOW_RAW` from Serial Monitor readings (115200 baud).
4. Test the relay polarity with `firmware/RelayTest` before connecting the pump.
5. Open `http://plantwater.local` or the IP shown on the OLED.

## Docs
- `docs/SAR_Project1_deOliveira_Garcia.docx` - project report
- `docs/Project1_Report.docx`, `docs/Project1_Presentation.pptx`, `docs/Project1_Code_and_Sensor_Guide.docx`

## Limitations
Unauthenticated HTTP dashboard and plain-HTTP weather request (classroom demo only); resistive soil probe corrodes; forecast temperature, not measured at the plant.
