"""Validate the actual writer output after test_detector, including privacy."""
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
files = sorted(root.glob("detector-*.jsonl"))
assert len(files) == 8
records = 0
for path in files:
    assert path.stat().st_size <= 4096
    for line in path.read_text().splitlines():
        event = json.loads(line)
        assert event["version"] == 1 and event["gps_valid"] is False
        assert event["source"] == "rid.ble" and event["rule_ids"] == ["remote-id"]
        assert event["signal_units"] == "dBm"
        assert event["band"] == 0 and event["rate_guess_hz"] == 0 and event["hits_per_min"] == 0
        assert len(event["entity"]) == 16 and len(event["boot"]) == 16
        assert not ({"mac", "lat", "lon", "location", "uas_id"} & event.keys())
        records += 1
assert records > 8 and (root / "keep-me.txt").read_text() == "unrelated"
print(f"PASS: {records} JSONL records, 8 bounded files, privacy fields and unrelated file preserved")
