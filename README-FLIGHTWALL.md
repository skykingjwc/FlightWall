# ChatGPT FlightWall Pico panel test

This firmware runs ChatGPT FlightWall Embedded on four chained
64x32, 1/16-scan HUB75 panels arranged as a 2x2 grid and connected through a
SEENGREAT RGB Matrix Adapter Board V3.9 to a Raspberry Pi Pico 2 W.

GPIO mapping: RGB data GP2-GP7; row address A-D GP8-GP11; E/GP12 unused;
clock GP13; latch GP14; output-enable GP15.

The 128x64 display uses a serpentine data chain beginning at the bottom-right
panel. A tile-aware software mapper preserves the correct quadrants while
applying the required 180-degree orientation correction inside each 64x32
panel. FlightWall retrieves live aircraft data from adsb.fi over Wi-Fi.

At startup, a labeled quadrant screen verifies panel placement and orientation.
The normal rotation uses the entire 128x64 display: seven aircraft rows per
page, a detailed nearest-aircraft screen with airline marks, and an animated
radar sweep. Screen duration is configurable from the embedded web page.

The project uses JuPfu's RP2040/RP2350 HUB75 DMA/PIO driver and the native
Raspberry Pi Pico SDK. It is configured for the `pico2_w` board.
