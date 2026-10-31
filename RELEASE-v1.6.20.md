# FlightWall v1.6.20

GitHub API diagnostic UI fix.

- Keeps the v1.6.19 ADS-B-safe ECDSA-only TLS transport test unchanged.
- Replaces the normal CGI form submission with a JavaScript-controlled diagnostic button.
- Polls the embedded status page after starting the test so the final GitHub API result is actually visible.
- Shows CHECK REQUESTED and TESTING GITHUB API while the diagnostic is running.
- Re-enables the test button after a final result or timeout.
- No firmware is downloaded or installed by this diagnostic build.
