#!/usr/bin/env python3
"""Print PKCS#8 DER as base64 for `supabase secrets set` (run locally only)."""

import base64
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

path = Path(sys.argv[1] if len(sys.argv) > 1 else
            "license-server/data/signing-key.pem")
key = serialization.load_pem_private_key(path.read_bytes(), password=None)
if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
    raise SystemExit("Expected an unencrypted ECDSA P-256 private key")
der = key.private_bytes(serialization.Encoding.DER,
                        serialization.PrivateFormat.PKCS8,
                        serialization.NoEncryption())
print(base64.b64encode(der).decode("ascii"))
