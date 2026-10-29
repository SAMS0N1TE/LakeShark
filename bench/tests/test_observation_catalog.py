"""Native store fixtures and adversarial offline export checks."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("catalog", ROOT / "tools/observation_catalog.py")
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)

# Field names/types follow scan_journal_format_json and the DETECTOR app's det_log_line.
SCAN = dict(schema=1, event="hold", session_id=7, sequence=1, uptime_ms=1234,
            unix_time=0, source="channels", mode=0, channel="WX", reason="signal",
            requested_hz=162550000, effective_known=True, effective_hz=162550000,
            power_pct=80, radio_error=0, manual_hold=False, gps_valid=False,
            gps_age_ms=0, latitude=None, longitude=None,
            counts=dict(tunes=2, holds=1, releases=0, receiver_errors=0, queue_dropped=0))
DET = dict(version=1, revision=3, boot="0123456789abcdef", sequence=1,
           uptime_us=9007199254740993, utc=None, source="adsb", rule_ids=["emergency"],
           entity="fedcba9876543210", category="aircraft", rssi=None,
           validated_message=True, signal_units="dBm", band=0, rate_guess_hz=0,
           hits_per_min=1, icao="ABC123", callsign="TEST", squawk=7700,
           emergency_state=1, altitude_baro_ft=10000, adsb_category="light",
           gps_valid=False, test=False, selected=1, allowed=1, accepted=1,
           failed=0, coalesced=0, dropped=0, suppressed=0)
IQ = ("Filetype: LakeShark SubGhz IQ File\nVersion: 1\nFrequency: 433920000\n"
      "Protocol: LR2021_IQ\nSampleRate: 125000\nSamples: 2\nIQ_Data: 0080FF7F0100FFFF\n")
SIDE = dict(radio="LR2021", time="2026-10-25T18:00:00Z", freq_hz=433920000,
            gain_tenths=0, bw_hz=31250, sample_rate=125000, edges=0, span_us=16,
            mag_peak=0, mag_floor=0, board="T-Display-P4", firmware="c0a23a4-dirty")
MISSION = dict(schema=2, type="mission_session", scope="bounded_fm_dsp_ledger",
               time_scope="current_boot_only", mission_id="1", name="test <mission>",
               active=False, started_us="9007199254740993", stopped_us="9007199254741993",
               retained_entries="1", overwritten_entries="0", clock_regressions="0",
               counter_saturated=False, delivered_u8_iq_bytes="480", delivered_blocks="1",
               boot_id=None, rf_coverage_us=None, sample_continuity=None, sample_frequency_verified=False)
ENTRY = dict(schema=2, type="mission_entry", mission_id="1", sequence="1", kind="fm_segment",
             at_us="9007199254741993", last_delivery_us="9007199254741993", generation="2",
             state="active", configured_center_hz="162550000", nominal_rate_hz="240000",
             delivered_u8_iq_bytes="480", delivered_blocks="1", nominal_sample_duration_us="1000",
             clock_regressed=False, counter_saturated=False, rf_coverage_us=None,
             sample_continuity=None, sample_frequency_verified=False, wall_interval_observation="unknown")
RX_SNAPSHOT = dict(rx_state="active", captured_us=9007199254740993, app="FM", source="RTL-SDR",
                   endpoint="usb", owner="FM", mode="FM", requested_hz=162550000, effective_hz=162550000,
                   sample_rate_hz=240000, bandwidth_hz=200000, gain_mode=0,
                   requested_gain_tenths_db=-10, effective_gain_tenths_db=-10,
                   context_epoch=1, consumer_epoch=1, iq_epoch=1, decoder_epoch=1,
                   iq_bytes_total=100, frame_count=None, valid_count=None, failed_count=None,
                   control_count=None, voice_count=None, audio_drop_count=None)
TRIAL = dict(schema_version=1, type="receiver_trial", state="COUNTER DELTAS ONLY", baseline_set=True,
             compared=True, time_invalid=False, stale=False, inactive=False, elapsed_us=1000,
             changed_mask=0, unknown_mask=0, reset_mask=0, delta_known_mask=1, iq_delta=480,
             frame_delta=None, valid_delta=None, failed_delta=None, control_delta=None, voice_delta=None,
             audio_drop_delta=None, before=RX_SNAPSHOT,
             after=dict(RX_SNAPSHOT, captured_us=9007199254741993, iq_bytes_total=580),
             rf_coverage=None, calibration=None, direction=None)
PLAN = dict(schema=3, type="rx_plan_session", plan_id="1", name="two steps", scope="bounded_rtl_fm_plan",
            time_scope="current_boot_only", state="completed", reason="ok", active=False,
            repeat_count="1", current_repeat="0", current_step="1", step_count="2",
            started_us="9007199254740993", ended_us="9007199256740993", clock_invalid=False,
            calls_suppressed=False, restore_attempted=True, restored=True, restore_center_hz="162550000",
            recording_policy="suppress_fm_auto_calls_during_run", rf_coverage_us=None,
            sample_continuity=None, sample_frequency_verified=False, boot_id=None, detail_truncated=False)
PLAN_STEP = dict(schema=3, type="rx_plan_step", plan_id="1", step_index="0", planned_center_hz="162400000",
                 planned_dwell_ms="500", attempts="1", completed="1", first_tune_us="9007199254740993",
                 last_tune_us="9007199254740993", last_dwell_started_us="9007199254840993",
                 last_dwell_ended_us="9007199255340993", delivered_u8_iq_bytes="480", delivered_blocks="1",
                 software_read_dsp_window_us="4000", nominal_rate_hz="240000", nominal_sample_duration_us="1000",
                 first_delivery_us="9007199254840993", last_delivery_us="9007199254840993",
                 counter_saturated=False, timing_invalid=False, loss_unknown=True, detail_truncated=False)
COVER_UNKNOWN = dict(physical_rf_coverage_us=None, hardware_loss_bytes=None, sample_continuity=None,
                     device_timestamps=None, per_sample_rf_frequency_hz=None, sample_frequency_verified=False)
COVERAGE = dict(schema=4, type="coverage_session", session_id="1", scope="software_observed_rtl_fm",
                time_scope="current_boot_only", active=False, calls_suppressed=False, started_us="9007199254740993",
                stopped_us="9007199254890993", last_update_us="9007199254890993", timing_invalid=False,
                counter_saturated=False, unknown_us="34000", receiving_us="4000", settling_us="110000",
                busy_us="1000", unavailable_us="1000", delivered_bytes="480", delivered_blocks="1",
                settling_discarded_bytes="960", ring_rejected_bytes="64", transfer_error_events="1",
                submission_error_events="0", flush_discarded_bytes="2048", provenance_rejected_blocks="1",
                retained_intervals="5", overwritten_intervals="0", settling_guard_us="100000",
                settling_guard_blocks="2", settling_policy="software_policy_not_calibrated",
                software_loss_scope="observed_transport_counter_deltas_only", boot_id=None, **COVER_UNKNOWN)
COVER_INTERVAL = dict(schema=4, type="coverage_interval", session_id="1", sequence="3",
                     started_us="9007199254884993", ended_us="9007199254888993", state="receiving",
                     context_known=True, calls_suppressed=True, context_epoch="3", stream_epoch="4", configured_center_hz="162550000",
                     nominal_rate_hz="240000", delivered_bytes="480", delivered_blocks="1",
                     settling_discarded_bytes="0", nominal_sample_duration_us="1000", plan_id="7",
                     plan_step="0", plan_repeat="0", **COVER_UNKNOWN)


def coverage_intervals():
    result = []
    cursor = int(COVERAGE["started_us"])
    for index, (state, duration) in enumerate((("unknown", 34000), ("settling", 110000),
                                              ("receiving", 4000), ("busy", 1000), ("unavailable", 1000))):
        known = state in ("receiving", "settling")
        result.append(dict(COVER_INTERVAL, sequence=str(index+1), started_us=str(cursor),
                           ended_us=str(cursor+duration), state=state, context_known=known,
                           context_epoch="3" if known else None, stream_epoch="4" if known else None,
                           configured_center_hz="162550000" if known else None, nominal_rate_hz="240000" if known else None,
                           delivered_bytes="480" if state == "receiving" else "0",
                           delivered_blocks="1" if state == "receiving" else "0",
                           settling_discarded_bytes="960" if state == "settling" else "0",
                           nominal_sample_duration_us="1000" if state == "receiving" else "0" if known else None))
        cursor += duration
    return result


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.card = self.base / "card"
        self.card.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def write(self, path, raw):
        target = self.card / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw.encode() if isinstance(raw, str) else raw)
        return target

    def record(self, path, value):
        return self.write(path, json.dumps(value) + "\n")

    def run_catalog(self, **options):
        return catalog.Catalog(self.card, **options).run()

    def test_native_sources_unknown_clocks_and_privacy(self):
        self.record("lakeshark/scan/scan_a.jsonl", SCAN)
        self.record("detector/events/detector-0.jsonl", DET)
        self.write("lakeshark/calls/unknown/one.wav.meta", "0,100,851000000,42,123,8000,0,NaN,poison\n")
        self.write("lakeshark/calls/unknown/one.wav", b"RIFFunverified-content")
        self.write("lakeshark/capture.sub", "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 433920000\nPreset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\nRAW_Data: 100 -200\n")
        self.record("lakeshark/capture.json", SIDE)
        result = self.run_catalog()
        self.assertEqual(len(result.records), 4)
        self.assertEqual(result.issues, [])
        for rec in result.records:
            self.assertIsNone(rec["time"]["utc_unix"])
            self.assertEqual(rec["relations"], [])
            self.assertFalse(rec["location"]["valid"])
            self.assertIsNone(rec["location"]["latitude"])
        detector = next(r for r in result.records if r["store"] == "detector")
        self.assertEqual(detector["time"]["monotonic"], "9007199254740993")
        audio = next(r for r in result.records if r["store"] == "call")
        self.assertEqual(audio["details"]["source"], 123)  # unit ID, not an invented receiver name
        self.assertIsNone(audio["provenance"]["radio"])
        self.assertIsNone(audio["artifacts"][1]["sha256"])
        self.assertEqual(result.summary()["coverage"], "not measured")

    def test_valid_location_and_clock(self):
        rec = dict(SCAN, unix_time=1792951200, gps_valid=True, latitude=42.0, longitude=-71.0)
        self.record("lakeshark/scan/a.jsonl", rec)
        result = self.run_catalog()
        self.assertEqual(result.records[0]["location"]["latitude"], 42)
        self.assertEqual(result.records[0]["time"]["utc_unix"], 1792951200)

    def test_id_scoping_rotation_duplicates_and_conflicts(self):
        self.record("lakeshark/scan/first.jsonl", SCAN)
        self.record("lakeshark/scan/reboot.jsonl", SCAN)
        self.record("detector/events/detector-0.jsonl", DET)
        self.record("detector/events/detector-1.jsonl", DET)
        altered = dict(DET, category="different")
        self.record("detector/events/detector-2.jsonl", altered)
        result = self.run_catalog()
        self.assertEqual(len(result.records), 4)
        detectors = [r for r in result.records if r["store"] == "detector"]
        self.assertEqual(len(detectors[0]["locations"]), 2)
        self.assertNotEqual(detectors[0]["id"], detectors[1]["id"])
        self.assertEqual([r["id"] for r in result.records], [r["id"] for r in self.run_catalog().records])
        self.assertTrue(any("conflicting native identity" in i["reason"] for i in result.issues))

    def test_malformed_truncated_types_nonfinite_and_invalid_location(self):
        examples = [b'{"schema":1}\n', b'{"schema":1,"schema":1}\n', b'[]\n',
                    json.dumps(dict(SCAN, sequence=True)).encode()+b'\n',
                    json.dumps(dict(SCAN, gps_valid=True, latitude=91, longitude=0)).encode()+b'\n',
                    json.dumps(dict(SCAN, extra=float("nan"))).encode()+b'\n',
                    json.dumps(SCAN).replace('"schema": 1', '"extra": 1e999, "schema": 1').encode()+b'\n',
                    json.dumps(SCAN).encode()]
        for i, raw in enumerate(examples):
            self.write(f"lakeshark/scan/{i}.jsonl", raw)
        result = self.run_catalog()
        self.assertEqual(len(result.records), 0)
        self.assertEqual(result.issue_count, len(examples))
        self.assertTrue(result.summary()["partial"])

    def test_bounds_are_explicit(self):
        self.write("lakeshark/scan/a.jsonl", "x"*100 + "\n")
        result = self.run_catalog(limits=dict(line_bytes=64))
        self.assertIn("line size limit", result.issues[0]["reason"])
        self.record("lakeshark/scan/a.jsonl", SCAN)
        self.record("lakeshark/scan/b.jsonl", dict(SCAN, sequence=2))
        for limits, reason in [(dict(records=1), "record limit"), (dict(files=1), "file limit"),
                               (dict(entries=1), "directory entry limit"), (dict(read_bytes=10), "truncated"),
                               (dict(metadata_bytes=1), "metadata file size limit")]:
            with self.subTest(limits=limits):
                self.write("lakeshark/one.sub", IQ)
                result = self.run_catalog(limits=limits)
                self.assertTrue(any(reason in i["reason"] for i in result.issues), result.issues)

    def test_missing_audio_and_bad_call_csv(self):
        self.write("lakeshark/calls/unknown/missing.wav.meta", "0,100,851000000,42,123,8000,0,99,999\n")
        self.write("lakeshark/calls/unknown/bad.wav.meta", "0,100,851000000,42,123,8000,1,NaN,0\n")
        self.write("lakeshark/calls/unknown/short.wav.meta", "1,2,3\n")
        result = self.run_catalog()
        self.assertEqual(len(result.records), 1)
        self.assertEqual(result.issue_count, 3)
        self.assertIsNone(result.records[0]["location"]["latitude"])

    def test_all_native_rec_directories_and_missing_sidecar_are_visible(self):
        pulse = "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 433920000\nProtocol: RAW\nRAW_Data: 100 -200\n"
        for relative in ("lakeshark/iq.sub", "subghz/export.sub", "lakeshark/rec/catch_123_1.sub"):
            self.write(relative, IQ if "iq.sub" in relative else pulse)
        self.record("lakeshark/iq.json", SIDE)
        result = self.run_catalog(sigmf=True)
        self.assertEqual(len(result.records), 3)
        self.assertEqual(len(result.converted), 1)
        self.assertEqual(result.issue_count, 2)  # these pulse exports have no sidecar

    def test_symlink_not_read(self):
        outside = self.base / "private.jsonl"
        outside.write_text(json.dumps(SCAN)+"\n")
        link = self.card / "lakeshark/scan/link.jsonl"
        link.parent.mkdir(parents=True)
        try:
            link.symlink_to(outside)
        except OSError:
            self.skipTest("OS lacks symlink privilege")
        result = self.run_catalog()
        self.assertEqual(result.records, [])
        self.assertIn("symlink", result.issues[0]["reason"])

    def test_sigmf_is_byte_preserving_and_clock_is_not_invented(self):
        self.write("lakeshark/iq.sub", IQ)
        self.record("lakeshark/iq.json", SIDE)
        result = self.run_catalog(sigmf=True)
        self.assertEqual(result.issues, [])
        out = catalog.write_export(result, self.base / "export")
        data = next(out.glob("*.sigmf-data")).read_bytes()
        self.assertEqual(data, bytes.fromhex("0080FF7F0100FFFF"))
        self.assertEqual(struct.unpack("<hhhh", data), (-32768, 32767, 1, -1))
        meta = json.loads(next(out.glob("*.sigmf-meta")).read_text())
        self.assertEqual(meta["global"]["core:datatype"], "ci16_le")
        self.assertEqual(meta["global"]["core:sample_rate"], 125000)
        self.assertEqual(meta["global"]["core:sha512"], hashlib.sha512(data).hexdigest())
        self.assertEqual(meta["captures"], [{"core:frequency": 433920000, "core:sample_start": 0}])
        self.assertIn("nominal", meta["global"]["core:description"])
        self.assertFalse(any("datetime" in c for c in meta["captures"]))

    def test_sigmf_rejects_unsupported_or_ambiguous_input(self):
        self.record("lakeshark/iq.json", SIDE)
        cases = [IQ.rstrip("\n"), IQ.replace("Samples: 2", "Samples: 3"), IQ.replace("0080FF7F", "0080FF7"),
                 IQ + "RAW_Data: 100 -100\n", IQ.replace("125000", "664800"),
                 IQ.replace("Version: 1", "Version: 2")]
        for value in cases:
            with self.subTest(value=value):
                self.write("lakeshark/iq.sub", value)
                result = self.run_catalog(sigmf=True)
                self.assertFalse(result.converted)
                self.assertTrue(result.issues)
        self.write("lakeshark/iq.sub", IQ)
        self.record("lakeshark/iq.json", dict(SIDE, sample_rate=664800))
        self.assertFalse(self.run_catalog(sigmf=True).converted)

    def test_offline_html_escapes_input_and_export_is_new_atomic_directory(self):
        payload = '</script><script>alert("x")</script>&'
        self.record("lakeshark/scan/a.jsonl", dict(SCAN, channel=payload))
        result = self.run_catalog()
        before = (self.card / "lakeshark/scan/a.jsonl").read_bytes()
        out = catalog.write_export(result, self.base / "export")
        page = (out / "index.html").read_text(encoding="utf-8")
        self.assertNotIn(payload, page)
        self.assertIn("&lt;/script&gt;", page)
        self.assertIn("addEventListener('input'", page)
        self.assertNotIn("src=\"http", page)
        self.assertEqual(before, (self.card / "lakeshark/scan/a.jsonl").read_bytes())
        with self.assertRaises(catalog.Invalid):
            catalog.write_export(result, out)
        with self.assertRaises(catalog.Invalid):
            catalog.write_export(result, self.card / "inside")
        self.assertEqual(list(self.base.glob(".catalog-*")), [])
        with patch.object(catalog, "render_html", side_effect=OSError("simulated output failure")):
            with self.assertRaises(OSError):
                catalog.write_export(result, self.base / "failed")
        self.assertFalse((self.base / "failed").exists())
        self.assertEqual(list(self.base.glob(".catalog-*")), [])

    def test_cli_partial_exit_and_machine_export(self):
        self.record("detector/events/detector-0.jsonl", DET)
        self.write("detector/events/detector-1.jsonl", "truncated")
        out = self.base / "output"
        proc = subprocess.run([sys.executable, "-B", str(ROOT / "tools/observation_catalog.py"),
                               str(self.card), "--output", str(out)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2, proc.stderr)
        record = json.loads((out / "catalog.jsonl").read_text())
        self.assertEqual(record["native_id"]["boot"], DET["boot"])
        self.assertTrue(json.loads((out / "summary.json").read_text())["partial"])

    def receipt(self, values, name="receipt.jsonl"):
        path = self.base / name
        path.write_text("".join(json.dumps(v) + "\n" for v in values), encoding="utf-8")
        return path

    def test_receipt_v1_v2_identity_native_references_and_no_time_joins(self):
        self.record("lakeshark/scan/a.jsonl", SCAN)
        receipt = self.receipt([dict(schema=1, type="scanner_journal", path="/sdcard/lakeshark/scan/a.jsonl",
                                     initialized=True, session_active=True, storage_ok=True, records_written="1",
                                     queue_dropped="0", write_errors="0", bytes_written="100"), MISSION, ENTRY])
        result = self.run_catalog()
        result.import_receipt(receipt)
        self.assertEqual(result.issue_count, 0)
        self.assertEqual(len(result.records), 4)
        self.assertEqual(result.records[-1]["details"]["mission_id"], "1")
        self.assertEqual(result.records[-1]["time"]["monotonic"], ENTRY["at_us"])
        self.assertIsNone(result.records[-1]["time"]["boot"])
        self.assertTrue(all(r["relations"] == [] for r in result.records))
        self.assertEqual(result.records[1]["artifacts"][1]["path"], "lakeshark/scan/a.jsonl")
        result.import_receipt(receipt)  # Identical saved invocation is not counted twice.
        self.assertEqual(len(result.records), 4)
        other = self.receipt([dict(ENTRY, at_us="10")], "other.jsonl")
        result.import_receipt(other)
        self.assertNotEqual(result.records[-1]["id"], result.records[-2]["id"])

    def test_receipt_rejects_claims_truncation_bounds_and_unsafe_paths(self):
        examples = [dict(ENTRY, sequence=True), dict(ENTRY, rf_coverage_us="100"),
                    dict(ENTRY, sample_frequency_verified=True), dict(ENTRY, at_us="18446744073709551616"),
                    dict(ENTRY, mission_id=None), dict(ENTRY, schema=3), dict(ENTRY, wall_interval_observation="observed"),
                    dict(MISSION, boot_id="invented"), dict(MISSION, name=5), dict(MISSION, name="bad\nname"),
                    dict(MISSION, name="x"*32), dict(ENTRY, state=[]),
                    dict(schema=1, type="call_artifact", path="/sdcard/../private.wav")]
        result = self.run_catalog()
        result.import_receipt(self.receipt(examples))
        self.assertEqual(result.records, [])
        self.assertEqual(result.issue_count, len(examples))
        receipt = self.receipt([ENTRY])
        receipt.write_bytes(receipt.read_bytes().rstrip(b"\n"))
        result.import_receipt(receipt)
        self.assertIn("truncated", result.issues[-1]["reason"])
        receipt.write_bytes(b"x" * 65537)
        with self.assertRaises(catalog.Invalid):
            result.import_receipt(receipt)
        receipt = self.receipt([ENTRY] * 65)
        result = self.run_catalog().import_receipt(receipt)
        self.assertEqual(len(result.records), 64)
        self.assertIn("receipt line limit", result.issues[-1]["reason"])

    def test_bundle_portability_verification_and_native_reanalysis(self):
        self.write("lakeshark/iq.sub", IQ)
        self.record("lakeshark/iq.json", SIDE)
        self.write("lakeshark/calls/unknown/one.wav.meta", "0,100,851000000,42,123,8000,0,0,0\n")
        self.write("lakeshark/calls/unknown/one.wav", b"RIFFexact audio bytes")
        receipt = self.receipt([MISSION, ENTRY])
        result = self.run_catalog(sigmf=True).import_receipt(receipt)
        second_receipt = self.receipt([TRIAL], "trial-receipt.jsonl")
        result.import_receipt(second_receipt)
        ids = [r["id"] for r in result.records]
        out = catalog.write_export(result, self.base / "bundle", bundle=True)
        manifest, report = catalog.verify_bundle(out)
        self.assertTrue(report["byte_integrity_ok"], report)
        self.assertFalse(report["source_partial"])
        self.assertEqual(manifest["settings"]["sigmf_requested"], True)
        self.assertEqual(manifest["source_identities"][1]["provenance"]["firmware"], SIDE["firmware"])
        page = (out / "index.html").read_text()
        self.assertIn('href="sources/card/lakeshark/iq.sub"', page)
        self.assertIn('href="bundle.json"', page)
        self.assertNotIn('href="file:', page)
        self.card.rename(self.base / "removed-card")
        receipt.unlink()
        second_receipt.unlink()
        rebuilt, second = catalog.reindex_bundle(out, self.base / "rebuilt", sigmf=True)
        self.assertEqual([r["id"] for r in rebuilt.records], ids)
        self.assertEqual(next(second.glob("*.sigmf-data")).read_bytes(), bytes.fromhex("0080FF7F0100FFFF"))
        self.assertTrue(catalog.verify_bundle(second)[1]["byte_integrity_ok"])
        with self.assertRaises(catalog.Invalid):
            catalog.reindex_bundle(out, out / "nested")

    def test_bundle_tamper_missing_and_mutated_source_refuse_reindex(self):
        source = self.record("lakeshark/scan/a.jsonl", SCAN)
        out = catalog.write_export(self.run_catalog(), self.base / "bundle", bundle=True)
        copied = out / "sources/card/lakeshark/scan/a.jsonl"
        copied.write_bytes(copied.read_bytes().replace(b'"WX"', b'"XY"'))
        _, report = catalog.verify_bundle(out)
        self.assertFalse(report["byte_integrity_ok"])
        with self.assertRaises(catalog.Invalid):
            catalog.reindex_bundle(out, self.base / "bad-reindex")
        copied.unlink()
        missing = catalog.verify_bundle(out)[1]["failures"]
        self.assertEqual([f["path"] for f in missing], ["sources/card/lakeshark/scan/a.jsonl"])
        result = self.run_catalog()
        source.write_text(json.dumps(dict(SCAN, channel="changed")) + "\n")
        changed = catalog.write_export(result, self.base / "changed", bundle=True)
        self.assertTrue(json.loads((changed / "bundle.json").read_text())["partial"])
        self.assertIn("source changed", result.issues[-1]["reason"])

    def test_manifest_validates_all_paths_before_member_reads(self):
        self.record("lakeshark/scan/a.jsonl", SCAN)
        out = catalog.write_export(self.run_catalog(), self.base / "bundle", bundle=True)
        manifest_path = out / "bundle.json"
        original = json.loads(manifest_path.read_text())
        for unsafe in ("../outside", "/absolute", "C:/escape", "a\\escape", "a/../b", "a//b", "a/NUL", "x:stream"):
            with self.subTest(path=unsafe):
                manifest = dict(original, files=[*original["files"], dict(bundle_path=unsafe, bytes=0,
                                 sha256="0"*64, role="generated")])
                manifest_path.write_text(json.dumps(manifest))
                with patch.object(catalog, "stream_hash", side_effect=AssertionError("must not read any member")):
                    with self.assertRaises(catalog.Invalid):
                        catalog.verify_bundle(out)
        manifest_path.write_text(json.dumps(original))
        excessive = json.loads(json.dumps(original))
        excessive["files"][0]["bytes"] = catalog.BUNDLE_LIMITS["file_bytes"] + 1
        manifest_path.write_text(json.dumps(excessive))
        with patch.object(catalog, "stream_hash", side_effect=AssertionError("must validate bounds before reads")):
            with self.assertRaises(catalog.Invalid):
                catalog.verify_bundle(out)
        manifest_path.write_text(json.dumps(original))
        target = out / "sources/card/lakeshark/scan/a.jsonl"
        outside = self.base / "outside"
        outside.write_text("private")
        target.unlink()
        try:
            target.symlink_to(outside)
        except OSError:
            return  # Path cases above remain tested without Windows symlink privilege.
        with self.assertRaises(catalog.Invalid):
            catalog.verify_bundle(out)

    def test_bundle_limits_partial_invalid_input_and_unlisted_sources(self):
        self.write("lakeshark/scan/a.jsonl", "truncated")
        result = self.run_catalog()
        out = catalog.write_export(result, self.base / "bundle", bundle=True)
        self.assertEqual((out / "sources/card/lakeshark/scan/a.jsonl").read_text(), "truncated")
        self.assertTrue(catalog.verify_bundle(out)[1]["source_partial"])
        (out / "sources/card/lakeshark/scan/unlisted.jsonl").write_text(json.dumps(SCAN) + "\n")
        rebuilt, _ = catalog.reindex_bundle(out, self.base / "reindexed")
        self.assertEqual(rebuilt.records, [])
        self.assertTrue(any("not a manifest member" in i["reason"] for i in rebuilt.issues))
        self.record("lakeshark/scan/a.jsonl", SCAN)
        limited = self.run_catalog()
        with patch.dict(catalog.BUNDLE_LIMITS, file_bytes=4):
            out = catalog.write_export(limited, self.base / "limited", bundle=True)
        manifest = json.loads((out / "bundle.json").read_text())
        self.assertTrue(manifest["partial"])
        self.assertEqual(len(manifest["omitted_sources"]), 1)
        self.assertFalse((out / "sources/card/lakeshark/scan/a.jsonl").exists())

    def test_bundle_cli_exit_codes(self):
        self.record("lakeshark/scan/a.jsonl", SCAN)
        out = self.base / "bundle"
        tool = str(ROOT / "tools/observation_catalog.py")
        def cli(*args):
            return subprocess.run([sys.executable, "-B", tool, *map(str, args)], capture_output=True, text=True)
        self.assertEqual(cli(self.card, "--output", out, "--bundle").returncode, 0)
        verified = cli("--verify-bundle", out)
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertTrue(json.loads(verified.stdout)["byte_integrity_ok"])
        rebuilt = cli("--reindex-bundle", out, "--output", self.base / "rebuild")
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stderr)
        (out / "catalog.jsonl").write_text("tamper")
        self.assertEqual(cli("--verify-bundle", out).returncode, 1)

    def test_receiver_trial_native_schema_preserves_unknowns_and_exact_integers(self):
        result = self.run_catalog().import_receipt(self.receipt([TRIAL]))
        self.assertEqual(result.issues, [])
        rec = result.records[0]
        self.assertEqual(rec["details"]["after"]["captured_us"], "9007199254741993")
        self.assertEqual(rec["details"]["before"]["requested_gain_tenths_db"], "-10")
        self.assertEqual(rec["details"]["iq_delta"], "480")
        self.assertIsNone(rec["details"]["frame_delta"])
        self.assertIsNone(rec["details"]["rf_coverage"])
        self.assertIsNone(rec["provenance"]["firmware"])
        self.assertEqual(rec["relations"], [])
        out = catalog.write_export(result, self.base / "trial-bundle", bundle=True)
        self.assertTrue(catalog.verify_bundle(out)[1]["byte_integrity_ok"])
        rebuilt, _ = catalog.reindex_bundle(out, self.base / "trial-reindex")
        self.assertEqual(rebuilt.records[0]["id"], rec["id"])

    def test_receiver_trial_rejects_invalid_types_claims_and_comparability(self):
        examples = [dict(TRIAL, schema_version=True), dict(TRIAL, calibration="calibrated"),
                    dict(TRIAL, stale=True), dict(TRIAL, delta_known_mask=True),
                    dict(TRIAL, changed_mask=8), dict(TRIAL, reset_mask=1),
                    dict(TRIAL, unknown_mask=128), dict(TRIAL, iq_delta=None),
                    dict(TRIAL, after=dict(RX_SNAPSHOT, iq_bytes_total=True)),
                    dict(TRIAL, compared=False)]
        result = self.run_catalog().import_receipt(self.receipt(examples))
        self.assertEqual(result.records, [])
        self.assertEqual(result.issue_count, len(examples))

    def test_rx_plan_explicit_control_times_and_portable_order(self):
        steps = [PLAN_STEP, dict(PLAN_STEP, step_index="1", planned_center_hz="162550000")]
        receipt = self.receipt([PLAN, *steps])
        result = self.run_catalog().import_receipt(receipt)
        self.assertEqual(result.issues, [])
        self.assertEqual(result.records[0]["details"]["started_us"], PLAN["started_us"])
        self.assertTrue(all(r["relations"] == [] for r in result.records))
        out = catalog.write_export(result, self.base / "plan", bundle=True)
        page = (out / "index.html").read_text()
        self.assertIn("planned versus actual control", page)
        self.assertIn("500 ms planned", page)
        self.assertIn("1 attempts / 1 completed", page)
        self.assertIn("earn no observation time", page)
        rebuilt, _ = catalog.reindex_bundle(out, self.base / "plan-reindexed")
        self.assertEqual([r["id"] for r in result.records], [r["id"] for r in rebuilt.records])

    def test_rx_plan_incomplete_and_clock_invalid_never_look_complete(self):
        result = self.run_catalog().import_receipt(self.receipt([dict(PLAN, clock_invalid=True), PLAN_STEP]))
        self.assertTrue(any("invalid clock" in i["reason"] for i in result.issues))
        self.assertTrue(any("incomplete" in i["reason"] for i in result.issues))
        page = catalog.render_rx_plans(result)
        self.assertIn("Partial receipt or invalid clock", page)
        bad = [dict(PLAN, repeat_count="0"), dict(PLAN, current_step="2"), dict(PLAN, state=[]),
               dict(PLAN, rf_coverage_us="500000"), dict(PLAN, restored=True, restore_attempted=False),
               dict(PLAN_STEP, completed="2"), dict(PLAN_STEP, planned_center_hz=True),
               dict(PLAN_STEP, nominal_sample_duration_us="4000"), dict(PLAN_STEP, loss_unknown=False),
               dict(PLAN_STEP, counter_saturated=True), dict(PLAN_STEP, timing_invalid=True)]
        invalid = self.run_catalog().import_receipt(self.receipt(bad, "invalid-plan.jsonl"))
        self.assertEqual(invalid.records, [])
        self.assertEqual(invalid.issue_count, len(bad))
        saturated = dict(PLAN_STEP, counter_saturated=True, delivered_u8_iq_bytes=None,
                         delivered_blocks=None, software_read_dsp_window_us=None, nominal_sample_duration_us=None)
        valid_partial = self.run_catalog().import_receipt(self.receipt([
            dict(PLAN, step_count="1", current_step="0"), saturated], "saturated-step.jsonl"))
        self.assertEqual(len(valid_partial.records), 2)
        self.assertIsNone(valid_partial.records[1]["details"]["software_read_dsp_window_us"])

    def test_software_coverage_separates_actual_windows_nominal_samples_and_loss(self):
        result = self.run_catalog().import_receipt(self.receipt([COVERAGE, *coverage_intervals()]))
        self.assertEqual(result.issues, [])
        page = catalog.render_software_coverage(result)
        for state in ("receiving", "settling", "busy", "unavailable", "unknown"):
            self.assertIn(f"coverage-{state}", page)
        self.assertIn("4000 µs software interval", page)
        self.assertIn("1000 µs nominal samples", page)
        self.assertIn("ring_rejected_bytes=64", page)
        self.assertIn("transfer_error_events=1", page)
        self.assertIn("Hardware loss remains unknown", page)
        self.assertIn("FM auto-CALLS suppressed: True", page)
        self.assertIn("separate receipts are not joined", page)
        self.assertTrue(all(record["relations"] == [] for record in result.records))

    def test_software_coverage_overflow_truncation_and_clock_flags_limit_timeline(self):
        intervals = coverage_intervals()[-2:]
        result = self.run_catalog().import_receipt(self.receipt([
            dict(COVERAGE, retained_intervals="2", overwritten_intervals="3"), *intervals]))
        self.assertTrue(result.issues)
        page = catalog.render_software_coverage(result)
        self.assertIn("Partial or limited receipt", page)
        self.assertIn("Unreported interval: unknown", page)
        invalid_time = dict(COVERAGE, timing_invalid=True, **{key: None for key in
            ("unknown_us", "receiving_us", "settling_us", "busy_us", "unavailable_us")})
        result = self.run_catalog().import_receipt(self.receipt([invalid_time, *coverage_intervals()], "clock.jsonl"))
        page = catalog.render_software_coverage(result)
        self.assertNotIn('style="width:', page)
        self.assertIn("No valid elapsed timeline available", page)
        missing = self.run_catalog().import_receipt(self.receipt([COVERAGE, COVER_INTERVAL], "missing.jsonl"))
        self.assertTrue(any("incomplete" in issue["reason"] for issue in missing.issues))
        wrong_totals = self.run_catalog().import_receipt(self.receipt([
            dict(COVERAGE, receiving_us="5000"), *coverage_intervals()], "totals.jsonl"))
        self.assertTrue(any("totals disagree" in issue["reason"] for issue in wrong_totals.issues))
        self.assertNotIn('style="width:', catalog.render_software_coverage(wrong_totals))

    def test_software_coverage_rejects_false_rf_time_counts_and_context(self):
        bad = [dict(COVERAGE, physical_rf_coverage_us="4000"), dict(COVERAGE, receiving_us=4000),
               dict(COVERAGE, timing_invalid=True), dict(COVERAGE, hardware_loss_bytes="64"),
               dict(COVER_INTERVAL, nominal_sample_duration_us="4000"),
               dict(COVER_INTERVAL, delivered_bytes="481"), dict(COVER_INTERVAL, state="settling"),
               dict(COVER_INTERVAL, context_known=False), dict(COVER_INTERVAL, plan_id=None),
               dict(COVER_INTERVAL, ended_us="1"), dict(COVER_INTERVAL, sample_frequency_verified=True)]
        result = self.run_catalog().import_receipt(self.receipt(bad))
        self.assertEqual(result.records, [])
        self.assertEqual(result.issue_count, len(bad))

    def test_plan_coverage_and_mission_bundle_reindex_keep_order_ids_and_unknowns(self):
        result = self.run_catalog()
        for index, values in enumerate(([PLAN, PLAN_STEP, dict(PLAN_STEP, step_index="1")],
                                        [COVERAGE, *coverage_intervals()], [MISSION, ENTRY])):
            result.import_receipt(self.receipt(values, f"{index}.jsonl"))
        self.assertEqual(result.issues, [])
        out = catalog.write_export(result, self.base / "combined", bundle=True)
        self.assertTrue(catalog.verify_bundle(out)[1]["byte_integrity_ok"])
        rebuilt, second = catalog.reindex_bundle(out, self.base / "combined-reindexed")
        self.assertEqual([r["id"] for r in result.records], [r["id"] for r in rebuilt.records])
        self.assertEqual([r["record_sha256"] for r in result.records], [r["record_sha256"] for r in rebuilt.records])
        page = (second / "index.html").read_text()
        self.assertIn("planned versus actual control", page)
        self.assertIn("Measured software observation intervals", page)
        self.assertNotIn('href="file:', page)
        self.assertTrue(catalog.verify_bundle(second)[1]["byte_integrity_ok"])

    def test_plan_step_aggregates_survive_partial_recent_coverage_ring(self):
        plan = [dict(PLAN, detail_truncated=True), dict(PLAN_STEP, detail_truncated=True),
                dict(PLAN_STEP, step_index="1", planned_center_hz="162550000", detail_truncated=True,
                     delivered_u8_iq_bytes="960", delivered_blocks="2", software_read_dsp_window_us="7000",
                     nominal_sample_duration_us="2000")]
        result = self.run_catalog().import_receipt(self.receipt(plan, "complete-plan-partial-detail.jsonl"))
        result.import_receipt(self.receipt([dict(COVERAGE, retained_intervals="2", overwritten_intervals="3"),
                                           *coverage_intervals()[-2:]], "recent-only.jsonl"))
        page = catalog.render_rx_plans(result)
        self.assertIn("162400000 Hz", page)
        self.assertIn("162550000 Hz", page)
        self.assertIn("480 bytes / 1 blocks", page)
        self.assertIn("960 bytes / 2 blocks", page)
        self.assertIn("7000 µs summed read/DSP windows", page)
        self.assertIn("Independent per-step delivery aggregates", page)
        out = catalog.write_export(result, self.base / "step-aggregates", bundle=True)
        self.assertTrue(catalog.verify_bundle(out)[1]["source_partial"])
        rebuilt, _ = catalog.reindex_bundle(out, self.base / "step-aggregates-reindexed")
        self.assertEqual([r["id"] for r in result.records], [r["id"] for r in rebuilt.records])


if __name__ == "__main__":
    unittest.main()
