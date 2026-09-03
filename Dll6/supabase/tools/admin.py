#!/usr/bin/env python3
"""Minimal Axiom license/release administrator. Secrets are read from env only."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path


ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
ACTIVATION_PATTERN = re.compile(r"^AXF-(?:[A-HJ-NP-Z2-9]{5}-){3}[A-HJ-NP-Z2-9]{5}$")


def setting(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"Missing environment variable: {name}")
    return value


def request(method: str, path: str, payload: bytes | None = None,
            content_type: str = "application/json",
            extra_headers: dict[str, str] | None = None) -> object:
    base = setting("SUPABASE_URL").rstrip("/")
    service_key = setting("SUPABASE_SERVICE_ROLE_KEY")
    headers = {
        "Authorization": f"Bearer {service_key}",
        "apikey": service_key,
        "Content-Type": content_type,
        "Prefer": "return=representation",
    }
    headers.update(extra_headers or {})
    req = urllib.request.Request(base + path, data=payload, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            body = response.read()
            return json.loads(body) if body else None
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", "replace")
        raise SystemExit(f"Supabase returned HTTP {error.code}: {detail}") from error


def normalize_key(value: str) -> str:
    result = value.strip().upper()
    if (len(result) < 12 or len(result) > 96 or
            any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-" for c in result)):
        raise SystemExit("Invalid license key format")
    return result


def key_digest(value: str) -> str:
    return hmac.new(setting("LICENSE_PEPPER").encode(), normalize_key(value).encode(),
                    hashlib.sha256).hexdigest()


def generated_key() -> str:
    return "AXM-" + "-".join(
        "".join(secrets.choice(ALPHABET) for _ in range(5)) for _ in range(4)
    )


def generated_activation_code() -> str:
    return "AXF-" + "-".join(
        "".join(secrets.choice(ALPHABET) for _ in range(5)) for _ in range(4)
    )


def activation_digest(value: str) -> str:
    normalized = value.strip().upper()
    if not ACTIVATION_PATTERN.fullmatch(normalized):
        raise SystemExit("Invalid FunPay activation code format")
    pepper = setting("ACTIVATION_CODE_PEPPER")
    if len(pepper) < 32:
        raise SystemExit("ACTIVATION_CODE_PEPPER must contain at least 32 characters")
    return hmac.new(pepper.encode(), normalized.encode(), hashlib.sha256).hexdigest()


def generate_activation_codes(args: argparse.Namespace) -> None:
    if not 1 <= args.count <= 10000:
        raise SystemExit("--count must be between 1 and 10000")
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    codes = list(dict.fromkeys(generated_activation_code() for _ in range(args.count)))
    while len(codes) < args.count:
        candidate = generated_activation_code()
        if candidate not in codes:
            codes.append(candidate)
    batch_id = str(uuid.uuid4())
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(output, flags, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        stream.write("\n".join(codes) + "\n")
    records = [{
        "code_hash": activation_digest(code),
        "plan_code": args.plan,
        "batch_id": batch_id,
    } for code in codes]
    request("POST", "/rest/v1/axiom_activation_codes", json.dumps(records).encode())
    print(f"created batch {batch_id}: {len(codes)} codes for {args.plan}")
    print(f"FunPay inventory file: {output}")


def create_license(args: argparse.Namespace) -> None:
    key = normalize_key(args.key or generated_key())
    record = {
        "key_hash": key_digest(key), "label": args.label,
        "max_devices": args.max_devices,
        "expires_at": args.expires_at or None,
    }
    request("POST", "/rest/v1/axiom_licenses", json.dumps(record).encode())
    print(key)


def revoke_license(args: argparse.Namespace) -> None:
    digest = urllib.parse.quote(key_digest(args.key), safe="")
    result = request("PATCH", f"/rest/v1/axiom_licenses?key_hash=eq.{digest}",
                     b'{"enabled":false}')
    if not result:
        raise SystemExit("License was not found")
    print("revoked")


def list_licenses(_args: argparse.Namespace) -> None:
    rows = request(
        "GET",
        "/rest/v1/axiom_licenses?select=id,label,enabled,expires_at,max_devices,created_at"
        "&order=created_at.desc",
    )
    print(json.dumps(rows, ensure_ascii=False, indent=2))


def publish(args: argparse.Namespace) -> None:
    module = Path(args.module).resolve()
    data = module.read_bytes()
    if len(data) < 4096 or data[:2] != b"MZ" or len(data) > 32 * 1024 * 1024:
        raise SystemExit("Module is missing, invalid, or larger than 32 MiB")
    digest = hashlib.sha256(data).hexdigest()
    storage_path = f"releases/{args.version}/Axiom-{digest[:16]}.dll"
    encoded_path = urllib.parse.quote(storage_path, safe="/")
    for attempt in range(3):
        try:
            request("POST", f"/storage/v1/object/axiom-modules/{encoded_path}", data,
                    "application/octet-stream", {"x-upsert": "true"})
            break
        except (urllib.error.URLError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(1 << attempt)
    rpc = {
        "p_version": args.version, "p_storage_path": storage_path,
        "p_sha256": digest, "p_byte_size": len(data),
        "p_minimum_launcher_version": args.minimum_launcher,
    }
    for attempt in range(3):
        try:
            request("POST", "/rest/v1/rpc/axiom_publish_release",
                    json.dumps(rpc).encode())
            break
        except (urllib.error.URLError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(1 << attempt)
    print(f"published {args.version}: {digest} ({len(data)} bytes)")


def identify_watermark(args: argparse.Namespace) -> None:
    data = Path(args.module).resolve().read_bytes()
    candidates: list[tuple[int, bytes]] = []
    cursor = 0
    while True:
        offset = data.find(b"AXWM", cursor)
        if offset < 0:
            break
        if offset + 64 <= len(data):
            candidates.append((offset, data[offset:offset + 64]))
        cursor = offset + 4
    if not candidates:
        raise SystemExit("No Axiom watermark records were found")

    licenses = request("GET", "/rest/v1/axiom_licenses?select=id,label,enabled")
    devices = request(
        "GET",
        "/rest/v1/axiom_license_devices?select=license_id,device_hash,revoked_at",
    )
    releases = request(
        "GET",
        "/rest/v1/axiom_releases?select=version,sha256,active",
    )
    matches: list[dict[str, object]] = []
    pepper = setting("LICENSE_PEPPER").encode()
    for offset, record in candidates:
        if record[4] != 1 or record[5] not in (1, 2, 3):
            continue
        license_id = str(uuid.UUID(bytes=record[8:24]))
        device_prefix = record[24:40].hex()
        release_prefix = record[40:48].hex()
        stored_tag = record[48:64]
        license_rows = [row for row in licenses if row["id"] == license_id]
        device_rows = [row for row in devices
                       if row["license_id"] == license_id and
                       row["device_hash"].startswith(device_prefix)]
        release_rows = [row for row in releases
                        if row["sha256"].startswith(release_prefix)]
        for license_row in license_rows:
            for device_row in device_rows:
                for release_row in release_rows:
                    material = (
                        f"watermark:v1:{license_id}:{device_row['device_hash']}:"
                        f"{release_row['sha256']}:{record[5]}"
                    ).encode()
                    expected = hmac.new(pepper, material, hashlib.sha256).digest()[:16]
                    if hmac.compare_digest(stored_tag, expected):
                        matches.append({
                            "offset": offset,
                            "slot": record[5],
                            "license_id": license_id,
                            "label": license_row["label"],
                            "license_enabled": license_row["enabled"],
                            "device_hash": device_row["device_hash"],
                            "device_revoked_at": device_row["revoked_at"],
                            "release": release_row["version"],
                            "release_active": release_row["active"],
                        })
    if not matches:
        raise SystemExit("Watermark records exist, but none passed HMAC validation")
    print(json.dumps(matches, ensure_ascii=False, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create-license")
    create.add_argument("--key")
    create.add_argument("--label", default="")
    create.add_argument("--expires-at", help="ISO-8601 timestamp, blank means unlimited")
    create.add_argument("--max-devices", type=int, default=1, choices=range(1, 21))
    create.set_defaults(run=create_license)
    revoke = commands.add_parser("revoke-license")
    revoke.add_argument("key")
    revoke.set_defaults(run=revoke_license)
    listing = commands.add_parser("list-licenses")
    listing.set_defaults(run=list_licenses)
    activations = commands.add_parser("generate-activation-codes")
    activations.add_argument("--plan", required=True)
    activations.add_argument("--count", required=True, type=int)
    activations.add_argument("--output", required=True)
    activations.set_defaults(run=generate_activation_codes)
    release = commands.add_parser("publish")
    release.add_argument("module")
    release.add_argument("--version", required=True)
    release.add_argument("--minimum-launcher", default="1.0.0")
    release.set_defaults(run=publish)
    identify = commands.add_parser("identify-watermark")
    identify.add_argument("module")
    identify.set_defaults(run=identify_watermark)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
