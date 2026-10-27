# ChatGPT FlightWall Embedded Edition 0.4.0

This build adds Wi-Fi provisioning and rollback-protected over-the-air (OTA)
firmware updates to the 128x32 dual-panel Pico 2 W preview.

## One-time wired installation

1. Turn **off the HUB75 power supply** and disconnect the Pico from the adapter
   if necessary. Connect the Pico 2 W to the PC by USB while holding BOOTSEL.
2. Confirm that the `RP2350` drive appears.
3. Copy `flightwall-embedded-v0.4.0-installer.uf2` to that drive.
4. The drive will disappear and FlightWall will restart. This single installer
   writes the A/B application layout, Wi-Fi firmware, and FlightWall program.
5. Disconnect USB, reinstall the Pico in the adapter, and power the display.

The installer intentionally erases the Pico's previous program and flash
partition table. It does not change the RP2350 boot ROM; BOOTSEL recovery always
remains available.

## Connect FlightWall to Wi-Fi

On its first boot the panels show the setup instructions:

1. Join the Wi-Fi network **FlightWall-Setup** from a phone or computer.
2. Use password **flightwall**.
3. A setup page should open automatically. If it does not, browse to
   `http://192.168.4.1/`.
4. Enter the destination network name and password, then choose **Save and
   connect**.
5. On success the panel displays its assigned IP address and `OTA PORT 4242`.

Pico 2 W uses 2.4 GHz Wi-Fi. A dual-band network is fine when 2.4 GHz is enabled.
The firmware saves credentials only after it successfully joins the network.

### Reopen setup mode

Connect USB serial at 115200 baud, restart FlightWall, and press `s` during the
two-second startup window. Press `w` instead to erase all saved networks. These
are recovery conveniences; normal operation does not require USB.

## Future updates over Wi-Fi

Keep `upload_firmware.py` beside the new application UF2 and run:

```powershell
py upload_firmware.py 192.168.50.43 flightwall-embedded-v0.4.1.uf2
```

Replace the IP address and filename as needed. The uploader verifies every
2 KiB chunk with SHA-256. FlightWall installs the image into the inactive
application partition, restarts, and retains the previous image for rollback.

Only use the normal application UF2 for OTA—not the one-time `installer.uf2`.

## Current scope

The aircraft screens still use simulated data. Version 0.4.0 establishes the
networking and update foundation; live aircraft retrieval comes next.
