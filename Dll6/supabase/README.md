# Axiom licensing on Supabase

The launcher contains only the verification public key and the Edge Function URL.
The database is private (RLS plus revoked client grants). The service-role key,
license pepper, IP hashing pepper, and P-256 signing key are Edge Function secrets.
Modules live in the private `axiom-modules` bucket and are streamed in
authenticated chunks after a successful license/device check. Every delivered
module contains three HMAC-authenticated watermark records bound to its license,
device, and release. The signed session covers the personalized module hash.

## Deploy

1. Install/login to the Supabase CLI and link this directory to a project.
2. Apply `migrations/202609010001_axiom_licensing.sql`.
3. Generate two independent random 32-byte values for `LICENSE_PEPPER` and
   `IP_HASH_PEPPER`. Never change `LICENSE_PEPPER` after issuing keys.
4. Convert the existing compatible P-256 key locally:
   `python tools/export_signing_key.py ../license-server/data/signing-key.pem`.
5. Set all three values with `supabase secrets set`; never commit them.
6. Deploy with `supabase functions deploy axiom-license --no-verify-jwt`.
7. Pin the function URL in `kDefaultApiBase` in `launcher_auth.cpp` before
   distribution. Production builds intentionally ignore writable URL overrides.

The function URL is intentionally public: requests reveal no database credential,
all successful responses are bound to the device and one-time nonce, and the
launcher rejects responses without a valid ECDSA signature.

## Administration

Run `tools/admin.py` only on an administrator machine. Set `SUPABASE_URL`,
`SUPABASE_SERVICE_ROLE_KEY`, and the same `LICENSE_PEPPER` in that terminal.
On Windows, `tools/admin.ps1` can keep these values in a current-user DPAPI blob
under `%LOCALAPPDATA%\Axiom\admin` instead of a plaintext `.env` file.

```text
python tools/admin.py create-license --label customer --max-devices 1
python tools/admin.py publish ..\x64\Release\Dll6.dll --version 1.0.2 --minimum-launcher 1.0.1
python tools/admin.py revoke-license AXM-XXXXX-XXXXX-XXXXX-XXXXX
python tools/admin.py identify-watermark leaked.dll
```

Only the generated plaintext key is shown once; the database stores its HMAC.
`identify-watermark` accepts a delivered or memory-dumped module and reports
only records whose HMAC validates against the current licensing database.
