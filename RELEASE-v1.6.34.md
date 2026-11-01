# FlightWall v1.6.34

Non-blocking ADS-B refresh.

- Converts the normal ADS-B HTTPS fetch from a blocking wait loop to a callback-driven request.
- Keeps lwIP's proven httpc_get_file_dns() transport and the existing ECDSA-only TLS profile.
- Keeps httpc_connection_t alive for the full asynchronous request lifetime.
- Parses and publishes completed aircraft responses from the main loop instead of the lwIP callback.
- Services completed ADS-B requests during normal display delays and radar animation.
- Prevents GitHub update HTTPS work from starting while an ADS-B request is active.
- Leaves the v1.6.33 compact signed update-check transport unchanged.
