$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'Dll6.vcxproj'
$log = Join-Path $PSScriptRoot 'build.log'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 with C++ workload."
}

$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild.exe not found via vswhere.'
}

Write-Host "Using MSBuild: $msbuild"
& $msbuild $project /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo 2>&1 | Tee-Object -FilePath $log
$code = $LASTEXITCODE
Write-Host "Exit code: $code"

$dll = Join-Path $PSScriptRoot 'x64\Release\Dll6.dll'
if (Test-Path $dll) {
    $info = Get-Item $dll
    Write-Host "Built: $($info.FullName)"
    Write-Host "Size: $($info.Length) bytes"
    Write-Host "Modified: $($info.LastWriteTime)"
} else {
    Write-Host "Dll6.dll not found at expected path: $dll"
}

exit $code
