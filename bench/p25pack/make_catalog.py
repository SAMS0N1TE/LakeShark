"""Create a signed p25/catalog.txt locally. Never deploys.
Uses ota_publish.py's ota_common key loading/trust checks and ECDSA P-256/SHA-256 signing path.
Metadata schema v1: version, tier (free|plus), states mapping USPS codes to
{name, version, tier}; defaults in data/p25/catalog-defaults.json.
"""
import argparse
import hashlib
import importlib.util
import json
import re
import sys
import subprocess
from pathlib import Path
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from make_pack import ROOT, STATES, clean
import os

PUBLISHER = Path(os.environ["LAKESHARK_OTA_TOOLS"]) if os.environ.get("LAKESHARK_OTA_TOOLS") else None

def metadata(path):
    obj = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(obj, dict) or obj.get("schema_version") != 1 or set(obj) - {"schema_version", "version", "tier", "states"}:
        raise ValueError("unsupported metadata schema")
    if not isinstance(obj.get("states"), dict):
        raise ValueError("states must be an object")
    def check(row, partial=False):
        if not isinstance(row, dict) or set(row) - {"name", "version", "tier"}:
            raise ValueError("invalid state metadata")
        if "name" in row and (not isinstance(row["name"], str) or len(clean(row["name"])) > 47):
            raise ValueError("invalid state name")
        if (not partial or "tier" in row) and row.get("tier") not in ("free", "plus"):
            raise ValueError("invalid tier")
        if (not partial or "version" in row) and (not isinstance(row.get("version"), str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-rc[0-9]+)?", row["version"]) or len(row["version"]) > 23):
            raise ValueError("invalid version")
    check({"version": obj.get("version"), "tier": obj.get("tier")})
    for state, row in obj["states"].items():
        if state not in STATES:
            raise ValueError("invalid USPS state in metadata")
        check(row, True)
    return obj

def build_body(packs, meta):
    rows = []
    for path in sorted(packs.glob("*.txt")):
        if path.name == "catalog.txt":
            continue
        state = path.stem
        if state not in STATES:
            raise ValueError("pack filename must be a USPS state")
        content = path.read_bytes()
        if not 0 < len(content) <= 1024 * 1024:
            raise ValueError("pack exceeds size policy")
        entry = {"name": state, "version": meta["version"], "tier": meta["tier"], **meta["states"].get(state, {})}
        rows.append(f"entry={state}|{clean(entry['name'])}|{path.name}|{len(content)}|{hashlib.sha256(content).hexdigest()}|{entry['version']}|{entry['tier']}\n")
    if not rows or len(rows) > 51:
        raise ValueError("catalog needs 1 to 51 state packs")
    payload = "".join(rows)
    body = ("LAKESHARK-OTA 1\nboard=P25-sites\nchannel=catalog\nschema=p25-sites-1\n"
            f"version={meta['version']}\nbuild=p25-sites-{meta['version']}\n"
            f"size={len(payload.encode())}\nsha256={hashlib.sha256(payload.encode()).hexdigest()}\nurl=catalog.txt\n" + payload)
    if len(body.encode()) + 170 > 16384:
        raise ValueError("catalog exceeds firmware capacity")
    return body.encode("ascii")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("packs", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--metadata", type=Path, default=ROOT / "data/p25/catalog-defaults.json")
    ap.add_argument("--publisher-dir", type=Path, default=PUBLISHER)
    ap.add_argument("--header", type=Path, default=ROOT / "main/ls_ota_key.h")
    ap.add_argument("--validator", type=Path, default=ROOT / "bench/build" / ("p25pack_validate.exe" if os.name == "nt" else "p25pack_validate"))
    ap.add_argument("--key", help="PEM path, else LAKESHARK_OTA_KEY; never a default")
    a = ap.parse_args()
    if a.publisher_dir is None:
        ap.error("--publisher-dir or LAKESHARK_OTA_TOOLS: the OTA publisher directory")
    try:
        body = build_body(a.packs, metadata(a.metadata))
        for pack in sorted(a.packs.glob("*.txt")):
            if pack.name != "catalog.txt":
                subprocess.run([str(a.validator.resolve()), str(pack.resolve())], check=True)
        spec = importlib.util.spec_from_file_location("ota_common", a.publisher_dir / "ota_common.py")
        oc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(oc)
        key = oc.load_private_key(oc.key_path(a.key))
        if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
            raise ValueError("signing requires ECDSA P-256")
        if oc.spki(key.public_key()) not in [oc.spki(k) for k in oc.load_header_keys(str(a.header))]:
            raise ValueError("signing key is not trusted by firmware")
        signature = key.sign(body, ec.ECDSA(hashes.SHA256()))
        a.out.parent.mkdir(parents=True, exist_ok=True)
        tmp = a.out.with_suffix(a.out.suffix + ".tmp")
        tmp.write_bytes(body + b"sig=" + signature.hex().encode() + b"\n")
        tmp.replace(a.out)
        print(f"Signed catalog written: {a.out}")
    except (ValueError, OSError, TypeError, UnicodeError, subprocess.CalledProcessError) as e:
        ap.exit(2, f"Catalog generation refused: {e}\n")
if __name__ == "__main__":
    main()
