param(
  [int]$FadeMs = 300,
  [int]$HoldMs = 1000,
  [int]$MaxHoldMs = 3000,
  [int]$TickMs = 30
)

# Full-screen black cover with fade-in, hold, fade-out.
# Minimum sequence: 300ms fade in + 1000ms hold + 300ms fade out = 1.6s.
# Flags in this folder:
#   cover-fade.flag  - start one sequence
#   cover-ready.flag - the content behind the cover is ready; fade out after the hold
#   cover-done.flag  - written when the sequence finishes
#   cover-stop.flag  - exit this process
#   cover-hide.flag  - hide immediately
# Keep this file ASCII-only.

$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies 'System.Windows.Forms','System.Drawing' -TypeDefinition @"
using System.Windows.Forms;
public class TefFadeForm : Form {
  protected override bool ShowWithoutActivation { get { return true; } }
  protected override CreateParams CreateParams {
    get {
      CreateParams cp = base.CreateParams;
      cp.ExStyle |= 0x08000000; // WS_EX_NOACTIVATE
      cp.ExStyle |= 0x00000080; // WS_EX_TOOLWINDOW
      cp.ExStyle |= 0x00000020; // WS_EX_TRANSPARENT (click-through)
      return cp;
    }
  }
}
"@

$script:base = $PSScriptRoot
$script:lockFile = Join-Path $script:base 'watcher.lock'
$script:form = New-Object TefFadeForm
$script:form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$script:form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$script:form.BackColor = [System.Drawing.Color]::Black
$script:form.TopMost = $true
$script:form.ShowInTaskbar = $false
$script:form.Bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
$script:form.Opacity = 0.01
$script:form.Hide()

$script:phase = 'idle'
$script:phaseStart = Get-Date

function Log($m) {
  try {
    $log = Join-Path $script:base 'cover-log.txt'
    "$(Get-Date -Format o) $m" | Out-File -FilePath $log -Append -Encoding utf8
  } catch {}
}
function RemoveFlag($name) {
  try { Remove-Item -LiteralPath (Join-Path $script:base $name) -Force -ErrorAction SilentlyContinue } catch {}
}
function WriteFlag($name) {
  try { Set-Content -LiteralPath (Join-Path $script:base $name) -Value (Get-Date -Format o) -Encoding UTF8 } catch {}
}

function Test-WatcherAlive {
  if (-not (Test-Path $script:lockFile)) { return $false }
  try {
    $age = ((Get-Date) - (Get-Item -LiteralPath $script:lockFile).LastWriteTime).TotalSeconds
    return $age -le 30
  } catch {
    return $false
  }
}

$script:timer = New-Object System.Windows.Forms.Timer
$script:timer.Interval = $TickMs
$script:timer.Add_Tick({
  $now = Get-Date
  $fadeFlag = Join-Path $script:base 'cover-fade.flag'
  $readyFlag = Join-Path $script:base 'cover-ready.flag'
  $stopFlag = Join-Path $script:base 'cover-stop.flag'
  $hideFlag = Join-Path $script:base 'cover-hide.flag'

  if (Test-Path $stopFlag) {
    $script:timer.Stop()
    $script:form.Hide()
    $script:form.Close()
    [System.Windows.Forms.Application]::Exit()
    return
  }
  if (Test-Path $hideFlag) {
    RemoveFlag 'cover-hide.flag'
    $script:form.Hide()
    $script:phase = 'idle'
    return
  }
  if ($script:phase -eq 'idle' -and -not (Test-WatcherAlive)) {
    Log 'watcher gone; exit'
    $script:timer.Stop()
    $script:form.Hide()
    $script:form.Close()
    [System.Windows.Forms.Application]::Exit()
    return
  }
  if ($script:phase -eq 'idle' -and (Test-Path $fadeFlag)) {
    RemoveFlag 'cover-fade.flag'
    RemoveFlag 'cover-done.flag'
    $script:form.Bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $script:form.Opacity = 0.01
    $script:form.Show()
    $script:form.TopMost = $true
    $script:form.BringToFront()
    $script:phase = 'in'
    $script:phaseStart = $now
    Log 'fade in'
    return
  }
  if ($script:phase -eq 'in') {
    $p = [Math]::Min(1.0, ($now - $script:phaseStart).TotalMilliseconds / $FadeMs)
    $script:form.Opacity = [Math]::Max(0.01, $p)
    if ($p -ge 1.0) {
      $script:form.Opacity = 1.0
      $script:phase = 'hold'
      $script:phaseStart = $now
      Log 'hold'
    }
  } elseif ($script:phase -eq 'hold') {
    $held = ($now - $script:phaseStart).TotalMilliseconds
    if (($held -ge $HoldMs -and (Test-Path $readyFlag)) -or $held -ge $MaxHoldMs) {
      $script:phase = 'out'
      $script:phaseStart = $now
      Log ("fade out held=" + [int]$held + " ready=" + (Test-Path $readyFlag))
    }
  } elseif ($script:phase -eq 'out') {
    $p = [Math]::Min(1.0, ($now - $script:phaseStart).TotalMilliseconds / $FadeMs)
    $script:form.Opacity = [Math]::Max(0.01, 1.0 - $p)
    if ($p -ge 1.0) {
      $script:form.Hide()
      $script:form.Opacity = 0.01
      $script:phase = 'idle'
      WriteFlag 'cover-done.flag'
      Log 'done'
    }
  }
})
$script:timer.Start()
[System.Windows.Forms.Application]::Run($script:form)
