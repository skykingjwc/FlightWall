# FlightWall v1.6.8

Radar presentation update built on the working ADS-B TLS diagnostic configuration.

- Replaces the discrete radar sweep rays with a continuous filled trailing wedge.
- Shortens the visible sweep tail to about 14 degrees.
- Draws the blue radar grid after the green sweep so grid lines stay solid blue.
- Removes the distracting green gaps where the sweep crossed grid pixels.
- Uses a time-based sweep angle and a faster 75 ms frame cadence for smoother motion.
- Keeps the v1.6.5 30-second minimum spacing between ADS-B API attempts.
- Retains the pre-v1.6.4 TLS feature set that restored ADS-B operation.
- Startup GitHub update checking remains disabled while the TLS/update transport design is being revised.
