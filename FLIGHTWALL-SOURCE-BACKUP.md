# FlightWall Embedded v1.5.2 source notes

This archive contains the complete source used to build the v1.5.2 Stage 3
firmware for:

- Raspberry Pi Pico 2 W (RP2350 ARM Secure build)
- SEENGREAT RGB Matrix Adapter Board V3.9
- Four chained 64x32, 1/16-scan HUB75 P5 panels (128x64 total)
- 2x2 serpentine panel mapping with bottom-right chain entry

The firmware connects directly to the adsb.fi Open Data API and does not
require Home Assistant. It includes Wi-Fi provisioning and saved-network
management, web settings, browser-based A/B OTA updates, emergency detection
and testing, watched aircraft, paginated traffic screens, nearest-aircraft
details with embedded airline marks, traffic history, and an animated radar.
The 128x64 interface displays seven aircraft per page and includes a
web-configurable normal-screen duration.

## Firmware signing

Private signing keys are deliberately excluded from the repository. Set
`FLIGHTWALL_SIGNING_KEY` to the path of a project-specific private key when
configuring a signed release build. An ordinary development build is hashed
but remains unsigned.

The v1.5.2 test image was signed with the public Raspberry Pi Pico SDK example
key. That signature is useful for exercising the signing machinery, but it
must not be treated as proof that an image is an authentic FlightWall release.

## Build prerequisites

- Raspberry Pi Pico SDK 2.3.0
- ARM GNU embedded toolchain with RP2350/Cortex-M33 support
- CMake
- Python 3
- Git submodules/dependencies required by the upstream JuPFu HUB75 project

The original project is based on https://github.com/JuPfu/hub75.

## Typical build

From the extracted source directory, adjust the paths for the local SDK and
toolchain, then run:

```sh
PICO_SDK_PATH=/path/to/pico-sdk \
PICO_TOOLCHAIN_PATH=/path/to/arm-toolchain \
cmake -S . -B build \
  -DFLIGHTWALL_SIGNING_KEY=/secure/path/to/flightwall-private.pem

PICO_SDK_PATH=/path/to/pico-sdk \
PICO_TOOLCHAIN_PATH=/path/to/arm-toolchain \
cmake --build build --target hub75_demo -j2
```

The resulting OTA image is `build/hub75_demo.uf2`. The FlightWall web updater
expects a Stage 3 application UF2 and removes its first partition-table block
before sending the application to the inactive A/B slot.

## Main FlightWall files

- `hub75_demo.cpp` — display layouts and main rotation
- `aircraft_data.c/.h` — adsb.fi HTTPS retrieval, parsing, sorting, and history
- `flightwall_settings.c/.h` — persistent display and watch-list settings
- `wifi_provisioning.c/.h` — Wi-Fi setup and embedded web interface handlers
- `ota_server.c/.h` — A/B firmware upload and reboot handling
- `content/` — embedded configuration webpage and response pages
- `partitions.json` — flash partition layout
- `mbedtls_config.h` and `lwipopts.h` — embedded TLS/network configuration
- `CMakeLists.txt` — Pico 2 W build definition and version

Version: 1.5.2
