# Build a portable mode-B2 release bundle from the built EXE and the docs that
# an operator needs. ASCII only (repo rule).
#
#   powershell -ExecutionPolicy Bypass -File scripts/make-release.ps1 -Version 0.1.0
#
# Produces dist/htmlppt-watchdog-<version>/ plus a .zip and prints the SHA256.

param(
  [Parameter(Mandatory = $true)] [string]$Version,
  [string]$Exe = 'native\build\htmlppt-watchdog.exe',
  [string]$OutDir = 'dist'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Exe)) {
  throw "missing $Exe; build native first (cmake --build native/build)"
}

$stage = Join-Path $OutDir "htmlppt-watchdog-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'docs') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'templates') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'demo') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'scripts') | Out-Null

Copy-Item $Exe (Join-Path $stage 'htmlppt-watchdog.exe')
Copy-Item 'README.md' $stage -ErrorAction SilentlyContinue
Copy-Item 'LICENSE' $stage -ErrorAction SilentlyContinue
Copy-Item 'native\README.md' (Join-Path $stage 'docs\native-README.md')
foreach ($doc in '04-reproduce.md', '05-playbook.md', '06-tooling.md') {
  $path = Join-Path 'docs' $doc
  if (Test-Path $path) { Copy-Item $path (Join-Path $stage 'docs') }
}
Copy-Item 'docs\07-acceptance.md' (Join-Path $stage 'docs') -ErrorAction SilentlyContinue
Copy-Item 'templates\live-page.html' (Join-Path $stage 'templates')
foreach ($file in 'make_deck.py', 'watcher.json', 'README.md') {
  Copy-Item (Join-Path 'demo' $file) (Join-Path $stage 'demo')
}
foreach ($file in 'preflight.sh', 'smoke-watchdog.ps1') {
  Copy-Item (Join-Path 'scripts' $file) (Join-Path $stage 'scripts')
}

$manifest = @(
  "htmlppt-watchdog $Version"
  "built $(Get-Date -Format o)"
  "exe htmlppt-watchdog.exe"
)
$manifest | Set-Content (Join-Path $stage 'VERSION.txt') -Encoding Ascii

$zip = Join-Path $OutDir "htmlppt-watchdog-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Output "release: $zip"
Write-Output "sha256:  $hash"
