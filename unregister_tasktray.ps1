# unregister_tasktray.ps1
# 管理者権限で実行してください
# 例:
#   powershell -ExecutionPolicy Bypass -File .\unregister_tasktray.ps1 -TaskName "HayateKomorebiTaskTray"

param(
  [string]$TaskName = "HayateKomorebiTaskTray"
)

function Require-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run as Administrator."
  }
}

Require-Admin

$existing = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if (-not $existing) {
  Write-Host "Task not found: $TaskName"
  exit 0
}

Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
Write-Host "Unregistered scheduled task: $TaskName"