# FlightWall v1.6.6

Diagnostic maintenance build for the ADS-B HTTPS regression introduced after v1.6.4.

- Skips the automatic GitHub update check during startup.
- Leaves manual/web-triggered update checks available.
- Keeps the 30-second minimum spacing between ADS-B API attempts from v1.6.5.
- Purpose: isolate the first ADS-B TLS connection from any preceding GitHub TLS activity.

If aircraft data works on v1.6.6, the regression is in TLS/resource lifecycle between the updater and ADS-B client rather than the adsb.fi request itself.
