# focus-agent.ps1 - persistent Edge focus agent for watch-live-content.vbs.
# Started once by the watcher, so PowerShell and the Win32 helper are already
# warm when a slide triggers. Waits for agent-arm.flag:
#   <profile>|<exitFlag>|<readyFlag>
# Focuses the Edge window for that profile until it closes, writes the ready
# flag on first focus and the exit flag when the window is gone.
# Exits on agent-stop.flag, or when watcher.lock is missing/older than 30s.
# Keep this file ASCII-only.

$ErrorActionPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'focus-lib.ps1')

$script:base = $PSScriptRoot
$script:armFlag = Join-Path $script:base 'agent-arm.flag'
$script:stopFlag = Join-Path $script:base 'agent-stop.flag'
$script:lockFile = Join-Path $script:base 'watcher.lock'
$script:returnFlag = Join-Path $script:base 'return-request.flag'

function Write-Flag([string]$path) {
  if ([string]::IsNullOrEmpty($path)) { return }
  try { Set-Content -LiteralPath $path -Value (Get-Date -Format o) -Encoding UTF8 } catch {}
}

function Test-AgentAlive {
  if (Test-Path $script:stopFlag) { return $false }
  if (-not (Test-Path $script:lockFile)) { return $false }
  try {
    $age = ((Get-Date) - (Get-Item -LiteralPath $script:lockFile).LastWriteTime).TotalSeconds
    if ($age -gt 30) { return $false }
  } catch {
    return $false
  }
  return $true
}

function Focus-Session([string]$Profile, [string]$ExitFlag, [string]$ReadyFlag) {
  $startDeadline = (Get-Date).AddSeconds(20)
  $deadline = (Get-Date).AddSeconds(600)
  $hwnd = [IntPtr]::Zero
  $found = $false
  $ready = $false
  $returnSignaled = $false
  $lastAttempt = [DateTime]::MinValue
  Write-TefLog ("agent session profile=" + $Profile)
  while ((Get-Date) -lt $deadline) {
    if (-not (Test-AgentAlive)) { Write-TefLog 'agent stopped during session'; break }
    if (-not $found) {
      if ((Get-Date) -ge $startDeadline) { Write-TefLog 'agent start timeout'; break }
      $hwnd = Find-TefEdgeWindowByProfile -Marker $Profile
      if ($hwnd -ne [IntPtr]::Zero) {
        $found = $true
        Write-TefLog ("agent found {0} profile={1}" -f $hwnd, $Profile)
      }
    } else {
      if (-not [TefWindow]::IsWindow($hwnd)) {
        $fresh = Find-TefEdgeWindowByProfile -Marker $Profile
        if ($fresh -ne [IntPtr]::Zero) {
          $hwnd = $fresh
        } else {
          break
        }
      }
      if (-not $returnSignaled) {
        $title = [TefWindow]::TextOf($hwnd)
        if ($title -like '*TEF-LIVE-RETURN*') {
          Write-Flag $script:returnFlag
          $returnSignaled = $true
          Write-TefLog 'agent return requested'
        }
      }
      if ([TefWindow]::IsForeground($hwnd)) {
        if (-not $ready) { Write-Flag $ReadyFlag; $ready = $true }
      } elseif (((Get-Date) - $lastAttempt).TotalMilliseconds -ge 200) {
        $lastAttempt = Get-Date
        if ([TefWindow]::Force($hwnd, $true)) {
          if (-not $ready) {
            Write-Flag $ReadyFlag
            $ready = $true
            Write-TefLog ("agent focused {0}" -f $hwnd)
          }
        }
      }
    }
    Start-Sleep -Milliseconds 100
  }
  Write-Flag $ExitFlag
  Write-TefLog 'agent session end'
}

Write-TefLog 'agent start'
while (Test-AgentAlive) {
  if (Test-Path $script:armFlag) {
    $raw = ''
    try { $raw = (Get-Content -LiteralPath $script:armFlag -Raw).Trim() } catch {}
    Remove-Item -LiteralPath $script:armFlag -Force -ErrorAction SilentlyContinue
    $parts = $raw -split '\|'
    if ($parts.Count -ge 3) {
      Focus-Session -Profile $parts[0] -ExitFlag $parts[1] -ReadyFlag $parts[2]
    } else {
      Write-TefLog 'agent bad arm flag'
    }
  }
  Start-Sleep -Milliseconds 50
}
Write-TefLog 'agent exit'
