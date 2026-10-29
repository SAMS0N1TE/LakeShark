#!/usr/bin/env python3
"""Read-only, bounded host catalog of LakeShark SD artifacts (GPL-3.0-or-later)."""
from __future__ import annotations

import argparse
import csv
import hashlib
import html
import json
import math
import os
from pathlib import Path
import re
import shutil
import tempfile
from urllib.parse import quote

VERSION = 1
LIMITS = dict(files=512, entries=20000, records=4096, line_bytes=8192,
              metadata_bytes=1048576, read_bytes=32 * 1024 * 1024, issues=512)
BUNDLE_LIMITS = dict(files=1024, file_bytes=16 * 1024 * 1024,
                     total_bytes=64 * 1024 * 1024, receipt_bytes=65536, receipt_lines=64)


def relative_name(value):
    """Portable names only; validate before stat/open, including on Windows."""
    text(value, "relative path", 512)
    if (not value or any(ord(c) < 32 or c in '\\:<>"|?*' for c in value) or value.startswith("/") or
            any(p in ("", ".", "..") or p.rstrip(" .") != p or
                re.fullmatch(r"(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", p, re.I)
                for p in value.split("/"))):
        raise Invalid("unsafe relative path")
    return value


def safe_member(root, name):
    name = relative_name(name)
    path = root / name
    for part in (path, *path.parents):
        if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
            raise Invalid("symlink/junction reference refused")
        if part == root:
            break
    if not path.resolve().is_relative_to(root):
        raise Invalid("reference escapes root")
    return path


def stream_hash(path, maximum, destination=None):
    before = path.stat()
    if not path.is_file() or before.st_size > maximum:
        raise Invalid("bundle file size limit or non-file source")
    result = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while True:
            chunk = source.read(min(65536, maximum - size + 1))
            if not chunk:
                break
            size += len(chunk)
            if size > maximum:
                raise Invalid("bundle byte limit; source grew")
            result.update(chunk)
            if destination is not None:
                destination.write(chunk)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns) or size != before.st_size:
        raise Invalid("source changed while copying/hashing")
    return size, result.hexdigest()


class Invalid(ValueError):
    pass


def integer(value, name, minimum=0, maximum=(1 << 64) - 1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise Invalid(f"invalid {name}")
    return value


def number(value, name, minimum, maximum):
    if type(value) not in (int, float) or not math.isfinite(value) or not minimum <= value <= maximum:
        raise Invalid(f"invalid {name}")
    return value


def text(value, name, maximum=256):
    if not isinstance(value, str) or len(value) > maximum:
        raise Invalid(f"invalid {name}")
    return value


def boolean(value, name):
    if type(value) is not bool:
        raise Invalid(f"invalid {name}")
    return value


def strict_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise Invalid(f"duplicate JSON key {key}")
            result[key] = value
        return result
    def constant(value):
        raise Invalid(f"nonfinite JSON value {value}")
    def finite_float(value):
        result = float(value)
        if not math.isfinite(result):
            raise Invalid("nonfinite JSON number")
        return result
    try:
        value = json.loads(raw, object_pairs_hook=pairs, parse_constant=constant, parse_float=finite_float)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise Invalid(f"invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise Invalid("JSON record is not an object")
    return value


def location(record):
    valid = boolean(record.get("gps_valid", False), "gps_valid")
    if not valid:
        return {"valid": False, "latitude": None, "longitude": None}
    return {"valid": True,
            "latitude": number(record.get("latitude"), "latitude", -90, 90),
            "longitude": number(record.get("longitude"), "longitude", -180, 180)}


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def receipt_uint(value, name, nullable=False, minimum=0, maximum=(1 << 64)-1):
    if value is None and nullable:
        return None
    if not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]{0,19}", value):
        raise Invalid(f"invalid decimal receipt field {name}")
    return integer(int(value), name, minimum, maximum)


def require_fields(record, fields):
    if any(key not in record for key in fields.split()):
        raise Invalid("missing required receipt fields")


def unknown_fields(record, fields):
    for key in fields.split():
        if key not in record or record[key] is not None:
            raise Invalid(f"receipt {key} must remain unknown")


def rx_plan_record(record):
    """Plan metadata and actual control events do not establish observation."""
    integer(record.get("schema"), "RX plan schema", 3, 3)
    kind = record.get("type")
    receipt_uint(record.get("plan_id"), "plan_id", minimum=1, maximum=0xffffffff)
    if kind == "rx_plan_session":
        require_fields(record, "name scope time_scope state reason active repeat_count current_repeat current_step step_count started_us ended_us clock_invalid calls_suppressed restore_attempted restored restore_center_hz recording_policy sample_frequency_verified detail_truncated")
        name = text(record["name"], "plan name", 31)
        if not name or any(not 32 <= ord(c) <= 126 for c in name):
            raise Invalid("invalid plan name")
        if record["scope"] != "bounded_rtl_fm_plan" or record["time_scope"] != "current_boot_only":
            raise Invalid("unsupported RX plan scope")
        states = {"loaded", "queued", "tuning", "settling", "dwelling", "restoring", "completed", "stopped", "cancelled", "failed"}
        if text(record["state"], "RX plan state", 32) not in states:
            raise Invalid("unsupported RX plan state")
        text(record["reason"], "plan reason", 128)
        for key in "active clock_invalid calls_suppressed restore_attempted restored sample_frequency_verified detail_truncated".split():
            boolean(record[key], key)
        count = receipt_uint(record["step_count"], "step_count", minimum=1, maximum=8)
        repeats = receipt_uint(record["repeat_count"], "repeat_count", minimum=1, maximum=4)
        receipt_uint(record["current_step"], "current_step", maximum=count-1)
        receipt_uint(record["current_repeat"], "current_repeat", maximum=repeats-1)
        start = receipt_uint(record["started_us"], "started_us", nullable=True)
        end = receipt_uint(record["ended_us"], "ended_us", nullable=True)
        receipt_uint(record["restore_center_hz"], "restore_center_hz", nullable=True, minimum=1)
        if record["state"] == "loaded" and (record["active"] or start is not None or end is not None):
            raise Invalid("loaded RX plan has execution timestamps/state")
        if record["active"] and end is not None:
            raise Invalid("active RX plan has terminal time")
        if not record["clock_invalid"] and start is not None and end is not None and end < start:
            raise Invalid("RX plan clock reversed without invalid flag")
        if record["restored"] and not record["restore_attempted"]:
            raise Invalid("RX plan restoration contradicts attempt flag")
        if record["recording_policy"] != "suppress_fm_auto_calls_during_run" or record["sample_frequency_verified"]:
            raise Invalid("unsupported RX plan policy/frequency claim")
        unknown_fields(record, "rf_coverage_us sample_continuity boot_id")
    elif kind == "rx_plan_step":
        require_fields(record, "step_index planned_center_hz planned_dwell_ms attempts completed first_tune_us last_tune_us last_dwell_started_us last_dwell_ended_us delivered_u8_iq_bytes delivered_blocks software_read_dsp_window_us nominal_rate_hz nominal_sample_duration_us first_delivery_us last_delivery_us counter_saturated timing_invalid loss_unknown detail_truncated")
        receipt_uint(record["step_index"], "step_index", maximum=7)
        receipt_uint(record["planned_center_hz"], "planned_center_hz", minimum=1)
        receipt_uint(record["planned_dwell_ms"], "planned_dwell_ms", minimum=1)
        attempts = receipt_uint(record["attempts"], "attempts", maximum=4)
        receipt_uint(record["completed"], "completed", maximum=attempts)
        for key in "first_tune_us last_tune_us last_dwell_started_us last_dwell_ended_us".split():
            receipt_uint(record[key], key, nullable=True)
        for key in "counter_saturated timing_invalid detail_truncated".split():
            boolean(record[key], key)
        if record["loss_unknown"] is not True:
            raise Invalid("per-step loss must remain unknown")
        for key in "delivered_u8_iq_bytes delivered_blocks".split():
            if (record[key] is None) != record["counter_saturated"]:
                raise Invalid("RX plan aggregate count availability mismatch")
            receipt_uint(record[key], key, nullable=record["counter_saturated"])
        invalid_window = record["timing_invalid"] or record["counter_saturated"]
        if (record["software_read_dsp_window_us"] is None) != invalid_window:
            raise Invalid("RX plan aggregate software time availability mismatch")
        receipt_uint(record["software_read_dsp_window_us"], "software_read_dsp_window_us", nullable=invalid_window)
        rate = receipt_uint(record["nominal_rate_hz"], "nominal_rate_hz", nullable=True, minimum=1)
        nominal = receipt_uint(record["nominal_sample_duration_us"], "nominal_sample_duration_us", nullable=True)
        if nominal is not None and (rate is None or record["delivered_u8_iq_bytes"] is None or
                nominal != (int(record["delivered_u8_iq_bytes"])//2)*1000000//rate):
            raise Invalid("RX plan aggregate nominal duration disagrees with count/rate")
        if record["delivered_u8_iq_bytes"] is not None and int(record["delivered_u8_iq_bytes"]) % 2:
            raise Invalid("RX plan aggregate incomplete IQ pairs")
        for key in "first_delivery_us last_delivery_us".split():
            receipt_uint(record[key], key, nullable=True)
        if (record["first_delivery_us"] is None) != (record["last_delivery_us"] is None):
            raise Invalid("RX plan aggregate delivery endpoint availability mismatch")
    else:
        raise Invalid("unsupported RX plan record")
    return record


def coverage_record(record):
    """Software read/DSP windows, not physical RF presence or continuity."""
    integer(record.get("schema"), "coverage schema", 4, 4)
    receipt_uint(record.get("session_id"), "coverage session_id", minimum=1, maximum=0xffffffff)
    unknown_fields(record, "physical_rf_coverage_us hardware_loss_bytes sample_continuity device_timestamps per_sample_rf_frequency_hz")
    if record.get("sample_frequency_verified") is not False:
        raise Invalid("coverage sample RF frequency remains unverified")
    kind = record.get("type")
    boolean(record.get("calls_suppressed"), "calls_suppressed")
    if kind == "coverage_session":
        require_fields(record, "scope time_scope active started_us stopped_us last_update_us timing_invalid counter_saturated unknown_us receiving_us settling_us busy_us unavailable_us delivered_bytes delivered_blocks settling_discarded_bytes ring_rejected_bytes transfer_error_events submission_error_events flush_discarded_bytes provenance_rejected_blocks retained_intervals overwritten_intervals settling_guard_us settling_guard_blocks settling_policy software_loss_scope")
        if record["scope"] != "software_observed_rtl_fm" or record["time_scope"] != "current_boot_only":
            raise Invalid("unsupported software coverage scope")
        for key in "active timing_invalid counter_saturated".split():
            boolean(record[key], key)
        start = receipt_uint(record["started_us"], "started_us")
        last = receipt_uint(record["last_update_us"], "last_update_us")
        stop = receipt_uint(record["stopped_us"], "stopped_us", nullable=True)
        if (stop is None) != record["active"]:
            raise Invalid("coverage terminal time/active mismatch")
        if not record["timing_invalid"] and (last < start or (stop is not None and stop < start)):
            raise Invalid("coverage clock reversed without invalid flag")
        invalid = record["timing_invalid"] or record["counter_saturated"]
        for key in "unknown_us receiving_us settling_us busy_us unavailable_us".split():
            if (record[key] is None) != invalid:
                raise Invalid("coverage duration availability contradicts clock/saturation flags")
            receipt_uint(record[key], key, nullable=invalid)
        for key in "delivered_bytes delivered_blocks settling_discarded_bytes ring_rejected_bytes transfer_error_events submission_error_events flush_discarded_bytes provenance_rejected_blocks".split():
            if (record[key] is None) != record["counter_saturated"]:
                raise Invalid("coverage counter availability contradicts saturation flag")
            receipt_uint(record[key], key, nullable=record["counter_saturated"])
        receipt_uint(record["retained_intervals"], "retained_intervals", maximum=32)
        receipt_uint(record["overwritten_intervals"], "overwritten_intervals")
        receipt_uint(record["settling_guard_us"], "settling_guard_us", minimum=100000, maximum=100000)
        receipt_uint(record["settling_guard_blocks"], "settling_guard_blocks", minimum=2, maximum=2)
        if record["settling_policy"] != "software_policy_not_calibrated" or record["software_loss_scope"] != "observed_transport_counter_deltas_only":
            raise Invalid("unsupported software coverage policy/loss scope")
        unknown_fields(record, "boot_id")
    elif kind == "coverage_interval":
        require_fields(record, "sequence started_us ended_us state context_known context_epoch stream_epoch configured_center_hz nominal_rate_hz delivered_bytes delivered_blocks settling_discarded_bytes nominal_sample_duration_us plan_id plan_step plan_repeat")
        receipt_uint(record["sequence"], "sequence", minimum=1)
        start = receipt_uint(record["started_us"], "started_us")
        end = receipt_uint(record["ended_us"], "ended_us")
        if end < start:
            raise Invalid("reversed software interval endpoints")
        if text(record["state"], "coverage state", 16) not in ("unknown", "receiving", "settling", "busy", "unavailable"):
            raise Invalid("unsupported software interval state")
        known = boolean(record["context_known"], "context_known")
        for key in "context_epoch stream_epoch configured_center_hz nominal_rate_hz".split():
            if (record[key] is not None) != known:
                raise Invalid("coverage context availability mismatch")
            receipt_uint(record[key], key, nullable=not known, minimum=1)
        for key in "delivered_bytes delivered_blocks settling_discarded_bytes nominal_sample_duration_us".split():
            receipt_uint(record[key], key, nullable=True)
        if record["delivered_bytes"] is not None and int(record["delivered_bytes"]) % 2:
            raise Invalid("incomplete delivered IQ pair count")
        if not known and record["nominal_sample_duration_us"] is not None:
            raise Invalid("nominal sample duration has unknown sample rate")
        if record["nominal_sample_duration_us"] is not None:
            if record["delivered_bytes"] is None:
                raise Invalid("nominal sample duration has unknown sample count")
            nominal = (int(record["delivered_bytes"])//2)*1000000//int(record["nominal_rate_hz"])
            if int(record["nominal_sample_duration_us"]) != nominal:
                raise Invalid("nominal sample duration disagrees with native count/rate")
        if record["state"] == "receiving" and (not known or record["delivered_bytes"] == "0" or record["delivered_blocks"] == "0"):
            raise Invalid("receiving interval lacks qualified sample evidence")
        if record["state"] != "receiving" and record["delivered_bytes"] not in (None, "0"):
            raise Invalid("non-receiving interval claims delivered samples")
        plan = receipt_uint(record["plan_id"], "plan_id", nullable=True, minimum=1, maximum=0xffffffff)
        for key, maximum in (("plan_step", 7), ("plan_repeat", 3)):
            if plan is None and record[key] is not None:
                raise Invalid("coverage native plan binding incomplete")
            receipt_uint(record[key], key, nullable=True, maximum=maximum)
    else:
        raise Invalid("unsupported coverage record")
    return record


def receiver_trial(record):
    """Validate the native diagnostic receipt; keep unmeasured evidence null."""
    integer(record.get("schema_version"), "receiver trial schema", 1, 1)
    text(record.get("state"), "receiver trial state", 64)
    flags = "baseline_set compared time_invalid stale inactive".split()
    for key in flags:
        boolean(record.get(key), key)
    masks = dict(changed_mask=511, unknown_mask=511, reset_mask=127, delta_known_mask=127)
    for key, maximum in masks.items():
        value = record.get(key)
        if key not in record or (value is None) == record["compared"]:
            raise Invalid(f"invalid receiver trial {key} availability")
        if value is not None:
            integer(value, key, maximum=maximum)
    elapsed = record.get("elapsed_us")
    if "elapsed_us" not in record or (elapsed is not None) != (record["compared"] and not record["time_invalid"]):
        raise Invalid("invalid receiver trial elapsed availability")
    if elapsed is not None:
        integer(elapsed, "elapsed_us")
    for key in ("rf_coverage", "calibration", "direction"):
        if key not in record or record[key] is not None:
            raise Invalid(f"receiver trial {key} must remain unknown")
    string_fields = "rx_state app source endpoint owner mode".split()
    numeric_fields = ("captured_us requested_hz effective_hz sample_rate_hz bandwidth_hz gain_mode "
                      "requested_gain_tenths_db effective_gain_tenths_db context_epoch consumer_epoch iq_epoch "
                      "decoder_epoch iq_bytes_total frame_count valid_count failed_count control_count voice_count audio_drop_count").split()
    for key, present in (("before", record["baseline_set"]), ("after", record["compared"])):
        snapshot = record.get(key)
        if key not in record or (snapshot is not None) != present or (present and not isinstance(snapshot, dict)):
            raise Invalid(f"invalid receiver trial {key} snapshot")
        if not present:
            continue
        for field in string_fields + numeric_fields:
            if field not in snapshot:
                raise Invalid("missing receiver trial snapshot field")
            value = snapshot[field]
            if value is None:
                if field == "rx_state":
                    raise Invalid("missing receiver state")
            elif field in string_fields:
                text(value, field, 128)
            elif field in ("gain_mode", "requested_gain_tenths_db", "effective_gain_tenths_db"):
                integer(value, field, -(1 << 31), (1 << 31)-1)
            else:
                integer(value, field)
    if record["compared"] and not record["baseline_set"]:
        raise Invalid("receiver comparison has no baseline")
    known = record["delta_known_mask"] or 0
    changed = (record["changed_mask"] or 0) | (record["unknown_mask"] or 0)
    if known and (any(record[k] for k in ("time_invalid", "stale", "inactive")) or changed & 127 or known & (record["reset_mask"] or 0)):
        raise Invalid("receiver trial deltas contradict comparability flags")
    if (changed & 128 and known & 1) or (changed & 256 and known & 126):
        raise Invalid("receiver trial deltas contradict counter epochs")
    for bit, key in enumerate("iq_delta frame_delta valid_delta failed_delta control_delta voice_delta audio_drop_delta".split()):
        value = record.get(key)
        if key not in record or (value is not None) != bool(known & (1 << bit)):
            raise Invalid("receiver trial delta/mask mismatch")
        if value is not None:
            integer(value, key)
    # The native diagnostic writer uses JSON integers; normalized browser records
    # retain all bits as decimal strings. Original bytes remain in the receipt.
    def normalize(value):
        if type(value) is int:
            return str(value)
        if isinstance(value, dict):
            return {k: normalize(v) for k, v in value.items()}
        return value
    result = normalize(record)
    result["schema_version"] = 1
    return result


class Catalog:
    def __init__(self, root, limits=None, sigmf=False, allowed_sources=None):
        self.root = Path(root).resolve(strict=True)
        if not self.root.is_dir():
            raise Invalid("card root must be a directory")
        self.limits = dict(LIMITS, **(limits or {}))
        self.sigmf = sigmf
        self.records = []
        self.issues = []
        self.issue_count = 0
        self.read_bytes = 0
        self.files = 0
        self.entries = 0
        self.by_id = {}
        self.native = {}
        self.converted = {}
        self.stopped = False
        self.source_stamps = {}
        self.receipts = {}
        self.bundle_links = {}
        self.bundle_manifest = None
        self.allowed_sources = allowed_sources

    def issue(self, path, reason, line=None):
        self.issue_count += 1
        if len(self.issues) < self.limits["issues"]:
            self.issues.append(dict(path=str(path), line=line, reason=reason))

    def relative(self, path):
        return Path(path).relative_to(self.root).as_posix()

    def safe(self, path):
        path = Path(path)
        if self.allowed_sources is not None and self.relative(path) not in self.allowed_sources:
            raise Invalid("source is not a manifest member")
        # Refuse symlinks/junctions even when their target happens to be inside.
        for part in (path, *path.parents):
            if part == self.root:
                break
            if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
                raise Invalid("symlink/junction reference refused")
        if not path.resolve().is_relative_to(self.root):
            raise Invalid("reference escapes card root")
        if not path.is_file():
            raise Invalid("missing or non-file artifact")
        stat = path.stat()
        self.source_stamps.setdefault(self.relative(path), (stat.st_size, stat.st_mtime_ns))
        return path

    def read(self, path, maximum=None):
        path = self.safe(path)
        maximum = maximum or self.limits["metadata_bytes"]
        size = path.stat().st_size
        if size > maximum:
            raise Invalid("metadata file size limit; file omitted")
        if self.read_bytes + size > self.limits["read_bytes"]:
            raise Invalid("total input byte limit; file omitted")
        with path.open("rb") as stream:
            raw = stream.read(maximum + 1)
        self.read_bytes += len(raw)
        if len(raw) > maximum:
            raise Invalid("metadata grew beyond size limit")
        return raw

    def artifact(self, path, role, raw=None):
        path = self.safe(path)
        return dict(path=self.relative(path), role=role, bytes=path.stat().st_size,
                    sha256=digest(raw) if raw is not None else None)

    def add(self, store, path, raw, native, scope, summary, **fields):
        if len(self.records) >= self.limits["records"]:
            self.issue(self.relative(path), "record limit; remaining input omitted")
            self.stopped = True
            return None
        raw_hash = digest(raw)
        identity = json.dumps([store, scope, native, raw_hash], sort_keys=True).encode()
        record_id = f"{store}:{digest(identity)}"
        locator = {"path": self.relative(path), "line": fields.pop("line", None)}
        if record_id in self.by_id:
            self.by_id[record_id]["locations"].append(locator)
            return self.by_id[record_id]
        key = json.dumps([store, scope, native], sort_keys=True)
        if key in self.native and self.native[key] != raw_hash:
            self.issue(self.relative(path), "conflicting native identity; both records retained", locator["line"])
        self.native[key] = raw_hash
        record = dict(catalog_version=VERSION, id=record_id, store=store,
                      native_id=native, native_scope=scope, record_sha256=raw_hash,
                      locations=[locator], summary=summary, relations=[],
                      relation_status="unknown; no cross-store association inferred",
                      provenance={"board": None, "firmware": None, "radio": None, "mode": None},
                      location={"valid": False, "latitude": None, "longitude": None}, **fields)
        self.records.append(record)
        self.by_id[record_id] = record
        return record

    def scan_record(self, path, raw, line):
        r = strict_json(raw)
        if r.get("schema") != 1 or type(r.get("schema")) is not int:
            raise Invalid("unsupported scanner schema")
        session = integer(r.get("session_id"), "session_id", maximum=0xffffffff)
        seq = integer(r.get("sequence"), "sequence", maximum=0xffffffff)
        uptime = integer(r.get("uptime_ms"), "uptime_ms")
        utc = integer(r.get("unix_time"), "unix_time", minimum=-(1 << 63), maximum=(1 << 63)-1)
        event = text(r.get("event"), "event", 32)
        channel = text(r.get("channel", ""), "channel", 64)
        mode = integer(r.get("mode"), "mode", -1, 127)
        requested = integer(r.get("requested_hz"), "requested_hz", maximum=0xffffffff)
        effective = integer(r.get("effective_hz"), "effective_hz", maximum=0xffffffff)
        known = boolean(r.get("effective_known"), "effective_known")
        loc = location(r)
        rec = self.add("scanner", path, raw, dict(session_id=str(session), sequence=str(seq)),
                       self.relative(path), f"{event}: {channel}", line=line,
                       time=dict(utc_unix=utc if utc > 0 else None, monotonic=str(uptime),
                                 monotonic_unit="ms", boot=None),
                       details=dict(event=event, channel=channel, reason=text(r.get("reason", ""), "reason"),
                                    requested_hz=requested, effective_hz=effective if known else None),
                       artifacts=[dict(path=self.relative(path), role="scanner JSONL", sha256=None)])
        if rec:
            rec["location"] = loc
            rec["provenance"]["mode"] = str(mode)

    def detector_record(self, path, raw, line):
        r = strict_json(raw)
        if type(r.get("version")) is not int or r["version"] != 1:
            raise Invalid("unsupported detector version")
        boot = text(r.get("boot"), "boot", 16)
        if not re.fullmatch(r"[0-9a-fA-F]{16}", boot):
            raise Invalid("invalid detector boot")
        seq = integer(r.get("sequence"), "sequence", maximum=0xffffffff)
        uptime = integer(r.get("uptime_us"), "uptime_us")
        utc = r.get("utc")
        if utc is not None:
            integer(utc, "utc", maximum=(1 << 63)-1)
        source = text(r.get("source"), "source", 32)
        valid = boolean(r.get("validated_message"), "validated_message")
        entity = text(r.get("entity"), "entity", 16)
        if not re.fullmatch(r"[0-9a-fA-F]{16}", entity):
            raise Invalid("invalid detector entity")
        details = dict(source=source, validated_message=valid, entity=entity,
                       category=text(r.get("category", ""), "category", 64))
        for key in ("revision", "band", "rate_guess_hz", "hits_per_min", "selected", "allowed",
                    "accepted", "failed", "coalesced", "dropped", "suppressed"):
            if key in r:
                details[key] = integer(r[key], key)
        if r.get("rssi") is not None:
            details["rssi"] = number(r["rssi"], "rssi", -10000, 10000)
        if "rule_ids" in r:
            if not isinstance(r["rule_ids"], list) or len(r["rule_ids"]) > 64:
                raise Invalid("invalid rule_ids")
            details["rule_ids"] = [text(value, "rule ID", 128) for value in r["rule_ids"]]
        for key in ("callsign", "icao", "signal_units"):
            if key in r:
                details[key] = text(r[key], key)
        if r.get("gps_valid") is not False:
            raise Invalid("detector v1 requires gps_valid=false")
        self.add("detector", path, raw, dict(boot=boot.lower(), sequence=str(seq)), "detector boot",
                 f"{source}: {details['category']} ({'validated' if valid else 'unvalidated'})", line=line,
                 time=dict(utc_unix=utc if utc else None, monotonic=str(uptime), monotonic_unit="us", boot=boot),
                 details=details, artifacts=[dict(path=self.relative(path), role="detector JSONL", sha256=None)])

    def import_receipt(self, path):
        """A saved invocation is its own scope, never a persistent boot/event ID."""
        path = Path(path).absolute()
        for part in (path, *path.parents):
            if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
                raise Invalid("symlink/junction receipt refused")
        if path.stat().st_size > BUNDLE_LIMITS["receipt_bytes"]:
            raise Invalid("receipt size limit")
        before = path.stat()
        with path.open("rb") as stream:
            raw = stream.read(BUNDLE_LIMITS["receipt_bytes"] + 1)
        if len(raw) > BUNDLE_LIMITS["receipt_bytes"]:
            raise Invalid("receipt size limit")
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise Invalid("receipt changed while reading")
        receipt_id = digest(raw)
        if receipt_id in self.receipts:
            return
        self.receipts[receipt_id] = dict(path=path, raw=raw)
        kinds = {1: {"snapshot", "scanner_journal", "detector_event", "call_artifact", "fm_dsp_iq", "receiver_trial", "error"},
                 2: {"mission_session", "mission_entry", "error"},
                 3: {"rx_plan_session", "rx_plan_step", "error"},
                 4: {"coverage_session", "coverage_interval", "error"}}
        count = 0
        for count, line in enumerate(raw.splitlines(keepends=True), 1):
            if count > BUNDLE_LIMITS["receipt_lines"]:
                self.issue(f"receipt:{receipt_id}", "receipt line limit; remaining input omitted")
                break
            try:
                if len(line) > self.limits["line_bytes"] or not line.endswith(b"\n"):
                    raise Invalid("oversized or unterminated/truncated receipt line")
                r = strict_json(line)
                trial = r.get("type") == "receiver_trial"
                schema = integer(r.get("schema_version") if trial else r.get("schema"), "receipt schema", 1, 4)
                kind = text(r.get("type"), "receipt type", 32)
                if kind not in kinds[schema]:
                    raise Invalid("unsupported receipt type")
                if trial:
                    r = receiver_trial(r)
                structured = schema in (3, 4) and kind != "error"
                if structured:
                    r = rx_plan_record(r) if schema == 3 else coverage_record(r)
                required = {
                    "snapshot": "scope uptime_us atomic_across_stores time_scope detector_snapshot_available",
                    "scanner_journal": "initialized session_active storage_ok records_written queue_dropped write_errors bytes_written path",
                    "detector_event": "sequence revision uptime_us source validated_message test locator boot_id artifact_link",
                    "call_artifact": "path utc frequency_hz talkgroup source_unit_id pcm_rate_hz duration_ms artifact_bytes protected artifact_verified",
                    "fm_dsp_iq": "scope known segment_open state segment nominal_rate_hz delivered_u8_iq_bytes counter_saturated rf_coverage_us",
                    "mission_session": "scope time_scope mission_id name active started_us stopped_us retained_entries overwritten_entries clock_regressions counter_saturated delivered_u8_iq_bytes delivered_blocks boot_id rf_coverage_us sample_continuity sample_frequency_verified",
                    "mission_entry": "mission_id sequence kind at_us last_delivery_us generation state configured_center_hz nominal_rate_hz delivered_u8_iq_bytes delivered_blocks nominal_sample_duration_us counter_saturated clock_regressed rf_coverage_us sample_frequency_verified wall_interval_observation",
                    "error": "reason partial",
                }
                if not trial and not structured and any(key not in r for key in required[kind].split()):
                    raise Invalid("missing required receipt fields")
                bools = {"atomic_across_stores", "detector_snapshot_available", "initialized", "session_active",
                         "storage_ok", "validated_message", "test", "protected", "artifact_verified", "known",
                         "segment_open", "counter_saturated", "active", "clock_regressed", "partial", "sample_frequency_verified"}
                for key, value in (() if trial or structured else r.items()):
                    if key in ("schema", "type"):
                        continue
                    if value is not None and isinstance(value, str):
                        text(value, key, 512)
                    elif value is not None and type(value) not in (bool, int, list):
                        raise Invalid(f"invalid receipt field {key}")
                    if key in bools:
                        boolean(value, key)
                    if key in ("scope", "time_scope", "source", "state", "reason", "kind", "name", "wall_interval_observation"):
                        text(value, key, 128)
                    if isinstance(value, list) and (len(value) > 32 or any(not isinstance(v, str) or len(v) > 64 for v in value)):
                        raise Invalid(f"invalid receipt list {key}")
                    if key.endswith(("_us", "_hz", "_bytes", "_blocks")) or key in (
                            "sequence", "revision", "mission_id", "segment", "generation", "retained_entries",
                            "overwritten_entries", "clock_regressions", "delivered_iq_pairs", "records_written",
                            "queue_dropped", "write_errors", "accepted_sinks", "failed_sinks", "coalesced",
                            "dropped", "suppressed", "utc", "talkgroup", "source_unit_id", "duration_ms",
                            "detector_recent_total", "detector_exported", "calls_recent_available"):
                        if value is not None and (not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]{0,19}", value)
                                                  or int(value) > (1 << 64)-1):
                            raise Invalid(f"invalid decimal receipt field {key}")
                for key in ("rf_coverage", "rf_coverage_us", "sample_continuity", "relationships"):
                    if r.get(key) is not None:
                        raise Invalid(f"receipt {key} must remain unknown")
                if r.get("sample_frequency_verified", False) is not False:
                    raise Invalid("receipt sample frequency is unverified")
                if kind == "mission_entry" and r.get("kind") not in ("start", "fm_segment", "boundary", "stop"):
                    raise Invalid("unsupported mission entry kind")
                if kind == "mission_entry" and r["wall_interval_observation"] != "unknown":
                    raise Invalid("mission wall observation must remain unknown")
                if kind == "mission_session" and (r["scope"] != "bounded_fm_dsp_ledger" or r["time_scope"] != "current_boot_only"):
                    raise Invalid("unsupported mission scope")
                if kind == "mission_session" and (not 1 <= len(r["name"]) <= 31 or
                                                  any(not 32 <= ord(c) <= 126 for c in r["name"])):
                    raise Invalid("invalid mission name")
                if r.get("boot_id") is not None:
                    raise Invalid("receipt persistent boot identity is unavailable")
                if schema == 2:
                    for key in (("mission_id",) if kind == "mission_session" else
                                ("mission_id", "sequence", "kind", "at_us") if kind == "mission_entry" else ()):
                        if r.get(key) is None:
                            raise Invalid(f"missing mission field {key}")
                artifacts = [dict(path=f"receipt:{receipt_id}", role="saved device receipt",
                                  bytes=len(raw), sha256=receipt_id)]
                native_path = r.get("path") if kind in ("scanner_journal", "call_artifact") else None
                if native_path is not None:
                    text(native_path, "native receipt path", 512)
                    relative = relative_name(native_path.removeprefix("/sdcard/"))
                    pattern = r"lakeshark/scan/[^/]+\.jsonl" if kind == "scanner_journal" else r"lakeshark/calls/[^/]+/[^/]+\.wav"
                    if not re.fullmatch(pattern, relative):
                        raise Invalid("unsupported native receipt artifact path")
                    try:
                        artifacts.append(self.artifact(self.root / relative, "explicit native receipt reference"))
                    except (Invalid, OSError) as exc:
                        self.issue(relative, str(exc))
                if len(self.records) >= self.limits["records"]:
                    raise Invalid("record limit; remaining receipt omitted")
                identity = dict(receipt_sha256=receipt_id, line=str(count))
                rec = dict(catalog_version=VERSION, id="receipt:" + digest(dumps(identity).encode()),
                           store="receipt", native_id=identity, native_scope="saved receipt; current boot scope only",
                           record_sha256=digest(line), locations=[dict(path=f"receipt:{receipt_id}", line=count)],
                           summary=f"device v{schema}: {kind}", relations=[],
                           relation_status="unknown; explicit artifact references only; no event association",
                           provenance=dict(board=None, firmware=None, radio=None, mode=None),
                           location=dict(valid=False, latitude=None, longitude=None),
                           time=dict(utc_unix=int(r["utc"]) if r.get("utc") else None,
                                     monotonic=(r.get("after") or {}).get("captured_us") if trial else
                                         r.get("uptime_us", r.get("at_us", r.get("started_us", r.get("last_tune_us")))),
                                     monotonic_unit="us", boot=None),
                           details=r, artifacts=artifacts)
                self.records.append(rec)
                if (kind == "error" or r.get("counter_saturated") or r.get("clock_invalid") or r.get("timing_invalid")
                        or r.get("detail_truncated") or r.get("overwritten_entries") not in (None, "0") or r.get("overwritten_intervals") not in (None, "0")):
                    self.issue(f"receipt:{receipt_id}", "device receipt reports error, saturation, invalid clock or overwritten/truncated detail", count)
            except (Invalid, ValueError) as exc:
                self.issue(f"receipt:{receipt_id}", str(exc), count)
        if not count:
            self.issue(f"receipt:{receipt_id}", "empty receipt")
        planned = [r["details"] for r in self.records if r["native_id"].get("receipt_sha256") == receipt_id
                   and r["details"].get("type") in ("rx_plan_session", "rx_plan_step")]
        if planned:
            sessions = [r for r in planned if r["type"] == "rx_plan_session"]
            steps = [r for r in planned if r["type"] == "rx_plan_step"]
            if (len(sessions) != 1 or len(steps) != int(sessions[0]["step_count"]) or
                    {int(r["step_index"]) for r in steps} != set(range(int(sessions[0]["step_count"]))) or
                    any(r["plan_id"] != sessions[0]["plan_id"] for r in steps)):
                self.issue(f"receipt:{receipt_id}", "RX plan steps incomplete, duplicated or outside the native session")
        coverage = [r["details"] for r in self.records if r["native_id"].get("receipt_sha256") == receipt_id
                    and r["details"].get("type") in ("coverage_session", "coverage_interval")]
        if coverage:
            sessions = [r for r in coverage if r["type"] == "coverage_session"]
            intervals = [r for r in coverage if r["type"] == "coverage_interval"]
            if (len(sessions) != 1 or len(intervals) != int(sessions[0]["retained_intervals"]) or
                    len({r["sequence"] for r in intervals}) != len(intervals) or
                    any(r["session_id"] != sessions[0]["session_id"] for r in intervals)):
                self.issue(f"receipt:{receipt_id}", "software coverage intervals incomplete, duplicated or outside native session")
            elif not sessions[0]["timing_invalid"]:
                header = sessions[0]
                cursor = int(header["started_us"])
                sequence = 0
                if not header["counter_saturated"]:
                    total = sum(int(header[key]) for key in ("unknown_us", "receiving_us", "settling_us", "busy_us", "unavailable_us"))
                    if total != int(header["last_update_us"])-cursor:
                        self.issue(f"receipt:{receipt_id}", "software coverage state totals disagree with elapsed software time")
                for interval in intervals:
                    start, end = int(interval["started_us"]), int(interval["ended_us"])
                    current_sequence = int(interval["sequence"])
                    if start < cursor or end > int(header["last_update_us"]) or current_sequence <= sequence:
                        self.issue(f"receipt:{receipt_id}", "software coverage intervals overlap, reverse, escape session time or have invalid sequence order")
                        break
                    cursor, sequence = end, current_sequence
        return self

    def json_lines(self, path, parser):
        self.safe(path)
        with path.open("rb") as stream:
            line = 0
            while not self.stopped:
                allowance = min(self.limits["line_bytes"] + 1,
                                self.limits["read_bytes"] - self.read_bytes)
                if allowance <= 0:
                    raise Invalid("total input byte limit; remaining file omitted")
                raw = stream.readline(allowance)
                if not raw:
                    break
                line += 1
                self.read_bytes += len(raw)
                if len(raw) > self.limits["line_bytes"]:
                    self.issue(self.relative(path), "line size limit; remaining file omitted", line)
                    break
                if not raw.endswith(b"\n"):
                    self.issue(self.relative(path), "unterminated/truncated line; remaining file omitted", line)
                    break
                try:
                    parser(path, raw, line)
                except (Invalid, ValueError, OverflowError) as exc:
                    self.issue(self.relative(path), str(exc), line)

    def call(self, path):
        raw = self.read(path, 2048)
        try:
            rows = list(csv.reader(raw.decode("utf-8").splitlines(), strict=True))
            if len(rows) != 1 or len(rows[0]) != 9:
                raise Invalid("call metadata must contain one nine-field CSV row")
            a = rows[0]
            values = [int(value) for value in a[:7]]
            stamp, duration, hz, tg, source, rate, gps = values
            for key, value in zip(("time", "duration", "frequency", "talkgroup", "source", "rate"), values[:6]):
                integer(value, key, maximum=(1 << 63)-1)
            if gps not in (0, 1) or not rate:
                raise Invalid("invalid call rate/GPS flag")
            loc = location(dict(gps_valid=bool(gps), latitude=float(a[7]) if gps else None,
                                longitude=float(a[8]) if gps else None))
        except (ValueError, UnicodeError, csv.Error) as exc:
            raise Invalid(f"invalid call metadata: {exc}") from exc
        wav = path.with_suffix("")
        artifacts = [self.artifact(path, "call metadata", raw)]
        # The call sidecar has no receiver/mode/build or shared event ID.
        # Keep missing audio visible as an issue; do not guess its contents.
        try:
            artifacts.append(self.artifact(wav, "audio WAV (content not verified)"))
        except Invalid as exc:
            self.issue(self.relative(wav), str(exc))
        rec = self.add("call", path, raw, {"wav_path": self.relative(wav)}, "card-relative WAV path",
                       f"{hz} Hz / TG {tg} / {duration} ms", time=dict(utc_unix=stamp or None,
                       monotonic=None, monotonic_unit=None, boot=None), details=dict(frequency_hz=hz,
                       talkgroup=tg, source=source, rate_hz=rate, duration_ms=duration), artifacts=artifacts)
        if rec:
            rec["location"] = loc

    def rec_capture(self, path):
        raw = self.read(path)
        try:
            lines = raw.decode("ascii").splitlines()
        except UnicodeError as exc:
            raise Invalid("non-ASCII REC capture") from exc
        if not lines or lines[0] not in ("Filetype: Flipper SubGhz RAW File", "Filetype: Flipper SubGhz Key File",
                                         "Filetype: LakeShark SubGhz IQ File"):
            raise Invalid("unrecognized REC filetype")
        headers = {}
        for line in lines:
            if line.startswith(("IQ_Data:", "RAW_Data:", "#")) or ": " not in line:
                continue
            key, value = line.split(": ", 1)
            if key in headers:
                raise Invalid(f"duplicate REC header {key}")
            headers[key] = value
        hz = integer(int(headers.get("Frequency", "0")), "Frequency", 1, 0xffffffff)
        protocol = text(headers.get("Protocol", ""), "Protocol", 48)
        artifacts = [self.artifact(path, "REC capture", raw)]
        side = path.with_suffix(".json")
        provenance = dict(board=None, firmware=None, radio=None, mode=protocol)
        details = dict(frequency_hz=hz, protocol=protocol, capture_time_text=None,
                       sigmf_status="not requested" if not self.sigmf else "unsupported pulse/key format")
        side_data = None
        try:
            side_raw = self.read(side)
            side_data = strict_json(side_raw)
            for key in ("board", "firmware", "radio"):
                if key in side_data:
                    provenance[key] = text(side_data[key], key, 128) or None
            details["capture_time_text"] = text(side_data.get("time", ""), "time", 64) or None
            artifacts.append(self.artifact(side, "REC sidecar", side_raw))
        except (Invalid, OSError) as exc:
            self.issue(self.relative(side), str(exc))
        iq = None
        if protocol == "LR2021_IQ":
            try:
                if not raw.endswith(b"\n"):
                    raise Invalid("unterminated native IQ capture")
                iq = self.native_iq(lines, headers, hz, side_data)
                details.update(sample_rate_hz=int(headers["SampleRate"]), samples=int(headers["Samples"]),
                               datatype="ci16_le", sigmf_status="eligible" if self.sigmf else "eligible; --sigmf not requested")
            except (Invalid, ValueError) as exc:
                details["sigmf_status"] = f"refused: {exc}"
                self.issue(self.relative(path), f"IQ export refused: {exc}")
        rec = self.add("rec", path, raw, {"capture_path": self.relative(path)}, "card-relative REC path",
                       f"{protocol}: {hz} Hz", time=dict(utc_unix=None, monotonic=None,
                       monotonic_unit=None, boot=None), details=details, artifacts=artifacts)
        if rec:
            rec["provenance"] = provenance
            if iq is not None and self.sigmf:
                self.converted[rec["id"]] = (iq, hz, int(headers["SampleRate"]))

    @staticmethod
    def native_iq(lines, headers, hz, side):
        # Only this versioned producer contract establishes signed LE I,Q.
        # See rec_lr_iq_save/load and iq_consume; external IQ is not guessed.
        if headers.get("Filetype") != "LakeShark SubGhz IQ File" or headers.get("Version") != "1":
            raise Invalid("unsupported native IQ producer/version")
        rate = int(headers.get("SampleRate", "0"))
        samples = int(headers.get("Samples", "0"))
        if rate != 125000 or not 1 <= samples <= 32767 or not 104000000 <= hz <= 2740000000:
            raise Invalid("unsupported native IQ rate/count/frequency")
        if not side or side.get("radio") != "LR2021" or type(side.get("freq_hz")) is not int or side["freq_hz"] != hz or type(side.get("sample_rate")) is not int or side["sample_rate"] != rate:
            raise Invalid("missing or mismatched LR2021 sidecar")
        data = bytearray()
        for line in lines[6:]:
            if not re.fullmatch(r"IQ_Data: (?:[0-9A-Fa-f]{8}){1,16}", line):
                raise Invalid("invalid native IQ data line")
            data.extend(bytes.fromhex(line[9:]))
            if len(data) > samples * 4:
                raise Invalid("native IQ sample count overflow")
        expected = ["Filetype: LakeShark SubGhz IQ File", "Version: 1", f"Frequency: {hz}",
                    "Protocol: LR2021_IQ", f"SampleRate: {rate}", f"Samples: {samples}"]
        if lines[:6] != expected or len(data) != samples * 4:
            raise Invalid("truncated or noncanonical native IQ capture")
        return bytes(data)

    def run(self):
        # Scan only stores with defined adapters; no recursive walk over music/maps.
        roots = [(self.root / "lakeshark/scan", "scanner", "*.jsonl"),
                 (self.root / "detector/events", "detector", "*.jsonl"),
                 (self.root / "lakeshark/calls", "call", "*.wav.meta"),
                 (self.root / "lakeshark", "rec", "*.sub"),
                 (self.root / "subghz", "rec", "*.sub"),
                 (self.root / "lakeshark/rec", "rec", "*.sub")]
        for directory, store, pattern in roots:
            if self.stopped:
                break
            if not directory.exists():
                continue
            # Calls alone have one directory level (YYYYMMDD).
            directories = [directory]
            index = 0
            while index < len(directories) and not self.stopped:
                current = directories[index]
                index += 1
                try:
                    if current.is_symlink() or (hasattr(current, "is_junction") and current.is_junction()):
                        raise Invalid("symlink/junction directory refused")
                    if not current.resolve().is_relative_to(self.root):
                        raise Invalid("directory escapes card root")
                    # Bound enumeration before sorting (huge directories stay bounded).
                    children = []
                    with os.scandir(current) as iterator:
                        for child in iterator:
                            self.entries += 1
                            if self.entries > self.limits["entries"]:
                                self.issue(self.relative(current), "directory entry limit; remaining input omitted")
                                self.stopped = True
                                break
                            children.append(Path(child.path))
                    for path in sorted(children):
                        if self.stopped:
                            break
                        if store == "call" and current == directory and path.is_dir():
                            directories.append(path)
                            continue
                        if not path.match(pattern):
                            continue
                        self.files += 1
                        if self.files > self.limits["files"]:
                            self.issue(self.relative(path), "file limit; remaining input omitted")
                            self.stopped = True
                            break
                        try:
                            before = self.safe(path).stat()
                            if store in ("scanner", "detector"):
                                self.json_lines(path, self.scan_record if store == "scanner" else self.detector_record)
                            elif store == "call":
                                self.call(path)
                            else:
                                self.rec_capture(path)
                            after = path.stat()
                            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                                self.issue(self.relative(path), "source changed while reading; use a quiescent card copy")
                        except (Invalid, OSError, ValueError) as exc:
                            self.issue(self.relative(path), str(exc))
                except (Invalid, OSError) as exc:
                    self.issue(self.relative(current), str(exc))
        return self

    def summary(self):
        return dict(catalog_version=VERSION, source_root=str(self.root), records=len(self.records),
                    source_files_considered=self.files, directory_entries=self.entries,
                    metadata_bytes_read=self.read_bytes, issue_count=self.issue_count,
                    issues_omitted=max(0, self.issue_count-len(self.issues)),
                    partial=bool(self.issue_count), limits=self.limits, issues=self.issues,
                    relationships="unknown; no timestamp joins", coverage="not measured",
                    artifact_integrity="record/metadata hashes only; un-hashed large artifacts are unverified")


def dumps(value):
    return json.dumps(value, ensure_ascii=True, allow_nan=False, sort_keys=True)


def capture_settings(record):
    details = record["details"]
    if details.get("type") == "receiver_trial":
        details = details.get("after") or details.get("before") or {}
    return {key: details.get(key) for key in
            ("frequency_hz", "requested_hz", "effective_hz", "rate_hz", "sample_rate_hz", "protocol",
             "configured_center_hz", "nominal_rate_hz", "bandwidth_hz", "gain_mode",
             "requested_gain_tenths_db", "effective_gain_tenths_db", "source", "endpoint", "mode",
             "planned_center_hz", "planned_dwell_ms", "plan_id", "plan_step", "plan_repeat")}


def copy_bundle_sources(catalog, stage):
    sources = {name: set() for name in catalog.source_stamps}
    sources.update({f"receipt:{key}": set() for key in catalog.receipts})
    for rec in catalog.records:
        for artifact in rec["artifacts"]:
            sources.setdefault(artifact["path"], set()).add(rec["id"])
    entries, omitted, total = [], [], 0
    (stage / "sources/card").mkdir(parents=True)
    receipt_order = {key: index for index, key in enumerate(catalog.receipts)}
    order = lambda item: (1, receipt_order[item[0][8:]]) if item[0].startswith("receipt:") else (0, item[0])
    for name, record_ids in sorted(sources.items(), key=order):
        try:
            if len(entries) >= BUNDLE_LIMITS["files"]:
                raise Invalid("bundle source file count limit")
            receipt = catalog.receipts.get(name.removeprefix("receipt:")) if name.startswith("receipt:") else None
            if receipt:
                member = "sources/receipts/" + name[8:] + ".jsonl"
                source = receipt["path"]
            else:
                relative_name(name)
                member = "sources/card/" + name
                source = catalog.safe(catalog.root / name)
                stat = source.stat()
                if catalog.source_stamps[name] != (stat.st_size, stat.st_mtime_ns):
                    raise Invalid("source changed since catalog read")
            target = stage / member
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                with target.open("xb") as stream:
                    size, sha = stream_hash(source, min(BUNDLE_LIMITS["file_bytes"], BUNDLE_LIMITS["total_bytes"] - total), stream)
                if not receipt and catalog.allowed_sources is not None and sha != catalog.allowed_sources[name]:
                    raise Invalid("source SHA-256 changed after bundle verification")
                if receipt and (sha != digest(receipt["raw"]) or target.read_bytes() != receipt["raw"]):
                    raise Invalid("receipt changed since import")
            except Exception:
                target.unlink(missing_ok=True)
                raise
            total += size
            entries.append(dict(bundle_path=member, source_identity=name, bytes=size, sha256=sha,
                                role="receipt" if receipt else "card", record_ids=sorted(record_ids)))
            catalog.bundle_links[name] = member
        except (Invalid, OSError) as exc:
            catalog.issue(name, f"bundle source omitted: {exc}")
            omitted.append(dict(source_identity=name, reason=str(exc)))
    return dict(bundle_format="lakeshark-capture-bundle", version=1, files=entries,
                omitted_sources=omitted, source_root_hint=str(catalog.root),
                source_identities=[dict(id=r["id"], native_id=r["native_id"], native_scope=r["native_scope"],
                                        provenance=r["provenance"],
                                        capture_settings=capture_settings(r)) for r in catalog.records],
                build=dict(host_tool_sha256=digest(Path(__file__).read_bytes()),
                           firmware="only recorded source provenance; otherwise unknown"),
                settings=dict(sigmf_requested=catalog.sigmf, catalog_limits=catalog.limits,
                              bundle_limits=BUNDLE_LIMITS),
                known_unknowns=["RF coverage", "sample continuity", "sample UTC/calibration",
                                "missing source build/settings", "cross-store relationships"],
                analysis="native LR2021 IQ to SigMF only; no decoder replay; existing P25 replay requires U8 240000 Hz",
                integrity="SHA-256 byte integrity relative to this manifest; not RF truth, authenticity or decoder correctness")


def verify_bundle(root):
    root = Path(root).absolute()
    # Validate the manifest's path before any read, including its parent chain.
    manifest_path = safe_member(root, "bundle.json")
    if manifest_path.stat().st_size > 4 * 1024 * 1024:
        raise Invalid("bundle manifest size limit")
    with manifest_path.open("rb") as stream:
        raw = stream.read(4 * 1024 * 1024 + 1)
    if len(raw) > 4 * 1024 * 1024:
        raise Invalid("bundle manifest size limit")
    manifest = strict_json(raw)
    if manifest.get("bundle_format") != "lakeshark-capture-bundle" or type(manifest.get("version")) is not int or manifest["version"] != 1:
        raise Invalid("unsupported bundle format/version")
    boolean(manifest.get("partial"), "bundle partial flag")
    entries = manifest.get("files")
    if not isinstance(entries, list) or len(entries) > 3 * BUNDLE_LIMITS["files"] + 4:
        raise Invalid("bundle member count limit")
    # Two passes: no member is read until every supplied path and bound is checked.
    checked, seen, declared = [], set(), 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise Invalid("invalid bundle member")
        name = relative_name(entry.get("bundle_path"))
        if name in seen or name == "bundle.json":
            raise Invalid("duplicate/reserved bundle member")
        seen.add(name)
        size = integer(entry.get("bytes"), "bundle bytes", maximum=BUNDLE_LIMITS["total_bytes"])
        sha = text(entry.get("sha256"), "bundle sha256", 64)
        if not re.fullmatch("[0-9a-f]{64}", sha):
            raise Invalid("invalid bundle SHA-256")
        role = entry.get("role")
        if role == "card":
            if size > BUNDLE_LIMITS["file_bytes"]:
                raise Invalid("bundle source file size limit")
            source = relative_name(entry.get("source_identity"))
            if name != "sources/card/" + source:
                raise Invalid("card source identity/path mismatch")
        elif role == "receipt":
            if size > BUNDLE_LIMITS["receipt_bytes"]:
                raise Invalid("bundle receipt size limit")
            source = text(entry.get("source_identity"), "receipt identity", 72)
            if not re.fullmatch("receipt:[0-9a-f]{64}", source) or name != "sources/receipts/" + source[8:] + ".jsonl" or sha != source[8:]:
                raise Invalid("receipt identity/path/hash mismatch")
        elif role != "generated" or not re.fullmatch(r"(?:catalog\.jsonl|summary\.json|index\.html|lr2021-[0-9a-f]{64}\.sigmf-(?:data|meta))", name):
            raise Invalid("unsupported bundle member role/path")
        declared += size
        if declared > BUNDLE_LIMITS["total_bytes"] + 32 * 1024 * 1024:
            raise Invalid("bundle total byte limit")
        checked.append((entry, safe_member(root, name)))
    if not {"catalog.jsonl", "summary.json", "index.html"}.issubset(seen):
        raise Invalid("bundle is missing generated catalog/view members")
    failures, verified = [], 0
    for entry, path in checked:
        try:
            size, sha = stream_hash(path, entry["bytes"])
            if size != entry["bytes"] or sha != entry["sha256"]:
                raise Invalid("byte size/SHA-256 mismatch")
            verified += 1
        except (Invalid, OSError) as exc:
            failures.append(dict(path=entry["bundle_path"], reason=str(exc)))
    return manifest, dict(verified_files=verified, failures=failures,
                          byte_integrity_ok=not failures, source_partial=manifest.get("partial") is not False,
                          rf_coverage="unknown; byte integrity is not RF evidence")


def reindex_bundle(root, output, sigmf=False):
    manifest, report = verify_bundle(root)
    if not report["byte_integrity_ok"]:
        raise Invalid("bundle integrity check failed; refusing reindex")
    root = Path(root).absolute()
    if Path(output).absolute().parent.resolve(strict=True).joinpath(Path(output).name).is_relative_to(root.resolve()):
        raise Invalid("reindex output must be outside the source bundle")
    allowed = {e["source_identity"]: e["sha256"] for e in manifest["files"] if e["role"] == "card"}
    catalog = Catalog(safe_member(root, "sources/card"), sigmf=sigmf, allowed_sources=allowed).run()
    for entry in manifest["files"]:
        if entry["role"] == "receipt":
            catalog.import_receipt(safe_member(root, entry["bundle_path"]))
    if report["source_partial"]:
        catalog.issue("bundle.json", "source bundle was partial; omitted input cannot be recovered")
    return catalog, write_export(catalog, output, bundle=True)


def write_export(catalog, output, bundle=False):
    output = Path(output).absolute()
    if output.exists() or output.is_symlink():
        raise Invalid("output must be a new directory")
    parent = output.parent.resolve(strict=True)
    output = parent / output.name
    if output.is_relative_to(catalog.root):
        raise Invalid("output must be outside the read-only card root")
    stage = Path(tempfile.mkdtemp(prefix=".catalog-", dir=parent))
    try:
        if bundle:
            catalog.bundle_manifest = copy_bundle_sources(catalog, stage)
        for rec in catalog.records:
            if rec["id"] not in catalog.converted:
                continue
            data, hz, rate = catalog.converted[rec["id"]]
            name = "lr2021-" + rec["id"].split(":")[1]
            (stage / f"{name}.sigmf-data").write_bytes(data)
            meta = {"global": {"core:datatype": "ci16_le", "core:sample_rate": rate,
                    "core:version": "1.2.6", "core:recorder": "LakeShark observation_catalog v1",
                    "core:sha512": hashlib.sha512(data).hexdigest(),
                    "core:description": f"Byte-preserving native LR2021_IQ v1 conversion; source {rec['locations'][0]['path']}; catalog {rec['id']}. Sample rate is nominal, not ppm calibrated. Sample UTC, amplitude calibration and continuity evidence unavailable."},
                    "captures": [{"core:sample_start": 0, "core:frequency": hz}], "annotations": []}
            (stage / f"{name}.sigmf-meta").write_text(dumps(meta) + "\n", encoding="utf-8")
            rec["details"]["sigmf_status"] = "exported; sample UTC/calibration/continuity unknown"
            rec["exports"] = [f"{name}.sigmf-meta", f"{name}.sigmf-data"]
        with (stage / "catalog.jsonl").open("w", encoding="utf-8", newline="\n") as stream:
            for record in catalog.records:
                stream.write(dumps(record) + "\n")
        summary = catalog.summary()
        summary["sigmf_exports"] = len(catalog.converted)
        summary["bundle"] = bool(bundle)
        (stage / "summary.json").write_text(dumps(summary) + "\n", encoding="utf-8")
        (stage / "index.html").write_text(render_html(catalog, summary, output), encoding="utf-8")
        if bundle:
            manifest = catalog.bundle_manifest
            declared = sum(entry["bytes"] for entry in manifest["files"])
            for path in sorted(stage.iterdir()):
                if path.is_file():
                    size, sha = stream_hash(path, 32 * 1024 * 1024)
                    declared += size
                    if declared > BUNDLE_LIMITS["total_bytes"] + 32 * 1024 * 1024:
                        raise Invalid("bundle copied/generated total byte limit")
                    manifest["files"].append(dict(bundle_path=path.name, role="generated", bytes=size, sha256=sha))
            manifest["partial"] = bool(catalog.issue_count)
            manifest_raw = dumps(manifest) + "\n"
            if len(manifest_raw.encode()) > 4 * 1024 * 1024:
                raise Invalid("bundle manifest size limit")
            (stage / "bundle.json").write_text(manifest_raw, encoding="utf-8")
        if output.exists():
            raise Invalid("output appeared during export; refusing overwrite")
        stage.rename(output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return output


def receipt_groups(catalog, types):
    groups = {}
    for record in catalog.records:
        if record["store"] == "receipt" and record["details"].get("type") in types:
            groups.setdefault(record["native_id"]["receipt_sha256"], []).append(record["details"])
    return groups


def render_rx_plans(catalog):
    esc = lambda value: html.escape("unknown" if value is None else str(value), quote=True)
    groups = receipt_groups(catalog, {"rx_plan_session", "rx_plan_step"})
    sections = []
    for receipt_id, records in groups.items():
        sessions = [r for r in records if r["type"] == "rx_plan_session"]
        header = sessions[0] if len(sessions) == 1 else {}
        partial = not header or any(i["path"] == f"receipt:{receipt_id}" for i in catalog.issues)
        rows = []
        for step in (r for r in records if r["type"] == "rx_plan_step"):
            rows.append(f'<tr><td>{esc(step["step_index"])}</td><td>{esc(step["planned_center_hz"])} Hz<br>{esc(step["planned_dwell_ms"])} ms planned</td>'
                        f'<td>{esc(step["attempts"])} attempts / {esc(step["completed"])} completed</td>'
                        f'<td>{esc(step["first_tune_us"])} / {esc(step["last_tune_us"])}</td>'
                        f'<td>{esc(step["last_dwell_started_us"])} / {esc(step["last_dwell_ended_us"])}</td>'
                        f'<td>{esc(step["delivered_u8_iq_bytes"])} bytes / {esc(step["delivered_blocks"])} blocks<br>{esc(step["software_read_dsp_window_us"])} µs summed read/DSP windows<br>{esc(step["nominal_sample_duration_us"])} µs nominal samples at {esc(step["nominal_rate_hz"])} pairs/s<br>first/last delivery: {esc(step["first_delivery_us"])} / {esc(step["last_delivery_us"])}<br>timing invalid: {esc(step["timing_invalid"])}; saturated: {esc(step["counter_saturated"])}; detailed trace truncated: {esc(step["detail_truncated"])}; per-step loss unknown: {esc(step["loss_unknown"])}</td></tr>')
        sections.append(f'<section class="receipt-panel"><h3>{esc(header.get("name", "Incomplete plan"))}: {esc(header.get("state"))}</h3>'
                        f'<p>Native plan {esc(header.get("plan_id"))}; receipt <code>{esc(receipt_id)}</code>. '
                        f'Reason: {esc(header.get("reason"))}. Current step/repeat: {esc(header.get("current_step"))}/{esc(header.get("current_repeat"))}; repeats planned: {esc(header.get("repeat_count"))}.</p>'
                        f'{"<p class=warning>Partial receipt or invalid clock: detailed timing history is limited. Independent per-step delivery aggregates remain as reported.</p>" if partial or header.get("clock_invalid") else ""}'
                        f'<p>Actual control timepoints are boot-relative microseconds. Planned dwell and completed control phases earn no observation time. Recording policy: {esc(header.get("recording_policy"))}; calls suppressed: {esc(header.get("calls_suppressed"))}; restoration attempted/restored: {esc(header.get("restore_attempted"))}/{esc(header.get("restored"))}.</p>'
                        '<p>Per-step aggregates survive recent interval-ring overwrite. Summed read/DSP windows count qualified software work only; gaps and whole planned dwells are never painted as observed. Nominal sample duration remains separate.</p>'
                        '<table><thead><tr><th>Step (zero-based)</th><th>Planned tuning / dwell</th><th>Actual progress</th><th>First / last tune, µs</th><th>Last dwell start / end, µs</th><th>Independent actual delivery aggregates</th></tr></thead>'
                        f'<tbody>{"".join(rows)}</tbody></table></section>')
    return '<h2>Receive plans: planned versus actual control</h2>' + "".join(sections) if sections else ""


def render_software_coverage(catalog):
    esc = lambda value: html.escape("unknown" if value is None else str(value), quote=True)
    groups = receipt_groups(catalog, {"coverage_session", "coverage_interval"})
    sections = []
    for receipt_id, records in groups.items():
        sessions = [r for r in records if r["type"] == "coverage_session"]
        header = sessions[0] if len(sessions) == 1 else {}
        intervals = [r for r in records if r["type"] == "coverage_interval"]
        partial = not header or any(i["path"] == f"receipt:{receipt_id}" for i in catalog.issues)
        timing = bool(header) and not header.get("timing_invalid") and not any(
            i["path"] == f"receipt:{receipt_id}" and any(token in i["reason"] for token in ("totals disagree", "invalid sequence", "overlap"))
            for i in catalog.issues)
        bars, rows = [], []
        start = int(header["started_us"]) if header else 0
        end = int(header["last_update_us"]) if header else 0
        span, cursor = max(0, end-start), start
        def bar(state, begin, finish, label):
            if span and finish > begin:
                width = (finish-begin)*100/span
                bars.append(f'<span class="coverage-{state}" style="width:{width:.6f}%" title="{esc(label)}" aria-label="{esc(label)}"></span>')
        for interval in intervals:
            begin, finish = int(interval["started_us"]), int(interval["ended_us"])
            if timing and begin >= cursor and finish <= end:
                if begin > cursor:
                    bar("unreported", cursor, begin, f"Unreported interval: unknown {cursor}..{begin} us")
                bar(interval["state"], begin, finish, f"Software {interval['state']}: {begin}..{finish} us")
                cursor = finish
            elif timing:
                timing = False
            duration = str(finish-begin) + " µs software interval" if timing else "duration unavailable: clock/sequence invalid"
            binding = (f'plan {esc(interval["plan_id"])} / step {esc(interval["plan_step"])} / repeat {esc(interval["plan_repeat"])}'
                       if interval["plan_id"] is not None else "unbound")
            rows.append(f'<tr><td>{esc(interval["sequence"])}<br>{binding}</td>'
                        f'<td>{esc(interval["state"])}<br>{esc(interval["started_us"])} → {esc(interval["ended_us"])}<br>{esc(duration)}</td>'
                        f'<td>{esc(interval["configured_center_hz"])} Hz / {esc(interval["nominal_rate_hz"])} pairs/s<br>context known: {esc(interval["context_known"])}<br>FM auto-CALLS suppressed: {esc(interval["calls_suppressed"])}</td>'
                        f'<td>{esc(interval["delivered_bytes"])} bytes / {esc(interval["delivered_blocks"])} blocks<br>{esc(interval["nominal_sample_duration_us"])} µs nominal samples<br>{esc(interval["settling_discarded_bytes"])} settling bytes discarded</td></tr>')
        if timing and cursor < end:
            bar("unreported", cursor, end, f"Unreported interval: unknown {cursor}..{end} us")
        totals = "".join(f'<tr><th>{state}</th><td>{esc(header.get(state+"_us"))} µs software state</td></tr>'
                         for state in ("unknown", "receiving", "settling", "busy", "unavailable"))
        losses = " / ".join(f'{esc(key)}={esc(header.get(key))}' for key in
                            ("ring_rejected_bytes", "transfer_error_events", "submission_error_events", "flush_discarded_bytes", "provenance_rejected_blocks"))
        chart = ('<div class="coverage-timeline" role="img" aria-label="Retained software intervals; unreported spans are unknown">'+"".join(bars)+'</div>'
                 if timing and span else '<p class="warning">No valid elapsed timeline available; raw native endpoints remain below.</p>')
        sections.append(f'<section class="receipt-panel"><h3>Software observation session {esc(header.get("session_id"))}</h3>'
                        f'<p>Receipt <code>{esc(receipt_id)}</code>; boot-relative µs {esc(header.get("started_us"))} → {esc(header.get("last_update_us"))}. '
                        f'Active: {esc(header.get("active"))}; stopped: {esc(header.get("stopped_us"))}; timing invalid: {esc(header.get("timing_invalid"))}; counters saturated: {esc(header.get("counter_saturated"))}; current FM auto-CALLS suppressed: {esc(header.get("calls_suppressed"))}.</p>'
                        f'{"<p class=warning>Partial or limited receipt: retained intervals cannot show complete session history. Review truncation, overwrites, clock and input issues.</p>" if partial else ""}'
                        f'<p>Retained intervals: {esc(header.get("retained_intervals"))}; overwritten: {esc(header.get("overwritten_intervals"))}. Aggregate state totals cover the native session accounting, including intervals no longer retained.</p>'
                        f'{chart}<p class="coverage-legend">Software states: receiving (read through DSP completion), settling, busy, unavailable, unknown. Dashed gaps are unreported and unknown; time after last_update_us is unobserved.</p>'
                        f'<table><tbody>{totals}</tbody></table>'
                        f'<p>Delivered samples: {esc(header.get("delivered_bytes"))} bytes / {esc(header.get("delivered_blocks"))} blocks. Settling discards: {esc(header.get("settling_discarded_bytes"))} bytes. Guard: {esc(header.get("settling_guard_us"))} µs and {esc(header.get("settling_guard_blocks"))} blocks; software policy, not calibrated tuner settling.</p>'
                        f'<p>Observed transport-counter deltas only: {losses}. First/new counter epochs establish a baseline. Hardware loss remains unknown; error events do not establish lost-byte counts.</p>'
                        '<p>Receiving windows are software observations. Nominal sample duration is separate and never converted into wall-time coverage. Physical RF continuity, per-sample frequency and RF absence remain unknown. Native plan bindings are shown as emitted; separate receipts are not joined.</p>'
                        '<table><thead><tr><th>Sequence / native binding (zero-based step/repeat)</th><th>Actual software window, µs</th><th>Configured receiver context</th><th>Delivered / nominal sample accounting</th></tr></thead>'
                        f'<tbody>{"".join(rows)}</tbody></table></section>')
    return '<h2>Measured software observation intervals</h2>'+"".join(sections) if sections else ""


def render_html(catalog, summary, output):
    esc = lambda value: html.escape(str(value), quote=True)
    rows = []
    for rec in catalog.records:
        links = []
        for artifact in rec["artifacts"]:
            name = artifact["path"]
            if summary.get("bundle"):
                member = catalog.bundle_links.get(name)
                if member is None:
                    links.append(f'{esc(artifact["role"])}: {esc(name)} (omitted; see issues)')
                    continue
                href = quote(member)
            elif name.startswith("receipt:"):
                href = catalog.receipts[name[8:]]["path"].as_uri()
            else:
                href = (catalog.root / name).as_uri()
            # Paths are trusted only as previously checked references. No HTML/script insertion.
            links.append(f'<a href="{esc(href)}">{esc(artifact["role"])}: {esc(name)}</a>')
        for name in rec.get("exports", []):
            links.append(f'<a href="{quote(name)}">{esc(name)}</a>')
        utc = rec["time"]["utc_unix"]
        stamp = str(utc) + " Unix UTC" if utc is not None else "UTC unknown"
        details = esc(json.dumps(rec, indent=2, ensure_ascii=True, allow_nan=False))
        search = esc(dumps(rec).lower())
        rows.append(f'<tr data-store="{esc(rec["store"])}" data-search="{search}"><td>{esc(rec["store"])}</td>'
                    f'<td>{esc(rec["summary"])}<br><small>{esc(stamp)}</small><details><summary>Record / provenance</summary><pre>{details}</pre></details></td>'
                    f'<td>{"<br>".join(links)}</td></tr>')
    issues = "".join(f'<li>{esc(i["path"])}{":"+str(i["line"]) if i["line"] else ""}: {esc(i["reason"])}</li>' for i in catalog.issues)
    return f'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>LakeShark observation catalog</title><style>
body{{font:16px system-ui;max-width:1300px;margin:2em auto;padding:0 1em;background:#101820;color:#e7eff2}}a{{color:#74e3d3}}table{{width:100%;border-collapse:collapse}}td,th{{padding:1em;border:1px solid #49616d;text-align:left;vertical-align:top}}pre{{white-space:pre-wrap;overflow-wrap:anywhere;max-width:70ch}}small{{color:#b9cdd4}}input,select{{font:inherit;margin:.5em;padding:.4em}}[hidden]{{display:none}}li{{overflow-wrap:anywhere}}td{{overflow-wrap:anywhere}}.receipt-panel{{border:1px solid #49616d;padding:1em;margin:1em 0;overflow:auto}}.warning{{color:#ffd394;font-weight:bold}}.coverage-timeline{{display:flex;height:2em;background:#2e3941;border:1px solid #b9cdd4;overflow:hidden}}.coverage-timeline span{{display:block;flex-shrink:0}}.coverage-receiving{{background:#4fbfa6}}.coverage-settling{{background:#d5ad55}}.coverage-busy{{background:#b37bc9}}.coverage-unavailable{{background:#d96f78}}.coverage-unknown{{background:#647580}}.coverage-unreported{{background:repeating-linear-gradient(45deg,#27333b,#27333b 4px,#82919b 4px,#82919b 6px)}}
</style><h1>LakeShark observation catalog</h1>
<p>Host-side export from {esc(catalog.root)}. {"Bundled links refer to copied sources; omitted members are listed below. Verify bundle.json with the CLI before re-analysis. Hashes establish bytes relative to the manifest, not RF truth or authenticity." if summary.get('bundle') else "Source artifacts remain on the card/copy; links need that location to remain available."}</p>
<p><strong>{summary['records']} records; {summary['issue_count']} issues; {summary['sigmf_exports']} SigMF exports.</strong> {"Partial catalog: review issues below." if summary['partial'] else "No input issues detected within the selected store adapters."}</p>
<p>Cross-store relationships are unknown. Physical RF coverage and sample continuity are not established. Software observation intervals require explicit schema 4 receipts. Activity is not a valid decode. Missing time/location/build remain unknown. Metadata hashes do not verify un-hashed audio contents.</p>
<p><a href="catalog.jsonl">JSONL catalog</a> · <a href="summary.json">Limits and issue report</a></p>
{'<p><a href="bundle.json">Source identities, build/settings, copied member hashes and known unknowns</a>. Verify using <code>python observation_catalog.py --verify-bundle &lt;this-directory&gt;</code>. Reindex using <code>--reindex-bundle &lt;this-directory&gt; --output &lt;new-directory&gt; [--sigmf]</code>. Reindex reruns the catalog adapters and optional native IQ conversion; decoder replay is unavailable for LR2021 captures.</p>' if summary.get('bundle') else ''}
{render_rx_plans(catalog)}
{render_software_coverage(catalog)}
<label>Search <input id="search" type="search" placeholder="frequency, channel, native ID, build…"></label>
<label>Store <select id="store"><option value="">All</option><option>scanner</option><option>detector</option><option>call</option><option>rec</option><option>receipt</option></select></label><p id="count"></p>
<table><thead><tr><th>Store</th><th>Observation</th><th>Artifacts</th></tr></thead><tbody>{''.join(rows)}</tbody></table>
<h2>Input issues</h2><ul>{issues or '<li>None detected.</li>'}</ul><p>{summary['issues_omitted']} additional issues omitted by the report bound. Limits and adapter scope are recorded in summary.json.</p>
<script>
const search=document.getElementById('search'),store=document.getElementById('store'),rows=[...document.querySelectorAll('tr[data-store]')];
function filter(){{let count=0;const q=search.value.toLowerCase();for(const row of rows){{row.hidden=!!((store.value&&row.dataset.store!==store.value)||!row.dataset.search.includes(q));if(!row.hidden)count++;}}document.getElementById('count').textContent=count+' of '+rows.length+' records shown';}}
search.addEventListener('input',filter);store.addEventListener('change',filter);filter();
</script></html>'''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("card_root", nargs="?", type=Path, help="quiescent SD root or a local copy")
    parser.add_argument("--output", type=Path, help="new directory outside card root")
    parser.add_argument("--sigmf", action="store_true", help="convert only supported native LR2021_IQ v1 captures")
    parser.add_argument("--receipt", action="append", default=[], type=Path, help="saved observations/mission/trial/plan/software-coverage JSONL (up to 8)")
    parser.add_argument("--bundle", action="store_true", help="opt in to bounded source copy/hash and portable manifest")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--verify-bundle", type=Path, help="verify member bytes against bundle.json; no RF claim")
    modes.add_argument("--reindex-bundle", type=Path, help="verify, reindex and optionally convert native IQ into a new bundle")
    args = parser.parse_args(argv)
    try:
        if len(args.receipt) > 8:
            raise Invalid("receipt input count limit (8)")
        if args.verify_bundle or args.reindex_bundle:
            if args.card_root or args.receipt or args.bundle:
                raise Invalid("bundle verification/reindex does not accept card_root, --receipt or --bundle")
            if args.verify_bundle:
                if args.output or args.sigmf:
                    raise Invalid("verification does not accept --output or --sigmf")
                _, report = verify_bundle(args.verify_bundle)
                print(dumps(report))
                return 1 if report["failures"] else 2 if report["source_partial"] else 0
            if args.output is None:
                raise Invalid("--output is required for reindex")
            catalog, output = reindex_bundle(args.reindex_bundle, args.output, sigmf=args.sigmf)
        else:
            if args.card_root is None or args.output is None:
                raise Invalid("card_root and --output are required for export")
            catalog = Catalog(args.card_root, sigmf=args.sigmf).run()
            for receipt in args.receipt:
                catalog.import_receipt(receipt)
            output = write_export(catalog, args.output, bundle=args.bundle)
    except (Invalid, OSError) as exc:
        parser.exit(1, f"catalog: {exc}\n")
    print(f"{len(catalog.records)} records; {catalog.issue_count} issues; open {output / 'index.html'}")
    return 2 if catalog.issue_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
