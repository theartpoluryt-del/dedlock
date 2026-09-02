$ErrorActionPreference = "Stop"
$serverDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $serverDirectory

python -c "import cryptography" 2>$null
if ($LASTEXITCODE -ne 0) {
    python -m pip install -r requirements.txt
}

python server.py init
python server.py serve
