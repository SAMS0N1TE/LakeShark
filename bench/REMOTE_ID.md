# DRONES and BLE Remote ID

The receiver in `components/apps/tui/ls_rid.c` implements the public ASTM
F3411 byte layouts independently. It consumes complete BLE advertising data
from LakeShark's existing BLE owner. It neither connects nor transmits.

The parser accepts service-data AD fields for UUID 0xFFFA with application
code 0x0D, an advertising counter and a validated message. It handles 25-byte
Basic ID, Location, Authentication, Self ID, System and Operator ID messages,
plus bounded message packs. AD framing and message lengths are checked before
publishing table changes. Authentication metadata is displayed but signatures
are not verified; broadcast identities and coordinates are untrusted reports.

The live table holds at most 32 entries and expires contacts after 60 seconds.
DRONES displays identity, signal, age, reported aircraft and operator positions
and decoded flight details. With a receiver GPS fix and a recent reported
aircraft position, it computes distance and true bearing. Otherwise the radar
uses illustrative RSSI ranking, which is not physical distance or bearing.
DRONES keeps no persistent records. The console supports `rid`, `rid detail N`
and `rid clear`.

SWEEP also consumes validated Remote ID observations. Its optional alert log
records uptime, MAC, category, radio and RSSI, without location or payload.
Wi-Fi Remote ID vendor information elements are unavailable through the
current survey adapter; this receiver covers BLE advertisements only.

Host checks in `bench/tests/test_rid.c` exercise message vectors, bounds,
random AD lengths, concurrent scanning/snapshots and fresh-position summaries.
They establish parser behavior, not hardware reception or identity authenticity.
