# FlightWall v1.4.0

This is the first four-panel hardware-validation release.

- Configures four 64x32 HUB75 panels as a 128x64, 2x2 serpentine display.
- Matches the physical chain: controller -> bottom-right -> bottom-left -> top-left -> top-right.
- Corrects the 180-degree physical rotation of the upper panel row.
- Swaps the two logical panel rows because the data chain begins on the physical bottom row.
- Displays a labeled quadrant test for five seconds during startup.
- Preserves the existing 128x32 FlightWall information layouts for initial hardware verification.
- Renames the former white power test to Full-Panel Load Test throughout the UI and source.

After installing, verify that the startup screen shows TOP LEFT, TOP RIGHT,
BOTTOM L, and BOTTOM R in their matching physical quadrants and that all text
is upright. The normal information screens intentionally use the upper half of
the 128x64 display in this validation release.
