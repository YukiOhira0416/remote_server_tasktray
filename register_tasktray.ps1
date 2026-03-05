# register_tasktray.ps1
# 管理者権限で実行してください
# 例:
#   powershell -ExecutionPolicy Bypass -File .\register_tasktray.ps1 -ExePath "C:\Program Files\HayateKomorebi\TaskTray\remote_server_tasktray.exe"

param(
  [Parameter(Mandatory=$true)][string]$ExePath,
  [string]$Arguments = "",
  [string]$TaskName = "HayateKomorebiTaskTray",
  [switch]$StartNow
)

function Require-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run as Administrator."
  }
}

Require-Admin

if (-not (Test-Path -LiteralPath $ExePath)) {
  throw "ExePath not found: $ExePath"
}

$workDir = Split-Path -Parent $ExePath

# Create scheduled task (At logon, run highest, interactive user)
$actionParams = @{ Execute = $ExePath; WorkingDirectory = $workDir }
if ($Arguments -ne "") { $actionParams['Argument'] = $Arguments }
$action    = New-ScheduledTaskAction @actionParams
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew

$task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description "HayateKomorebi TaskTray auto-start"

# Overwrite if already exists
Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null

Write-Host "Registered scheduled task: $TaskName"

if ($StartNow) {
  Start-ScheduledTask -TaskName $TaskName
  Write-Host "Started scheduled task: $TaskName"
}