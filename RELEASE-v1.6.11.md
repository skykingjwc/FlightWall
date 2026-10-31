# FlightWall v1.6.11

Final nearest-aircraft visual polish before updater work.

- Moves supported airline marks upward in the nearest-aircraft identity area.
- Adds the full airline name beneath each supported logo.
- Simplifies embedded logo abbreviations where the separate airline name makes them redundant.
- Keeps the full NEAREST AIRCRAFT title and lower flight-data grid.
- Keeps the FPM climb/descent indicator using the same draw_trend_arrow routine and +/-250 FPM threshold as the aircraft-list ALT column.
- Leaves the automatic-update implementation unchanged for a separate follow-up commit.
