param(
    [ValidateSet('arm64', 'x86', 'x64')]
    [string]$Arch = 'x64'
)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $targetArch = $env:VSCMD_ARG_TGT_ARCH
    if (-not $targetArch) { throw "Open a Visual Studio Native Tools PowerShell session configured for $Arch." }
    if ($targetArch -ne $Arch) { throw "This shell targets $targetArch; open a matching $Arch shell." }
    $outputDirectory = Join-Path $PSScriptRoot "build/$Arch"
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    $resource = Join-Path $outputDirectory 'kitty-litter.res'
    & rc.exe /nologo "/fo$resource" (Join-Path $PSScriptRoot 'app.rc')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $source = Join-Path $PSScriptRoot 'main.c'
    $output = Join-Path $outputDirectory 'kitty-litter.exe'
    & cl.exe /nologo /O2 /W4 /DWIN32_LEAN_AND_MEAN $source $resource "/Fe:$output" /link /SUBSYSTEM:WINDOWS comctl32.lib shell32.lib ole32.lib
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
