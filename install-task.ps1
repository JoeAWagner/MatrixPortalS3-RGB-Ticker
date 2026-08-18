# install-task.ps1
# Registers a Windows Scheduled Task that runs the calendar sync at logon and
# restarts it automatically if it stops. No admin rights needed (per-user task).
#
#   Install:    powershell -ExecutionPolicy Bypass -File install-task.ps1
#   Remove:     powershell -ExecutionPolicy Bypass -File install-task.ps1 -Uninstall
#   Start now:  Start-ScheduledTask -TaskName MatrixPortalCalendarSync

param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$TaskName = 'MatrixPortalCalendarSync'
$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$runner   = Join-Path $here 'run_sync.ps1'

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Removed scheduled task '$TaskName'."
    return
}

if (-not (Test-Path (Join-Path $here 'sync.config.ps1'))) {
    Write-Warning "sync.config.ps1 not found. Copy sync.config.example.ps1 to sync.config.ps1 and fill it in before the sync can run."
}

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$runner`""

$trigger = New-ScheduledTaskTrigger -AtLogOn

# Run forever, restart on failure, never spawn a second copy.
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit ([TimeSpan]::Zero)

$principal = New-ScheduledTaskPrincipal `
    -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName `
    -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
    -Description 'Syncs Apple Calendar Now/Up Next to the Matrix Portal LED ticker.' `
    -Force | Out-Null

Write-Host "Registered scheduled task '$TaskName' (runs at logon, restarts on failure)."
Write-Host "Start it now with:  Start-ScheduledTask -TaskName $TaskName"
Write-Host "Logs:               $here\calendar_sync.log"
