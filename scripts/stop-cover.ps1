$base = Split-Path -Parent $MyInvocation.MyCommand.Path
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -like '*black-cover.ps1*' } | ForEach-Object {
  Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
}
Remove-Item (Join-Path $base 'cover-show.flag'), (Join-Path $base 'cover-hide.flag'), (Join-Path $base 'cover-exit.flag') -Force -ErrorAction SilentlyContinue
Write-Output 'black cover stopped'
