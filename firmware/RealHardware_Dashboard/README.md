# RealHardware_Dashboard

Main firmware. Setup:

1. Copy `secrets.example.h` to `secrets.h` and put in your 2.4 GHz Wi-Fi name and password (`secrets.h` is git-ignored).
2. Calibrate `SOIL_RAW_DRY`, `SOIL_RAW_WET`, `LEVEL_LOW_RAW` (see `docs/wiki/Calibration.md`).
3. Board **ESP32 Dev Module**, libraries **Adafruit SSD1306** and **Adafruit GFX Library**.
