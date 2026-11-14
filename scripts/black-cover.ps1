Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies 'System.Windows.Forms','System.Drawing' -TypeDefinition @"
using System.Windows.Forms;
public class TefCoverForm : Form {
  protected override bool ShowWithoutActivation { get { return true; } }
  protected override CreateParams CreateParams {
    get {
      CreateParams cp = base.CreateParams;
      cp.ExStyle |= 0x08000000; // WS_EX_NOACTIVATE
      cp.ExStyle |= 0x00000080; // WS_EX_TOOLWINDOW
      return cp;
    }
  }
}
"@

$script:base = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$script:form = New-Object TefCoverForm
$script:form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$script:form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$script:form.BackColor = [System.Drawing.Color]::Black
$script:form.TopMost = $true
$script:form.ShowInTaskbar = $false
$script:form.Bounds = $script:screen
$script:form.Hide()
$script:shownAt = $null

$script:timer = New-Object System.Windows.Forms.Timer
$script:timer.Interval = 100
$script:timer.Add_Tick({
  $b = $script:base
  if (Test-Path (Join-Path $b 'cover-show.flag')) {
    Remove-Item (Join-Path $b 'cover-show.flag') -Force -ErrorAction SilentlyContinue
    $script:form.Bounds = $script:screen
    $script:form.Show()
    $script:form.TopMost = $true
    $script:form.BringToFront()
    $script:shownAt = Get-Date
  }
  if (Test-Path (Join-Path $b 'cover-hide.flag')) {
    Remove-Item (Join-Path $b 'cover-hide.flag') -Force -ErrorAction SilentlyContinue
    $script:form.Hide()
    $script:shownAt = $null
  }
  if ($script:form.Visible -and $script:shownAt -ne $null -and ((Get-Date) - $script:shownAt).TotalSeconds -gt 5) {
    $script:form.Hide()
    $script:shownAt = $null
  }
  if (Test-Path (Join-Path $b 'cover-exit.flag')) {
    Remove-Item (Join-Path $b 'cover-exit.flag') -Force -ErrorAction SilentlyContinue
    $script:timer.Stop()
    $script:form.Close()
    $script:shownAt = $null
    [System.Windows.Forms.Application]::Exit()
  }
})
$script:timer.Start()
[System.Windows.Forms.Application]::Run($script:form)
