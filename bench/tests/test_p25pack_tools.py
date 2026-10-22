"""Production tools end to end with fictional packs and a disposable test key."""
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.exceptions import InvalidSignature

ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = Path(sys.argv[1])
OUT = Path(sys.argv[2]) / "tools"
OUT.mkdir(parents=True, exist_ok=True)
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
sys.path.insert(0, str(ROOT / "bench/p25pack"))
from make_pack import produce
from make_catalog import build_body, metadata, PUBLISHER
sys.argv = [sys.argv[0]]

class PackTools(unittest.TestCase):
    def test_csv_round_trip_uses_firmware_parser(self):
        packs = OUT / "packs"
        self.assertEqual(["MA"], produce(ROOT / "bench/p25pack/sample/sites.csv", packs, VALIDATOR))
        body = (packs / "MA.txt").read_text()
        self.assertIn("sysid=291", body)
        self.assertIn("wacn=781824", body)
        self.assertIn("control=851012500|42.0000000|-71.0000000|25000", body)
        subprocess.run([str(VALIDATOR), str(packs / "MA.txt")], check=True)
        original = (packs / "MA.txt").read_bytes()
        bad = OUT / "bad.csv"
        bad.write_text((ROOT / "bench/p25pack/sample/sites.csv").read_text().replace("42.0000000", "91.0000000"))
        with self.assertRaises(subprocess.CalledProcessError):
            produce(bad, packs, VALIDATOR)
        self.assertEqual(original, (packs / "MA.txt").read_bytes())

    def test_signed_catalog_and_refusals(self):
        if PUBLISHER is None or not (PUBLISHER / "ota_common.py").exists():
            self.skipTest("set LAKESHARK_OTA_TOOLS to the OTA publisher directory")
        packs = OUT / "signed-packs"
        produce(ROOT / "bench/p25pack/sample/sites.csv", packs, VALIDATOR)
        key = ec.generate_private_key(ec.SECP256R1())
        keyfile = OUT / "disposable-test-key.pem"
        keyfile.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode()
        header = OUT / "test-trust.h"
        header.write_text('static const char *KEYS[] = {"' + public.replace('\n', '\\n"\n"') + '"};')
        meta = OUT / "metadata.json"
        obj = {"schema_version": 1, "version": "1.0.0", "tier": "free", "states": {"MA": {"name": "Fictional Massachusetts", "tier": "plus"}}}
        meta.write_text(json.dumps(obj))
        catalog = OUT / "catalog.txt"
        cmd = [sys.executable, "-B", str(ROOT / "bench/p25pack/make_catalog.py"), str(packs), "--out", str(catalog), "--metadata", str(meta), "--key", str(keyfile), "--header", str(header)]
        try:
            subprocess.run(cmd, check=True)
            text = catalog.read_bytes()
            body, sigline = text.rsplit(b"sig=", 1)
            signature = bytes.fromhex(sigline.decode().strip())
            key.public_key().verify(signature, body, ec.ECDSA(hashes.SHA256()))
            with self.assertRaises(InvalidSignature):
                key.public_key().verify(signature, body.replace(b"plus", b"free"), ec.ECDSA(hashes.SHA256()))
            self.assertIn(hashlib.sha256((packs / "MA.txt").read_bytes()).hexdigest().encode(), body)
            self.assertIn(b"|1.0.0|plus", body)
            # Production trust list must refuse this disposable key.
            badcmd = cmd.copy(); badcmd[-1] = str(ROOT / "main/ls_ota_key.h")
            refused = subprocess.run(badcmd, capture_output=True, text=True)
            self.assertNotEqual(0, refused.returncode)
            self.assertIn("not trusted", refused.stderr)
            self.assertEqual(text, catalog.read_bytes())
        finally:
            keyfile.unlink(missing_ok=True)
        obj["states"]["MA"]["tier"] = "premium"; meta.write_text(json.dumps(obj))
        with self.assertRaises(ValueError):
            metadata(meta)

if __name__ == "__main__":
    unittest.main()
