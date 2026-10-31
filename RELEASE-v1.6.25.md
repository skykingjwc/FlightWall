# FlightWall v1.6.25

Non-blocking updater architecture.

- Update checks no longer sit in a synchronous wait loop on the main thread.
- GitHub HTTP/TLS requests are started asynchronously and completed from lwIP callbacks.
- Routine checks leave the current FlightWall page running and rotating normally.
- The updater no longer frees the TLS configuration merely because a local software timeout expired while lwIP may still own an active connection.
- Firmware downloads also run callback-driven; the display intentionally holds the DOWNLOADING UPDATE screen while the transfer is active.
- The web server and ICMP/network stack are no longer intentionally starved by the main-loop updater wait.
- Signed combined feed, ECDSA verification, Git blob binding, SHA-256 validation, A/B staging, and rollback protection remain unchanged.
