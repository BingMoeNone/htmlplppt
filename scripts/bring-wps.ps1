param(
  [int]$TimeoutSeconds = 6,
  [int]$IntervalMilliseconds = 500
)

# Re-focus the WPS slideshow window after ResumeShow. WPS does not expose
# SlideShowWindow.HWND, so locate its fullscreen window by enumerating the
# windows owned by wpp.exe / wps.exe and scoring slideshow traits.
. (Join-Path $PSScriptRoot 'focus-lib.ps1')
Add-Type -AssemblyName System.Windows.Forms

function Find-InPids {
  param([uint32[]]$Pids, [bool]$RequireShowTrait)
  if ($Pids.Count -eq 0) { return [IntPtr]::Zero }

  $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $minW = [int]($screen.Width * 0.9)
  $minH = [int]($screen.Height * 0.9)

  $hwnd = [TefWindow]::FindFullscreenForPids($Pids, $minW, $minH)
  if ($hwnd -ne [IntPtr]::Zero -and $RequireShowTrait -and -not [TefWindow]::LooksLikeShow($hwnd)) {
    $hwnd = [IntPtr]::Zero
  }
  if ($hwnd -eq [IntPtr]::Zero) {
    $parts = [string[]]@('Slide Show', 'Slideshow', 'LyMtrix-TEF-Lv6-live')
    $hwnd = [TefWindow]::FindTitleForPids($Pids, $parts)
  }
  return $hwnd
}

function Find-WpsShowWindow {
  # wpp.exe is WPS Presentation; POWERPNT.EXE is Microsoft PowerPoint.
  # Prefer those; wps.exe may also own Writer windows, so only accept
  # windows that clearly look like a slideshow.
  $wppPids = @(Get-Process -Name wpp, POWERPNT -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
  $hwnd = Find-InPids -Pids $wppPids -RequireShowTrait $false
  if ($hwnd -ne [IntPtr]::Zero) { return $hwnd }

  $wpsPids = @(Get-Process -Name wps -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
  return Find-InPids -Pids $wpsPids -RequireShowTrait $true
}

Write-TefLog 'bring-wps start'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$done = $false
$hwnd = [IntPtr]::Zero

while ((Get-Date) -lt $deadline) {
  $hwnd = Find-WpsShowWindow
  if ($hwnd -ne [IntPtr]::Zero) {
    if ([TefWindow]::Force($hwnd, $true)) {
      Write-TefLog ("focused {0} class={1} title={2}" -f $hwnd, [TefWindow]::ClassOf($hwnd), [TefWindow]::TextOf($hwnd))
      $done = $true
      break
    }
  }
  Start-Sleep -Milliseconds $IntervalMilliseconds
}

if (-not $done) {
  if ($hwnd -ne [IntPtr]::Zero) {
    Write-TefLog ("failed {0}" -f $hwnd)
  } else {
    Write-TefLog 'failed no-window'
  }
  exit 1
}
Write-TefLog 'bring-wps exit'
exit 0
