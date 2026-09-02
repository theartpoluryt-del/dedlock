#!/usr/bin/env python3
"""Minimal self-hosted Axiom license and module delivery service."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import secrets
import sqlite3
import threading
import time
from collections import defaultdict, deque
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)


ROOT = Path(__file__).resolve().parent
DATA_DIR = Path(os.environ.get("AXIOM_SERVER_DATA", ROOT / "data")).resolve()
DATABASE_PATH = DATA_DIR / "licenses.sqlite3"
PRIVATE_KEY_PATH = DATA_DIR / "signing-key.pem"
DEFAULT_MODULE_PATH = (ROOT.parent / "x64" / "Release" / "Dll6.dll").resolve()
TOKEN_LIFETIME_SECONDS = 300
MAX_REQUEST_BYTES = 8192
LICENSE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def utc_timestamp() -> int:
    return int(time.time())


def b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def b64url_decode(value: str) -> bytes:
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def normalize_license(value: str) -> str:
    return value.strip().upper()


def license_digest(value: str) -> str:
    return hashlib.sha256(normalize_license(value).encode("utf-8")).hexdigest()


def database() -> sqlite3.Connection:
    connection = sqlite3.connect(DATABASE_PATH, timeout=10)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys=ON")
    connection.execute("PRAGMA journal_mode=WAL")
    return connection


def initialize_database() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    with database() as connection:
        connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS licenses (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                key_hash TEXT NOT NULL UNIQUE,
                label TEXT NOT NULL DEFAULT '',
                enabled INTEGER NOT NULL DEFAULT 1,
                expires_at INTEGER,
                max_devices INTEGER NOT NULL DEFAULT 1,
                created_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS devices (
                license_id INTEGER NOT NULL,
                device_hash TEXT NOT NULL,
                first_seen_at INTEGER NOT NULL,
                last_seen_at INTEGER NOT NULL,
                PRIMARY KEY (license_id, device_hash),
                FOREIGN KEY (license_id) REFERENCES licenses(id) ON DELETE CASCADE
            );
            """
        )


def ensure_signing_key() -> ec.EllipticCurvePrivateKey:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if PRIVATE_KEY_PATH.exists():
        key = serialization.load_pem_private_key(
            PRIVATE_KEY_PATH.read_bytes(), password=None
        )
        if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(
            key.curve, ec.SECP256R1
        ):
            raise RuntimeError("signing-key.pem is not an ECDSA P-256 key")
        return key
    key = ec.generate_private_key(ec.SECP256R1())
    private_bytes = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    temporary = PRIVATE_KEY_PATH.with_suffix(".tmp")
    temporary.write_bytes(private_bytes)
    os.chmod(temporary, 0o600)
    temporary.replace(PRIVATE_KEY_PATH)
    return key


def public_key_hex(key: ec.EllipticCurvePrivateKey) -> str:
    numbers = key.public_key().public_numbers()
    return numbers.x.to_bytes(32, "big").hex() + numbers.y.to_bytes(32, "big").hex()


def module_path() -> Path:
    return Path(os.environ.get("AXIOM_MODULE_PATH", DEFAULT_MODULE_PATH)).resolve()


def module_metadata() -> tuple[Path, bytes, str]:
    path = module_path()
    data = path.read_bytes()
    if len(data) < 4096 or data[:2] != b"MZ":
        raise RuntimeError(f"module is missing or invalid: {path}")
    return path, data, hashlib.sha256(data).hexdigest()


def canonical_payload(fields: dict[str, str | int]) -> bytes:
    order = ("v", "license", "device", "expires", "nonce", "sha256", "size")
    return "&".join(f"{name}={fields[name]}" for name in order).encode("ascii")


def sign_token(key: ec.EllipticCurvePrivateKey, payload: bytes) -> str:
    der = key.sign(payload, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    raw_signature = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    return f"{b64url_encode(payload)}.{b64url_encode(raw_signature)}"


def verify_token(
    key: ec.EllipticCurvePrivateKey, token: str
) -> dict[str, str] | None:
    try:
        encoded_payload, encoded_signature = token.split(".", 1)
        payload = b64url_decode(encoded_payload)
        raw_signature = b64url_decode(encoded_signature)
        if len(raw_signature) != 64:
            return None
        r = int.from_bytes(raw_signature[:32], "big")
        s = int.from_bytes(raw_signature[32:], "big")
        key.public_key().verify(
            encode_dss_signature(r, s), payload, ec.ECDSA(hashes.SHA256())
        )
        fields = {name: values[0] for name, values in parse_qs(
            payload.decode("ascii"), strict_parsing=True
        ).items()}
        if fields.get("v") != "1" or int(fields["expires"]) < utc_timestamp():
            return None
        return fields
    except (ValueError, KeyError, UnicodeError):
        return None
    except Exception:
        return None


class RateLimiter:
    def __init__(self, limit: int = 12, window_seconds: int = 60) -> None:
        self.limit = limit
        self.window_seconds = window_seconds
        self.events: dict[str, deque[float]] = defaultdict(deque)
        self.lock = threading.Lock()

    def allow(self, address: str) -> bool:
        now = time.monotonic()
        with self.lock:
            events = self.events[address]
            while events and now - events[0] > self.window_seconds:
                events.popleft()
            if len(events) >= self.limit:
                return False
            events.append(now)
            return True


class AxiomHandler(BaseHTTPRequestHandler):
    server_version = "AxiomLicense/1"
    signing_key: ec.EllipticCurvePrivateKey
    limiter = RateLimiter()

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{self.log_date_time_string()} {self.client_address[0]} {fmt % args}")

    def send_json(self, status: HTTPStatus, payload: dict[str, object]) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        request_url = urlsplit(self.path)
        if request_url.path == "/health":
            self.send_json(HTTPStatus.OK, {"ok": True})
            return
        if request_url.path != "/v1/module":
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False})
            return
        authorization = self.headers.get("Authorization", "")
        if not authorization.startswith("Bearer "):
            self.send_json(HTTPStatus.UNAUTHORIZED, {"ok": False})
            return
        token = authorization[7:].strip()
        fields = verify_token(self.signing_key, token)
        if not fields:
            self.send_json(HTTPStatus.UNAUTHORIZED, {"ok": False})
            return
        with database() as connection:
            license_row = connection.execute(
                "SELECT enabled, expires_at FROM licenses WHERE id=?",
                (int(fields["license"]),),
            ).fetchone()
        if not license_row or not license_row["enabled"] or (
            license_row["expires_at"] is not None
            and license_row["expires_at"] < utc_timestamp()
        ):
            self.send_json(HTTPStatus.FORBIDDEN, {"ok": False})
            return
        try:
            _, data, digest = module_metadata()
        except (OSError, RuntimeError):
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE, {"ok": False})
            return
        if digest != fields.get("sha256") or str(len(data)) != fields.get("size"):
            self.send_json(HTTPStatus.CONFLICT, {"ok": False, "retry": True})
            return
        query = parse_qs(request_url.query)
        try:
            offset = int(query.get("offset", ["0"])[0])
            requested = int(query.get("length", [str(len(data))])[0])
        except (TypeError, ValueError):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False})
            return
        if offset < 0 or offset > len(data) or requested <= 0:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False})
            return
        # Small independent responses survive restricted and unstable routes
        # much more reliably than a single multi-megabyte tunnel stream.
        requested = min(requested, 512 * 1024)
        payload = data[offset : offset + requested]
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("X-Axiom-Offset", str(offset))
        self.send_header("X-Axiom-Total", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self) -> None:
        if self.path != "/v1/session":
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False})
            return
        if not self.limiter.allow(self.client_address[0]):
            self.send_json(HTTPStatus.TOO_MANY_REQUESTS, {"ok": False})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_REQUEST_BYTES:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False})
            return
        try:
            request = json.loads(self.rfile.read(length))
            raw_key = normalize_license(str(request["key"]))
            device = str(request["device"])
            nonce = str(request["nonce"])
            if (
                len(raw_key) < 12
                or len(device) != 64
                or len(nonce) != 32
                or any(character not in "0123456789abcdef" for character in device + nonce)
            ):
                raise ValueError
        except (ValueError, KeyError, TypeError, json.JSONDecodeError):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False})
            return

        now = utc_timestamp()
        with database() as connection:
            connection.execute("BEGIN IMMEDIATE")
            license_row = connection.execute(
                "SELECT * FROM licenses WHERE key_hash=?", (license_digest(raw_key),)
            ).fetchone()
            if not license_row or not license_row["enabled"] or (
                license_row["expires_at"] is not None
                and license_row["expires_at"] < now
            ):
                self.send_json(HTTPStatus.UNAUTHORIZED, {"ok": False})
                return
            existing = connection.execute(
                "SELECT 1 FROM devices WHERE license_id=? AND device_hash=?",
                (license_row["id"], device),
            ).fetchone()
            if not existing:
                device_count = connection.execute(
                    "SELECT COUNT(*) FROM devices WHERE license_id=?",
                    (license_row["id"],),
                ).fetchone()[0]
                if device_count >= license_row["max_devices"]:
                    self.send_json(HTTPStatus.FORBIDDEN, {"ok": False})
                    return
                connection.execute(
                    "INSERT INTO devices VALUES (?, ?, ?, ?)",
                    (license_row["id"], device, now, now),
                )
            else:
                connection.execute(
                    "UPDATE devices SET last_seen_at=? WHERE license_id=? AND device_hash=?",
                    (now, license_row["id"], device),
                )
        try:
            _, data, digest = module_metadata()
        except (OSError, RuntimeError):
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE, {"ok": False})
            return
        expiration = now + TOKEN_LIFETIME_SECONDS
        payload = canonical_payload(
            {
                "v": 1,
                "license": license_row["id"],
                "device": device,
                "expires": expiration,
                "nonce": nonce,
                "sha256": digest,
                "size": len(data),
            }
        )
        token = sign_token(self.signing_key, payload)
        self.send_json(
            HTTPStatus.OK,
            {"ok": True, "token": token, "expires": expiration},
        )


def generate_license() -> str:
    groups = [
        "".join(secrets.choice(LICENSE_ALPHABET) for _ in range(5))
        for _ in range(4)
    ]
    return "AXM-" + "-".join(groups)


def add_license(key: str, label: str, days: int | None, max_devices: int) -> None:
    normalized = normalize_license(key)
    expiration = utc_timestamp() + days * 86400 if days else None
    with database() as connection:
        connection.execute(
            "INSERT INTO licenses(key_hash,label,enabled,expires_at,max_devices,created_at) "
            "VALUES(?,?,1,?,?,?)",
            (license_digest(normalized), label, expiration, max_devices, utc_timestamp()),
        )


def main() -> None:
    parser = argparse.ArgumentParser(description="Axiom license server")
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("init")
    create = subcommands.add_parser("create-key")
    create.add_argument("--key")
    create.add_argument("--label", default="")
    create.add_argument("--days", type=int)
    create.add_argument("--devices", type=int, default=1)
    subcommands.add_parser("list-keys")
    revoke = subcommands.add_parser("revoke-key")
    revoke.add_argument("id", type=int)
    reset = subcommands.add_parser("reset-devices")
    reset.add_argument("id", type=int)
    serve = subcommands.add_parser("serve")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8787)
    subcommands.add_parser("show-public-key")
    args = parser.parse_args()

    initialize_database()
    key = ensure_signing_key()
    if args.command == "init":
        print(f"database: {DATABASE_PATH}")
        print(f"module: {module_path()}")
        print(f"public-key: {public_key_hex(key)}")
    elif args.command == "create-key":
        value = normalize_license(args.key or generate_license())
        if args.devices < 1 or args.days is not None and args.days < 1:
            parser.error("days and devices must be positive")
        add_license(value, args.label, args.days, args.devices)
        print(value)
    elif args.command == "list-keys":
        with database() as connection:
            rows = connection.execute(
                "SELECT l.id,l.label,l.enabled,l.expires_at,l.max_devices,"
                "COUNT(d.device_hash) devices FROM licenses l LEFT JOIN devices d "
                "ON d.license_id=l.id GROUP BY l.id ORDER BY l.id"
            )
            for row in rows:
                expires = (
                    datetime.fromtimestamp(row["expires_at"], timezone.utc).isoformat()
                    if row["expires_at"]
                    else "never"
                )
                print(
                    f"id={row['id']} enabled={row['enabled']} label={row['label']!r} "
                    f"expires={expires} devices={row['devices']}/{row['max_devices']}"
                )
    elif args.command == "revoke-key":
        with database() as connection:
            connection.execute("UPDATE licenses SET enabled=0 WHERE id=?", (args.id,))
    elif args.command == "reset-devices":
        with database() as connection:
            connection.execute("DELETE FROM devices WHERE license_id=?", (args.id,))
    elif args.command == "show-public-key":
        print(public_key_hex(key))
    elif args.command == "serve":
        AxiomHandler.signing_key = key
        server = ThreadingHTTPServer((args.host, args.port), AxiomHandler)
        print(f"Axiom server listening on http://{args.host}:{args.port}")
        print(f"module: {module_path()}")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()


if __name__ == "__main__":
    main()
