param(
  [Parameter(Mandatory = $true)] [string]$Path,
  [Parameter(Mandatory = $true)] [string]$Out
)

$ErrorActionPreference = 'Stop'
$app = New-Object -ComObject Kwpp.Application
try {
  # WPS Office 12.1 rejects Visible=$false with HRESULT E_FAIL (0x80004005).
  # Fall back to a visible window; this keeps the smoke runnable on WPS.
  try {
    $app.Visible = $false
  } catch {
    Write-Output "warn: Visible=false rejected ($($_.Exception.Message)); running visible"
    $app.Visible = $true
  }
  $presentation = $app.Presentations.Open($Path, $false, $false, $false)
  $count = $presentation.Slides.Count
  Write-Output "slides=$count"
  if ($count -ne 20) { throw "expected 20 slides, got $count" }
  $slide = $presentation.Slides.Item(13)
  $slide.Export($Out, 'PNG', 1920, 1080)
  Write-Output "exported=$Out"
  $presentation.Close()
} finally {
  try { $app.Quit() } catch {}
}
