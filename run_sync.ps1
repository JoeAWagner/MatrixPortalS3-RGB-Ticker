# run_sync.ps1
# Launches the calendar sync with credentials from the local sync.config.ps1.
# install-task.ps1 registers this to run at logon; you can also run it by hand.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

# Single-instance guard: if a copy is already running, exit quietly.
$mutex = New-Object System.Threading.Mutex($false, 'Local\MatrixPortalCalendarSync')
$owned = $false
try { $owned = $mutex.WaitOne(0) }
catch [System.Threading.AbandonedMutexException] { $owned = $true }
if (-not $owned) { exit 0 }

# Load local credentials (gitignored). Any env vars already set still apply.
$config = Join-Path $here 'sync.config.ps1'
if (Test-Path $config) {
    . $config
} else {
    Write-Warning "sync.config.ps1 not found - relying on existing environment variables."
}

# Send the sync's own logging to a rotating file alongside this script.
$env:SYNC_LOG = Join-Path $here 'calendar_sync.log'

function Write-LauncherLog($msg) {
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  launcher  $msg" |
        Add-Content -Path $env:SYNC_LOG -Encoding utf8
}

# Find a Python that can actually run the sync.
#
# "python" on PATH is NOT trustworthy on Windows: if a real install is removed
# or drops off PATH, the Microsoft Store alias at
# ...\AppData\Local\Microsoft\WindowsApps\python.exe silently takes over.
# It is a stub that opens the Store and exits, so the sync appears to launch
# and then does nothing at all. Skip that path, and prove each candidate by
# importing the modules the sync actually needs.
function Resolve-Python {
    $candidates = @()

    # The py launcher resolves real installs properly.
    $pyLauncher = (Get-Command py -ErrorAction SilentlyContinue).Source
    if ($pyLauncher) {
        try {
            $found = & $pyLauncher -3 -c "import sys; print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and $found) { $candidates += $found.Trim() }
        } catch {}
    }

    # Common per-user install locations.
    foreach ($v in @('Python314','Python313','Python312','Python311')) {
        $candidates += "$env:LOCALAPPDATA\Programs\Python\$v\python.exe"
    }

    # Whatever is on PATH, unless it is the Store alias.
    $onPath = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($onPath -and $onPath -notlike '*\WindowsApps\*') { $candidates += $onPath }

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        if (-not (Test-Path $c)) { continue }
        if ($c -like '*\WindowsApps\*') { continue }
        & $c -c "import caldav, requests" 2>$null
        if ($LASTEXITCODE -eq 0) { return $c }
    }
    return $null
}

$py = Resolve-Python
if (-not $py) {
    Write-LauncherLog "ERROR: no Python with 'caldav' and 'requests' installed."
    Write-LauncherLog "  Install them with:  py -3 -m pip install caldav requests"
    exit 1
}
Write-LauncherLog "using $py"

& $py (Join-Path $here 'calendar_sync.py')
