param(
  [int]$TimeoutSeconds = 600,
  [int]$IntervalMilliseconds = 300,
  [string]$ProfileMarker = '',
  [int]$StartTimeoutSeconds = 0,
  [string]$ExitFlag = ''
)

# Keep the Edge app window in the foreground until it closes. By default the
# watcher flow locates the window by the TEF-LIVE-STAGE title; button-mode
# launchers pass -ProfileMarker so any HTML title can be focused.
. (Join-Path $PSScriptRoot 'focus-lib.ps1')

function Get-EdgeWindow {
  if (-not [string]::IsNullOrEmpty($ProfileMarker)) {
    return Find-TefEdgeWindowByProfile -Marker $ProfileMarker
  }
  return Find-TefEdgeWindow
}

Write-TefLog 'bring-edge start'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
if ($StartTimeoutSeconds -gt 0) {
  $startDeadline = (Get-Date).AddSeconds($StartTimeoutSeconds)
} else {
  $startDeadline = $deadline
}
$hwnd = [IntPtr]::Zero
$foundOnce = $false
$loggedFocus = $false
$lastFailLog = [DateTime]::MinValue
$lastAttempt = [DateTime]::MinValue

while ((Get-Date) -lt $deadline) {
  if ($hwnd -eq [IntPtr]::Zero -or -not [TefWindow]::IsWindow($hwnd)) {
    if ($foundOnce) { break }
    if ((Get-Date) -ge $startDeadline) {
      Write-TefLog 'bring-edge start timeout'
      break
    }
    $hwnd = Get-EdgeWindow
    if ($hwnd -ne [IntPtr]::Zero) {
      $foundOnce = $true
      if ([string]::IsNullOrEmpty($ProfileMarker)) {
        Write-TefLog ("found {0}" -f $hwnd)
      } else {
        Write-TefLog ("found {0} profile={1}" -f $hwnd, $ProfileMarker)
      }
    }
  }

  if ($hwnd -ne [IntPtr]::Zero) {
    if ([TefWindow]::IsForeground($hwnd)) {
      $loggedFocus = $true
    } elseif (((Get-Date) - $lastAttempt).TotalMilliseconds -ge 1000) {
      $lastAttempt = Get-Date
      $fresh = Get-EdgeWindow
      if ($fresh -ne [IntPtr]::Zero -and $fresh -ne $hwnd) {
        $hwnd = $fresh
        Write-TefLog ("refound {0}" -f $hwnd)
      }
      $ok = [TefWindow]::Force($hwnd, $true)
      if ($ok) {
        if (-not $loggedFocus) {
          Write-TefLog ("focused {0}" -f $hwnd)
          $loggedFocus = $true
        }
      } elseif (((Get-Date) - $lastFailLog).TotalSeconds -gt 5) {
        Write-TefLog ("focus failed {0}" -f $hwnd)
        $lastFailLog = Get-Date
      }
    }
  }

  Start-Sleep -Milliseconds $IntervalMilliseconds
}

Write-TefLog 'bring-edge exit'
if (-not [string]::IsNullOrEmpty($ExitFlag)) {
  try { Set-Content -LiteralPath $ExitFlag -Value (Get-Date -Format o) -Encoding UTF8 } catch {}
}
