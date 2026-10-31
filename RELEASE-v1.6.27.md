# FlightWall v1.6.27

Manual-upload protection for automatic updates.

- Keeps the two-minute delay before the first automatic GitHub update check.
- Adds explicit mutual exclusion between manual firmware uploads and GitHub update checks.
- Automatic checks are deferred while the TCP 4242 uploader has an active client.
- GitHub checks are also blocked while the OTA staging path is already active.
- Manual firmware uploads now take priority over background update activity instead of relying on timing alone.
- Retains the callback-driven updater architecture from v1.6.25.
