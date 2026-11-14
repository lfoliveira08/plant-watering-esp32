# Contributing

Issues and pull requests are welcome.

- Firmware lives in `firmware/`. Each sketch must compile for **ESP32 Dev Module** (the CI workflow checks this).
- Never commit Wi-Fi credentials: they belong in `firmware/RealHardware_Dashboard/secrets.h`, which is git-ignored.
- Test the relay polarity with `firmware/RelayTest` before connecting a pump.
- Keep pull requests focused and describe how you tested them on hardware.
