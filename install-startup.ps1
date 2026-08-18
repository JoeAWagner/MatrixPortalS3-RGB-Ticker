# install-startup.ps1
# Auto-starts the calendar sync at logon via a shortcut in your Startup folder.
# Needs NO admin rights (unlike a Scheduled Task). The sync runs hidden.
#
#   Install:   powershell -ExecutionPolicy Bypass -File install-startup.ps1
#   Remove:    powershell -ExecutionPolicy Bypass -File install-startup.ps1 -Uninstall

param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$startup = [Environment]::GetFolderPath('Startup')
$lnk     = Join-Path $startup 'MatrixPortalCalendarSync.lnk'

if ($Uninstall) {
    if (Test-Path $lnk) { Remove-Item $lnk -Force; Write-Host "Removed $lnk" }
    else { Write-Host "No startup shortcut found." }
    return
}

if (-not (Test-Path (Join-Path $here 'sync.config.ps1'))) {
    Write-Warning "sync.config.ps1 not found. Copy sync.config.example.ps1 to sync.config.ps1 and fill it in before this will do anything."
}

$runner = Join-Path $here 'run_sync.ps1'
$ws = New-Object -ComObject WScript.Shell
$sc = $ws.CreateShortcut($lnk)
$sc.TargetPath       = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$sc.Arguments        = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$runner`""
$sc.WorkingDirectory = $here
$sc.WindowStyle      = 7           # minimized (PowerShell also hides itself)
$sc.Description       = 'Matrix Portal calendar sync (runs at logon)'
$sc.Save()

Write-Host "Installed startup shortcut:"
Write-Host "  $lnk"
Write-Host "It will start automatically at your next logon."
Write-Host "To start it right now:  powershell -ExecutionPolicy Bypass -File `"$runner`""
