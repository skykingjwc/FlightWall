# FlightWall airline icons

Put editable airline artwork in `airlines/source/` as PNG files named with the
three-letter callsign prefix, for example `DAL.png`, `UAL.png`, `SWA.png`,
`UPS.png`, or `FDX.png`.

Each PNG must be exactly **50 x 22 pixels**. Transparency is supported.

When source PNGs are pushed to `main`, GitHub Actions converts them to a
compact RGB332 pack and publishes `airlines/generated/pack.bin` plus its
SHA-256 manifest. FlightWall checks the manifest after startup and then once
per day.

Downloaded logos override the compiled firmware logo for the same callsign.
If the pack is unavailable, invalid, or does not contain a callsign, FlightWall
continues using its existing built-in logo.

The generated files should not be edited by hand.
