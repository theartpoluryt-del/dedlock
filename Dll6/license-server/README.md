# Axiom license server

The server keeps license hashes and device bindings in SQLite, signs five-minute
sessions with ECDSA P-256, and serves the current DLL only to an authorized
session. The private signing key is generated below `data/` and must never be
copied to a client or committed to Git.

```powershell
python -m pip install -r requirements.txt
python server.py init
python server.py create-key --label "customer" --devices 1
python server.py list-keys
python server.py serve
```

On the development PC, `start-local.ps1` performs the dependency check and
starts the service on `127.0.0.1:8787`. A development launcher can point to it
by placing that URL in `%LOCALAPPDATA%\Axiom\launcher\server.ini`.

For production, put the service behind an HTTPS reverse proxy, set
`AXIOM_SERVER_DATA` to a persistent private directory, and set
`AXIOM_MODULE_PATH` to the released DLL. Back up `signing-key.pem` and
`licenses.sqlite3`; losing the signing key requires rebuilding the launcher
with a new public key.

The production launcher has an HTTPS endpoint compiled in. `server.ini` is an
optional first-line override for development and incident recovery; remote
plain HTTP endpoints are rejected.

Administrative operations are intentionally CLI-only:

```powershell
python server.py revoke-key 3
python server.py reset-devices 3
```
