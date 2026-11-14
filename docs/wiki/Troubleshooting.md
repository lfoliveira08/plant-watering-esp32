# Troubleshooting

| Symptom | Fix |
|---|---|
| Pump runs when it should be off | Relay polarity is reversed: set `RELAY_ACTIVE_LOW` (check with `RelayTest`) |
| Dashboard unreachable | Use the IP shown on the OLED; `plantwater.local` needs mDNS support. If Wi-Fi failed, join the `PlantWater` network |
| No weather data | Needs 2.4 GHz Wi-Fi with internet; not available in AP mode |
| State shows FAULT | Soil did not respond 3 times: check pump, tube and probe, then press EN |
| "OLED not found" | Check SDA D21 / SCL D22 and I2C address 0x3C |
| Garbled characters (Â°) in dashboard | Update to the latest firmware (UTF-8 fix) |
