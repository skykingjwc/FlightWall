# FlightWall v1.6.9

Radar tuning and nearest-aircraft display cleanup.

- Adds persistent settings for radar sweep leading-edge brightness (10-100%).
- Adds persistent settings for radar sweep tail length (4-30 degrees).
- Migrates existing settings automatically; defaults preserve the v1.6.8 look at 50% brightness and 14 degrees.
- Removes the redundant bearing arrow beside the nearest-aircraft distance.
- Removes the forced plus sign from the FPM value.
- Reuses the aircraft-list climb/descent arrow style on the nearest-aircraft FPM line when vertical rate exceeds +/-250 FPM.
- Keeps the smooth time-based radar animation and blue grid-over-sweep rendering.
- Keeps the 30-second minimum spacing between ADS-B API attempts.
- Retains the pre-v1.6.4 TLS feature set that restored ADS-B operation.
