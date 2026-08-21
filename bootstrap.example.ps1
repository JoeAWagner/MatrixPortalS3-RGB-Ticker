# start-ticker-sync.ps1
#
# Logon bootstrap for the Matrix Portal calendar sync. This file deliberately
# lives OUTSIDE iCloud Drive: the Startup shortcut points here, so it is always
# present at logon even before iCloud has mounted. It then waits for the
# project folder to appear and hands off to the project's own run_sync.ps1.
#
# Credentials live next to this file (sync.config.ps1), not in the synced
# folder, so the Apple app-specific password is never uploaded to a cloud.
#
# Install: copy this file and sync.config.ps1 to a local folder such as
# C:\dev\ticker, edit $project below, then point the Startup
# shortcut here instead of at the copy inside the synced project folder.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Log beside this script, not in iCloud - a constantly rewritten log inside a
# synced folder is pointless upload churn.
$env:SYNC_LOG = Join-Path $here 'calendar_sync.log'

function Write-BootLog($msg) {
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  bootstrap  $msg" |
        Add-Content -Path $env:SYNC_LOG -Encoding utf8
}

$project = 'C:\path\to\MatrixPortalS3_Scrolling_Web_JW_10'   # <-- edit this
$runner  = Join-Path $project 'run_sync.ps1'

# Load credentials into the environment. run_sync.ps1 falls back to whatever is
# already set when it finds no sync.config.ps1 of its own, which is exactly the
# handoff we want.
$config = Join-Path $here 'sync.config.ps1'
if (Test-Path $config) {
    . $config
} else {
    Write-BootLog "ERROR: $config not found - the sync has no credentials"
    exit 1
}

# iCloud Drive may not be mounted yet at logon. Wait for it rather than failing
# silently, which is what the old in-iCloud shortcut did.
$deadline = (Get-Date).AddMinutes(20)
$waited   = $false
while (-not (Test-Path $runner)) {
    if ((Get-Date) -gt $deadline) {
        Write-BootLog "ERROR: $runner never appeared (iCloud not mounted?); giving up"
        exit 1
    }
    if (-not $waited) { Write-BootLog "waiting for iCloud project folder..."; $waited = $true }
    Start-Sleep -Seconds 15
}
if ($waited) { Write-BootLog "project folder appeared" }

& $runner
