# FlightWall v1.6.29

Direct signed-feed transport diagnostic.

- Fetches update/feed.json directly from raw.githubusercontent.com.
- Removes the GitHub Contents API JSON/base64 envelope from the update-check path.
- The signed manifest and ECDSA signature inside feed.json are unchanged.
- Firmware blob downloads still use api.github.com and remain SHA-256 verified against the signed manifest.
- Retains HTTPS client serialization, manual-upload protection, and the two-minute automatic-check delay.
