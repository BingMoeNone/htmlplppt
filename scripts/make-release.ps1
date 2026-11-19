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

# The version is joined into a directory name that the script deletes
# recursively, so it must be a plain semver-shaped token: a value such as '*' or
# '0.1.0\..\..\..' would otherwise reach a Remove-Item for another directory.
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$') {
  throw "invalid -Version '$Version'; expected MAJOR.MINOR.PATCH with an optional -suffix"
}

if (-not (Test-Path -LiteralPath $Exe)) {
  throw "missing $Exe; build native first (cmake --build native/build)"
}

$out_dir_full = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutDir))
$stage = Join-Path $OutDir "htmlppt-watchdog-$Version"
$stage_full = [System.IO.Path]::GetFullPath($stage)
if (-not $stage_full.StartsWith(
    $out_dir_full + [System.IO.Path]::DirectorySeparatorChar,
    [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "refusing a stage path outside $out_dir_full"
}
if (Test-Path -LiteralPath $stage) {
  Remove-Item -LiteralPath $stage -Recurse -Force
}
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
foreach ($file in 'preflight.sh', 'smoke-watchdog.ps1', 'setup.sh') {
  Copy-Item (Join-Path 'scripts' $file) (Join-Path $stage 'scripts')
}
# The bundled demo/README tells the operator to run the preflight/setup scripts
# and demo/make_deck.py, so the Python pieces those need must be in the bundle
# too; without them the documented steps cannot run.
Copy-Item 'requirements.txt' $stage -ErrorAction SilentlyContinue
Copy-Item 'package.json' $stage -ErrorAction SilentlyContinue
Copy-Item 'vendor' (Join-Path $stage 'vendor') -Recurse -ErrorAction SilentlyContinue

$manifest = @(
  "htmlppt-watchdog $Version"
  "built $(Get-Date -Format o)"
  "exe htmlppt-watchdog.exe"
)
$manifest | Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Encoding Ascii

$zip = Join-Path $OutDir "htmlppt-watchdog-$Version.zip"
$zip_full = [System.IO.Path]::GetFullPath($zip)
if (-not $zip_full.StartsWith(
    $out_dir_full + [System.IO.Path]::DirectorySeparatorChar,
    [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "refusing a zip path outside $out_dir_full"
}
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
# Archive the stage directory itself, so unpacking yields one folder instead of
# scattering its files into the current directory.
Compress-Archive -LiteralPath $stage_full -DestinationPath $zip

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Output "release: $zip"
Write-Output "sha256:  $hash"
