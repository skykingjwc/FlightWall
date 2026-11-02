# FlightWall v1.6.37

Small usability and startup-display fixes.

- Keeps manual update checks on the main web page instead of navigating to check_update.cgi.
- Polls the rendered update status in-page until the check completes.
- Removes the short-lived FETCHING AIRCRAFT startup frame now that ADS-B refresh is asynchronous.
- Leaves the last complete startup frame on the panels until the first normal aircraft page is presented.
- No OTA feed staging is triggered by this build.
