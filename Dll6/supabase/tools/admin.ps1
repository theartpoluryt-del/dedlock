param(
    [switch]$Initialize,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AdminArguments
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$stateDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Axiom\admin'
$statePath = Join-Path $stateDirectory 'supabase-admin.dpapi'
$entropy = [Text.Encoding]::UTF8.GetBytes('Axiom/SupabaseAdmin/v1')

if ($Initialize) {
    $plainText = [Console]::In.ReadToEnd()
    $settings = $plainText | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($settings.supabase_url) -or
        [string]::IsNullOrWhiteSpace($settings.service_role_key) -or
        (-not [string]::IsNullOrWhiteSpace($settings.license_pepper) -and
         $settings.license_pepper.Length -lt 32) -or
        (-not [string]::IsNullOrWhiteSpace($settings.activation_code_pepper) -and
         $settings.activation_code_pepper.Length -lt 32)) {
        throw 'Invalid Axiom Supabase administrator state.'
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($plainText)
    $protected = [System.Security.Cryptography.ProtectedData]::Protect(
        $bytes, $entropy,
        [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    [IO.Directory]::CreateDirectory($stateDirectory) | Out-Null
    [IO.File]::WriteAllBytes($statePath, $protected)
    exit 0
}

if (-not [IO.File]::Exists($statePath)) {
    throw 'Run admin.ps1 -Initialize first.'
}
$protected = [IO.File]::ReadAllBytes($statePath)
$bytes = [System.Security.Cryptography.ProtectedData]::Unprotect(
    $protected, $entropy,
    [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
$settings = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($settings.service_role_key)) {
    throw 'The local administrator state does not contain a Supabase secret key.'
}
$env:SUPABASE_URL = $settings.supabase_url
$env:SUPABASE_SERVICE_ROLE_KEY = $settings.service_role_key
if (-not [string]::IsNullOrWhiteSpace($settings.license_pepper)) {
    $env:LICENSE_PEPPER = $settings.license_pepper
}
if (-not [string]::IsNullOrWhiteSpace($settings.activation_code_pepper)) {
    $env:ACTIVATION_CODE_PEPPER = $settings.activation_code_pepper
}
try {
    & python (Join-Path $PSScriptRoot 'admin.py') @AdminArguments
    exit $LASTEXITCODE
} finally {
    Remove-Item Env:SUPABASE_URL -ErrorAction SilentlyContinue
    Remove-Item Env:SUPABASE_SERVICE_ROLE_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:LICENSE_PEPPER -ErrorAction SilentlyContinue
    Remove-Item Env:ACTIVATION_CODE_PEPPER -ErrorAction SilentlyContinue
}
