# FlightWall v1.6.16

Pre-updater stabilization release.

- Redraws the Delta widget so the lower red wedge points upward and the upper navy mark uses a sharper Delta-style chevron silhouette.
- Keeps the larger Delta footprint introduced in recent releases.
- Adds an explicit blank-frame handoff after the animated radar screen before the next aircraft-list cycle.
- Requires the manual firmware browser reconnect flow to observe FlightWall go offline and then serve two valid FlightWall pages in a row before reloading.
- Extends the reconnect wait window and keeps cache-busting/no-cache requests.
- Corrects the firmware page text to state that automatic GitHub update checking is temporarily disabled.
- Leaves the signed automatic updater implementation unchanged for the next dedicated release.
