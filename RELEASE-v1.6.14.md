# FlightWall v1.6.14

Manual firmware-upload browser reconnect improvement.

- Adds a JavaScript reconnect watcher after a manual UF2 upload.
- Waits for the FlightWall web server to disappear during reboot before reloading.
- Polls with cache-busting and no-cache headers until the device comes back online.
- Handles dropped HTTP connections during reboot by entering the same reconnect flow.
- Reloads the main page automatically once FlightWall is reachable again.
- Leaves the signed automatic updater unchanged for a separate follow-up.
