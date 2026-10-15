"""Validate the actual writer output after test_detector, including privacy."""
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
files = sorted(root.glob("detector-*.jsonl"))
assert len(files) == 8
records = 0
aircraft = 0
for path in files:
    assert path.stat().st_size <= 4096
    for line in path.read_text().splitlines():
        event = json.loads(line)
        assert event["version"] == 1 and event["gps_valid"] is False
        assert event["signal_units"] == "dBm"
        assert len(event["entity"]) == 16 and len(event["boot"]) == 16
        assert not ({"mac", "lat", "lon", "location", "uas_id", "distance_km", "position"} & event.keys())
        if event["source"] == "adsb":
            # Broadcast identifiers only; no signal level, no position, no range.
            assert event["rule_ids"] == ["air-emergency-squawk"] and event["rssi"] is None
            assert len(event["icao"]) == 6 and event["callsign"] == "UAL123" and event["squawk"] == 7700
            assert event["altitude_baro_ft"] in (3500, None) and event["adsb_category"] in ("rotorcraft", None)
            aircraft += 1
        else:
            assert event["source"] == "rid.ble" and event["rule_ids"] == ["remote-id"]
            assert event["band"] == 0 and event["rate_guess_hz"] == 0 and event["hits_per_min"] == 0
        records += 1
assert records > 8 and (root / "keep-me.txt").read_text() == "unrelated"
print(f"PASS: {records} JSONL records ({aircraft} aircraft), 8 bounded files, privacy fields and unrelated file preserved")
