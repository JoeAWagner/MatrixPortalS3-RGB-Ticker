# run_sync.ps1
# Launches the calendar sync with credentials from the local sync.config.ps1.
# install-task.ps1 registers this to run at logon; you can also run it by hand.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

# Load local credentials (gitignored). Any env vars already set still apply.
$config = Join-Path $here 'sync.config.ps1'
if (Test-Path $config) {
    . $config
} else {
    Write-Warning "sync.config.ps1 not found — relying on existing environment variables."
}

# Send the sync's own logging to a rotating file alongside this script.
$env:SYNC_LOG = Join-Path $here 'calendar_sync.log'

# Locate a Python interpreter (python.exe, then the py launcher).
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }
if (-not $py) {
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  launcher  ERROR: Python not found on PATH" |
        Add-Content -Path $env:SYNC_LOG -Encoding utf8
    exit 1
}

& $py (Join-Path $here 'calendar_sync.py')
