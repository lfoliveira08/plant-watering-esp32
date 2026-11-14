# Calibration

Open the Serial Monitor at 115200 baud. The firmware prints a `[SOIL]` line every 2 s with the raw ADC values.

1. **SOIL_RAW_DRY**: probe in air or dry soil, note `raw` (the Keyestudio probe reads low when dry).
2. **SOIL_RAW_WET**: probe in water or saturated soil, note `raw`.
3. **LEVEL_LOW_RAW**: set the water level sensor to the "tank low" depth and note `raw`. `LEVEL_HYST_RAW` adds hysteresis so the alarm does not flutter.
4. Edit the constants at the top of `RealHardware_Dashboard.ino` and re-upload.

Use the dashboard simulation buttons to test rain, low-tank and temperature behaviour without touching the sensors.
