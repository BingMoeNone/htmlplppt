# Self-test for Windows foreground activation.
# Creates a small visible window in a child process, calls TefWindow.Force,
# verifies GetForegroundWindow, then restores the original foreground window.
# The test briefly steals focus (about 1-2 seconds). Keep this file ASCII-only.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'focus-lib.ps1')

$childCode = @'
$out = $env:TEF_FOCUS_OUT
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = New-Object System.Windows.Forms.Form
$form.Text = 'TEF-FOCUS-SELFTEST'
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedToolWindow
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$form.Location = New-Object System.Drawing.Point(60, 60)
$form.Size = New-Object System.Drawing.Size(240, 120)
$form.ShowInTaskbar = $false
$form.TopMost = $false
$form.Show()
[System.Windows.Forms.Application]::DoEvents()
[System.IO.File]::WriteAllText($out, $form.Handle.ToInt64().ToString())
$end = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $end) {
  [System.Windows.Forms.Application]::DoEvents()
  Start-Sleep -Milliseconds 100
}
$form.Close()
'@

$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($childCode))
$out = Join-Path $env:TEMP ("tef-focus-selftest-{0}.txt" -f $PID)
Remove-Item $out -ErrorAction SilentlyContinue
$env:TEF_FOCUS_OUT = $out

$orig = [TefWindow]::GetForegroundWindow()
$child = Start-Process -FilePath 'powershell.exe' `
  -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded) `
  -WindowStyle Hidden -PassThru

$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline -and -not (Test-Path $out)) { Start-Sleep -Milliseconds 100 }

$test = [IntPtr]::Zero
if (Test-Path $out) { $test = [IntPtr][long](Get-Content $out) }
Start-Sleep -Milliseconds 300

$ok = $false
if ($test -ne [IntPtr]::Zero) { $ok = [TefWindow]::Force($test, $false) }
$fg = [TefWindow]::GetForegroundWindow()
$match = ($fg -eq $test)

$restored = $false
if ($orig -ne [IntPtr]::Zero) {
  [void][TefWindow]::Force($orig, $false)
  $restored = ([TefWindow]::GetForegroundWindow() -eq $orig)
}

try { Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue } catch {}
Remove-Item $out -ErrorAction SilentlyContinue
Remove-Item Env:TEF_FOCUS_OUT -ErrorAction SilentlyContinue

Write-Output ("orig={0} test={1} force={2} fg={3} match={4} restored={5}" -f $orig, $test, $ok, $fg, $match, $restored)
if ($match) {
  Write-Output 'PASS: foreground activation works'
  exit 0
}
Write-Output 'FAIL: foreground activation was blocked'
exit 1
