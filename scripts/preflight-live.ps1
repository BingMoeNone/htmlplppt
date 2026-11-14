param(
  [Parameter(Mandatory = $true)] [string]$Folder
)

$ErrorActionPreference = 'Stop'
function Fail($message) { Write-Output "FAIL: $message"; exit 1 }

if (-not (Test-Path $Folder)) { Fail "folder not found: $Folder" }
foreach ($name in @('watch-slides.vbs', 'bring-edge.ps1', 'bring-wps.ps1', 'focus-lib.ps1', 'stop-watch.vbs', 'V2.html')) {
  if (-not (Test-Path (Join-Path $Folder $name))) { Fail "missing $name" }
}
$deck = Get-ChildItem -Path $Folder -Filter '*.pptx' -File | Select-Object -First 1
if (-not $deck) { Fail 'no .pptx found in folder' }

$edge = @(
  'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
  'C:\Program Files\Microsoft\Edge\Application\msedge.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) {
  $cmd = Get-Command 'msedge.exe' -ErrorAction SilentlyContinue
  if (-not $cmd) { Fail 'Microsoft Edge not found (install Edge or add msedge.exe to PATH)' }
  $edge = $cmd.Source
}

if (-not (Test-Path (Join-Path $env:WINDIR 'System32\wscript.exe'))) { Fail 'wscript.exe not found' }
if (-not (Test-Path (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'))) { Fail 'Windows PowerShell not found' }

$wps = (Test-Path 'Registry::HKEY_CLASSES_ROOT\Kwpp.Application') -or (Test-Path 'Registry::HKCU\Software\Classes\Kwpp.Application')
if (-not $wps) { Write-Output 'WARN: Kwpp.Application not registered; WPS may not be installed for this user' }

Write-Output "OK: Edge=$edge Deck=$($deck.Name) Folder=$Folder"
