# smoke-watchdog.ps1 - on-machine check for the mode-B2 watchdog loop.
#
# The supervisor resolves the config, writes worker-plan.json and
# worker-config.json, then launches the worker automatically. The worker opens
# the deck in WPS, starts the slideshow, drives the cover/Edge/show actions,
# and heartbeats its state into the shared black box.
#
# This script starts the supervisor as a background process, waits for the
# plan, polls --status once a second until the black box reports a worker
# state, then runs --stop and requires the supervisor to exit 0. Every wait is
# bounded by -HoldSeconds. On any failure the supervisor is torn down and the
# last 20 journal records are printed, so the operator sees why the run did not
# reach the show.
#
# WPS Presentation must be installed and the deck must be closed before a run:
# the supervisor reads the deck facts through COM and the worker opens the deck.
#
# This file is ASCII only: the repository smoke test requires ASCII for the
# shipped .ps1/.vbs scripts.

param(
  [Parameter(Mandatory = $true)] [string]$Config,
  [Parameter(Mandatory = $true)] [string]$StateDir,
  [Parameter(Mandatory = $true)] [string]$Exe,
  [int]$HoldSeconds = 20
)

$ErrorActionPreference = 'Stop'

$RunId = 'smoke'
$TailLines = 20
$Supervisor = $null
$ConfigPath = ''
$StatePath = ''
$JournalPath = ''
$PlanPath = ''
$ExePath = ''

# --status prints one line per black-box record:
#   <sequence> <timestamp_ms> <event> <state> <detail>
# A line with those leading numeric fields is a state line: the worker is
# publishing state records. The header ("run ... records ...") and the trailing
# "journal ..." line cannot match it.
$StateLinePattern = '^\s*\d+\s+\d+\s+\d+\s+\d+(\s|$)'

function Resolve-InputPath {
  param([string]$Path)
  # Prefix relative paths with the current location but do not canonicalize:
  # Path.GetFullPath expands 8.3 short names to their long form, and the native
  # CLI parses argv through the ANSI code page, so an 8.3 path such as
  # C:\Users\ADMIN~1\... must stay exactly as given.
  if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
  return Join-Path (Get-Location).Path $Path
}

function Format-PathArgument {
  param([string]$Path)
  # CommandLineToArgvW treats backslashes before the closing quote as escapes,
  # so double a trailing run before quoting. Windows paths cannot contain a
  # double quote, so no other escaping is needed.
  $doubled = [regex]::Replace($Path, '\\+$', '$&$&')
  return '"' + $doubled + '"'
}

# Launches a process with redirected stdout/stderr and no console window. Unlike
# Start-Process, the .NET Process object keeps a usable ExitCode after the
# process exits, which the bounded waits below need.
function Start-CapturedProcess {
  param([string]$FilePath, [string]$Arguments)
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $FilePath
  $info.Arguments = $Arguments
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $info
  if (-not $process.Start()) {
    throw "cannot start process: $FilePath"
  }
  return $process
}

# Runs a one-shot CLI invocation and returns its exit code plus captured output.
# The wait is bounded; on timeout the process is killed and ExitCode is -1.
function Invoke-CliCapture {
  param([string]$FilePath, [string]$Arguments, [int]$TimeoutSeconds)
  $process = Start-CapturedProcess -FilePath $FilePath -Arguments $Arguments
  try {
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
      try { $process.Kill() } catch { }
      [void]$process.WaitForExit(5000)
      return [pscustomobject]@{
        ExitCode = -1
        StdOut = ''
        StdErr = "timed out after $TimeoutSeconds s"
      }
    }
    return [pscustomobject]@{
      ExitCode = $process.ExitCode
      StdOut = $process.StandardOutput.ReadToEnd()
      StdErr = $process.StandardError.ReadToEnd()
    }
  } finally {
    $process.Dispose()
  }
}

function Get-CapturedOutput {
  param([System.Diagnostics.Process]$Process)
  $result = [pscustomobject]@{ StdOut = ''; StdErr = '' }
  if ($null -eq $Process) { return $result }
  if (-not $Process.HasExited) { return $result }
  try {
    $result.StdOut = $Process.StandardOutput.ReadToEnd()
    $result.StdErr = $Process.StandardError.ReadToEnd()
  } catch {
    # A failed read must never hide the journal tail below.
  }
  return $result
}

function Write-IndentedLines {
  param([string]$Text, [string]$Label)
  if ($null -eq $Text) { return }
  $lines = @($Text -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 })
  if ($lines.Count -eq 0) { return }
  Write-Output "${Label}:"
  $shown = 0
  foreach ($line in $lines) {
    if ($shown -ge $TailLines) {
      Write-Output '  ...'
      break
    }
    Write-Output "  $line"
    $shown++
  }
}

function Write-JournalTail {
  if (-not (Test-Path -LiteralPath $JournalPath -PathType Leaf)) {
    Write-Output "journal: $JournalPath (not created)"
    return
  }
  Write-Output "journal tail ($JournalPath):"
  $lines = @(Get-Content -LiteralPath $JournalPath -Tail $TailLines -Encoding UTF8 -ErrorAction SilentlyContinue)
  if ($lines.Count -eq 0) {
    Write-Output '  (empty)'
    return
  }
  foreach ($line in $lines) {
    Write-Output "  $line"
  }
}

# Stops the background supervisor before reporting a failure. --stop is the
# normal path (it terminates the worker job and flushes the journal); a hard
# kill is the bounded fallback when the supervisor does not answer.
function Stop-SupervisorProcess {
  if ($null -eq $Supervisor) { return }
  if (-not $Supervisor.HasExited) {
    $stopArgs = '--stop --run-id {0} --state-dir {1}' -f $RunId, (Format-PathArgument $StatePath)
    $null = Invoke-CliCapture -FilePath $ExePath -Arguments $stopArgs -TimeoutSeconds 15
  }
  if (-not $Supervisor.WaitForExit(5000)) {
    try { $Supervisor.Kill() } catch { }
    [void]$Supervisor.WaitForExit(5000)
  }
}

function Fail-Run {
  param([string]$Message)
  Write-Output "FAIL: $Message"
  Stop-SupervisorProcess
  $captured = Get-CapturedOutput -Process $Supervisor
  Write-IndentedLines -Text $captured.StdErr -Label 'supervisor stderr'
  Write-IndentedLines -Text $captured.StdOut -Label 'supervisor stdout'
  if ($null -ne $Supervisor -and -not $Supervisor.HasExited) {
    Write-Output 'WARN: the supervisor process is still running; stop it with --stop'
  }
  Write-JournalTail
  exit 1
}

try {
  if ($HoldSeconds -lt 1) {
    Write-Output "FAIL: -HoldSeconds must be at least 1 (got $HoldSeconds)"
    exit 1
  }

  if ([string]::IsNullOrWhiteSpace($Config) -or [string]::IsNullOrWhiteSpace($StateDir) -or [string]::IsNullOrWhiteSpace($Exe)) {
    Write-Output 'FAIL: -Config, -StateDir, and -Exe must not be empty'
    exit 1
  }

  try {
    $ConfigPath = Resolve-InputPath $Config
    $StatePath = Resolve-InputPath $StateDir
    $ExePath = Resolve-InputPath $Exe
  } catch {
    Write-Output "FAIL: invalid path: $($_.Exception.Message)"
    exit 1
  }

  $PlanPath = Join-Path $StatePath 'worker-plan.json'
  $JournalPath = Join-Path $StatePath 'journal.jsonl'

  if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    Write-Output "FAIL: watchdog executable not found: $ExePath"
    exit 1
  }
  if (-not (Test-Path -LiteralPath $StatePath -PathType Container)) {
    try {
      New-Item -ItemType Directory -Path $StatePath -Force | Out-Null
    } catch {
      Write-Output "FAIL: cannot create state directory ${StatePath}: $($_.Exception.Message)"
      exit 1
    }
  }

  $StartedAt = Get-Date
  $supervisorArgs = '--supervisor --config {0} --state-dir {1} --run-id {2}' -f (Format-PathArgument $ConfigPath), (Format-PathArgument $StatePath), $RunId
  $Supervisor = Start-CapturedProcess -FilePath $ExePath -Arguments $supervisorArgs

  # Step 1: the supervisor writes the plan (and the effective config) before it
  # launches the worker, so a fresh plan means the config and deck facts
  # resolved. A plan older than this run is stale and is ignored.
  $planDeadline = (Get-Date).AddSeconds($HoldSeconds)
  $planReady = $false
  while ((Get-Date) -lt $planDeadline) {
    if (Test-Path -LiteralPath $PlanPath -PathType Leaf) {
      $plan = Get-Item -LiteralPath $PlanPath
      if ($plan.LastWriteTime -ge $StartedAt) {
        $planReady = $true
        break
      }
    }
    if ($Supervisor.HasExited) { break }
    Start-Sleep -Milliseconds 200
  }
  if (-not $planReady) {
    if ($Supervisor.HasExited) {
      Fail-Run "supervisor exited with code $($Supervisor.ExitCode) before writing worker-plan.json (config: $ConfigPath)"
    }
    Fail-Run "worker-plan.json was not written within $HoldSeconds s (config: $ConfigPath)"
  }

  # Step 2: poll the black box until a worker state record appears. --status
  # returns 1 with "no blackbox" before the supervisor has created the mapping,
  # and 0 with an empty record list until the worker appends its first record.
  $stateDeadline = (Get-Date).AddSeconds($HoldSeconds)
  $statusArgs = '--status --run-id {0} --state-dir {1}' -f $RunId, (Format-PathArgument $StatePath)
  $sawState = $false
  $stateLine = ''
  while ((Get-Date) -lt $stateDeadline) {
    if ($Supervisor.HasExited) { break }
    $status = Invoke-CliCapture -FilePath $ExePath -Arguments $statusArgs -TimeoutSeconds $HoldSeconds
    if ($status.ExitCode -eq 0) {
      foreach ($line in ($status.StdOut -split "`r?`n")) {
        if ($line -match $StateLinePattern) {
          $sawState = $true
          $stateLine = $line.Trim()
          break
        }
      }
    }
    if ($sawState) { break }
    Start-Sleep -Seconds 1
  }
  if (-not $sawState) {
    if ($Supervisor.HasExited) {
      Fail-Run "supervisor exited with code $($Supervisor.ExitCode) before reporting a worker state (config: $ConfigPath)"
    }
    Fail-Run "no worker state was reported by --status within $HoldSeconds s (config: $ConfigPath)"
  }

  # Step 3: stop the run and require a clean supervisor exit. --stop returns
  # only after the supervisor has terminated the worker and flushed the
  # journal, so its 10 s cleanup cap plus the configured hold is enough.
  $stopArgs = '--stop --run-id {0} --state-dir {1}' -f $RunId, (Format-PathArgument $StatePath)
  $stop = Invoke-CliCapture -FilePath $ExePath -Arguments $stopArgs -TimeoutSeconds ($HoldSeconds + 10)
  if ($stop.ExitCode -ne 0) {
    Fail-Run "--stop failed with exit code $($stop.ExitCode) (config: $ConfigPath)"
  }
  if (-not $Supervisor.WaitForExit($HoldSeconds * 1000)) {
    Fail-Run "supervisor did not exit within $HoldSeconds s after --stop (config: $ConfigPath)"
  }
  if ($Supervisor.ExitCode -ne 0) {
    Fail-Run "supervisor exited with code $($Supervisor.ExitCode) after --stop (config: $ConfigPath)"
  }

  $Supervisor.Dispose()
  Write-Output "state: $stateLine"
  Write-Output "OK: run $RunId reported a worker state and stopped cleanly"
  exit 0
} catch {
  Fail-Run "unexpected error: $($_.Exception.Message)"
}
