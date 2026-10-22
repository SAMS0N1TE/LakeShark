"""CSV to state packs. Every output is reparsed by the firmware host validator.
Build: cmake --build bench/build --target p25pack_validate
Run: python bench/p25pack/make_pack.py input.csv --out output-directory
Optional talkgroup_file: CSV with id,alias,enabled,priority columns.
IDs accept decimal or 0x-prefixed hex. sysid/wacn can also be a slash pair.
"""
import argparse
import csv
from decimal import Decimal, InvalidOperation
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STATES = set("AL AK AZ AR CA CO CT DE DC FL GA HI ID IL IN IA KS KY LA ME MD MA MI MN MS MO MT NE NV NH NJ NM NY NC ND OH OK OR PA RI SC SD TN TX UT VT VA WA WV WI WY".split())

def clean(value):
    value = value.strip()
    if not value or any(ord(c) < 32 or ord(c) > 126 or c in "|=" for c in value):
        raise ValueError("names must be nonempty printable text without | or =")
    return value

def decimal(value):
    n = Decimal(value)
    if not n.is_finite():
        raise ValueError("non-finite numeric value")
    return n

def integer(value):
    value = value.strip()
    return int(value, 16 if value.lower().startswith("0x") else 10)

def produce(source, out, validator):
    packs = {}
    with source.open(newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        required = {"state", "system", "site", "lat", "lon", "range_km", "control_mhz"}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError("missing CSV columns: " + ", ".join(sorted(required - set(reader.fieldnames or []))))
        for row in reader:
            if None in row or any(row.get(key) is None for key in required):
                raise ValueError("incomplete or extra CSV columns")
            state = row["state"].strip().upper()
            if state not in STATES:
                raise ValueError("invalid USPS state")
            lines = ["version=2", "system=" + clean(row["system"]), "site=" + clean(row["site"])]
            ids = {key: (row.get(key) or "").strip() for key in ("sysid", "wacn", "nac")}
            if row.get("sysid/wacn"):
                ids["sysid"], ids["wacn"] = row["sysid/wacn"].split("/")
            for key, value in ids.items():
                if value:
                    lines.append(f"{key}={integer(value)}")
            lat, lon = decimal(row["lat"]), decimal(row["lon"])
            radius = decimal(row["range_km"]) * 1000
            if radius != radius.to_integral_value():
                raise ValueError("range must resolve to whole metres")
            for freq in row["control_mhz"].split(";"):
                hz = decimal(freq) * 1000000
                if hz != hz.to_integral_value():
                    raise ValueError("control must resolve to whole Hz")
                lines.append(f"control={int(hz)}|{lat:f}|{lon:f}|{int(radius)}")
            tgfile = (row.get("talkgroup_file") or row.get("talkgroup file") or "").strip()
            if tgfile:
                with (source.parent / tgfile).open(newline="", encoding="utf-8-sig") as tf:
                    for tg in csv.DictReader(tf):
                        enabled = tg.get("enabled", "true").strip().lower()
                        if enabled not in ("true", "false"):
                            raise ValueError("talkgroup enabled must be true or false")
                        lines.append(f"tg={integer(tg['id'])}|{clean(tg['alias'])}|{enabled}|{integer(tg.get('priority', '0'))}")
            packs.setdefault(state, []).append("\n".join(lines) + "\n")
    if not packs:
        raise ValueError("CSV contains no sites")
    out.mkdir(parents=True, exist_ok=True)
    # Validate every candidate before replacing any output.
    with tempfile.TemporaryDirectory(prefix="p25pack-", dir=out) as tmp:
        for state, records in sorted(packs.items()):
            path = Path(tmp) / f"{state}.txt"
            path.write_text("\n".join(records), encoding="ascii", newline="\n")
            subprocess.run([str(validator), str(path)], check=True)
        for state in sorted(packs):
            os.replace(Path(tmp) / f"{state}.txt", out / f"{state}.txt")
    return sorted(packs)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--validator", type=Path, default=ROOT / "bench/build" / ("p25pack_validate.exe" if os.name == "nt" else "p25pack_validate"))
    a = ap.parse_args()
    try:
        produce(a.csv.resolve(), a.out.resolve(), a.validator.resolve())
    except (ValueError, InvalidOperation, OSError, subprocess.CalledProcessError) as e:
        ap.exit(2, f"Pack generation refused: {e}\n")
if __name__ == "__main__":
    main()
