#Requires -Version 5.1
<#
.SYNOPSIS
  Gỡ cài đặt Âm lịch Việt Nam cho lịch Taskbar.
.PARAMETER KeepData
  Giữ lại thư mục C:\ProgramData\AmLichVN (settings.ini, log).
#>
param([switch]$KeepData)

$ErrorActionPreference = 'Continue'

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($KeepData) { $argsList += '-KeepData' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argsList
    exit
}

$InstallDir = Join-Path $env:ProgramFiles 'AmLichVN'
$DataDir    = Join-Path $env:ProgramData 'AmLichVN'

Write-Host '1/5 Tắt AmLichVN...'
Get-Process AmLichVN -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name AmLichVN -ErrorAction SilentlyContinue
Remove-Item 'HKCU:\Software\AmLichVN' -Recurse -ErrorAction SilentlyContinue

Write-Host '2/5 Khởi động lại tiến trình bảng lịch để gỡ DLL khỏi bộ nhớ...'
Get-Process ShellHost, ShellExperienceHost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

Write-Host "3/5 Xoá $InstallDir ..."
for ($i = 0; $i -lt 10 -and (Test-Path $InstallDir); $i++) {
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $InstallDir) { Start-Sleep -Milliseconds 700 }
}
if (Test-Path $InstallDir) {
    Write-Warning "Chưa xoá được $InstallDir (file còn bị giữ). Hãy đăng xuất/đăng nhập lại rồi xoá tay thư mục này."
}

Write-Host '4/5 Trả lại cài đặt XAML...'
$marker = Get-ItemProperty 'HKLM:\Software\AmLichVN' -Name SetCompositionDiag -ErrorAction SilentlyContinue
if ($marker) {
    Remove-ItemProperty 'HKLM:\Software\Microsoft\XAML\Debug' -Name DisableCompositionDiag -ErrorAction SilentlyContinue
}
Remove-Item 'HKLM:\Software\AmLichVN' -Recurse -ErrorAction SilentlyContinue

if ($KeepData) {
    Write-Host "5/5 Giữ lại $DataDir"
} else {
    Write-Host "5/5 Xoá $DataDir ..."
    Remove-Item $DataDir -Recurse -Force -ErrorAction SilentlyContinue
}

# Nếu bảng thông báo/lịch chưa tự chạy lại thì khởi động lại Explorer.
if (-not (Get-Process ShellHost, ShellExperienceHost -ErrorAction SilentlyContinue)) {
    Write-Host 'Khởi động lại Explorer để bảng lịch chạy lại...'
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'Đã gỡ cài đặt.' -ForegroundColor Green
Read-Host 'Nhấn Enter để đóng'
