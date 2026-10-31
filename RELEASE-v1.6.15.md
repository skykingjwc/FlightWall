# FlightWall v1.6.15

Stabilization pass before automatic-updater work.

- Refines the Delta logo geometry to better match the real two-part widget silhouette.
- Keeps the larger Delta footprint while sharpening the upper chevron and flattening the lower red wedge.
- Hardens manual firmware browser reconnect behavior.
- Probes the actual embedded index.shtml page after reboot.
- Verifies the returned page is FlightWall before navigating.
- Uses no-store/no-cache headers and cache-busting query strings.
- Navigates directly back to index.shtml and forces a follow-up reload as a fallback.
- Leaves the signed automatic updater unchanged for the next dedicated pass.
