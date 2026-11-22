#Requires -Version 5.1
<#
.SYNOPSIS
  Cài đặt (hoặc cập nhật) Âm lịch Việt Nam cho lịch Taskbar.

.DESCRIPTION
  - Chép AmLichVN.exe + AmLichTap.dll vào "C:\Program Files\AmLichVN"
  - Tạo thư mục dữ liệu "C:\ProgramData\AmLichVN" (cài đặt, trạng thái, log)
    và cấp quyền để tiến trình bảng lịch của Windows ghi được trạng thái.
  - Bật HKLM\Software\Microsoft\XAML\Debug\DisableCompositionDiag = 1 để tránh
    lỗi khi cơ chế chẩn đoán XAML chạy trên nhiều luồng giao diện.
  - Khởi động AmLichVN.exe (lần chạy đầu tự bật "Khởi động cùng Windows").

  Chạy lại script này để cập nhật lên bản mới; settings.ini cũ được giữ nguyên.
#>
param(
    # Thư mục chứa AmLichVN.exe, AmLichTap.dll, settings.default.ini
    [string]$Source = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

# --- Tự yêu cầu quyền Administrator ---------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Cần quyền Administrator, đang mở lại script...'
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', "`"$PSCommandPath`"", '-Source', "`"$Source`"")
    exit
}

$InstallDir = Join-Path $env:ProgramFiles 'AmLichVN'
$DataDir    = Join-Path $env:ProgramData 'AmLichVN'
$Files      = 'AmLichVN.exe', 'AmLichTap.dll'

foreach ($f in $Files) {
    if (-not (Test-Path (Join-Path $Source $f))) {
        throw "Không thấy $f trong '$Source'. Hãy chạy build.cmd trước rồi chạy script trong thư mục dist."
    }
}

Write-Host '1/6 Dừng AmLichVN đang chạy (nếu có)...'
Get-Process AmLichVN -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# Nếu đang cập nhật, AmLichTap.dll cũ đang bị tiến trình shell giữ: khởi động
# lại tiến trình shell để nhả file (Windows tự chạy lại chúng).
$oldDll = Join-Path $InstallDir 'AmLichTap.dll'
if (Test-Path $oldDll) {
    Write-Host '2/6 Khởi động lại tiến trình bảng lịch để nhả DLL cũ...'
    Get-Process ShellHost, ShellExperienceHost -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
} else {
    Write-Host '2/6 Cài mới.'
}

Write-Host "3/6 Chép file vào $InstallDir ..."
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
foreach ($f in $Files) {
    $dst = Join-Path $InstallDir $f
    for ($i = 0; $i -lt 10; $i++) {
        try {
            Copy-Item (Join-Path $Source $f) $dst -Force
            break
        } catch {
            if ($i -eq 9) { throw "Không chép được $f (file đang bị dùng?): $_" }
            Start-Sleep -Milliseconds 500
        }
    }
    Unblock-File $dst -ErrorAction SilentlyContinue
}
foreach ($s in 'install.ps1', 'uninstall.ps1', 'check.ps1', 'KiemTra.cmd', 'GoCaiDat.cmd') {
    $p = Join-Path $Source $s
    if (Test-Path $p) { Copy-Item $p $InstallDir -Force }
}
# Tiến trình bảng lịch (có thể chạy trong AppContainer) phải đọc được DLL.
icacls $InstallDir /grant '*S-1-15-2-1:(OI)(CI)RX' | Out-Null

Write-Host "4/6 Tạo thư mục dữ liệu $DataDir ..."
New-Item -ItemType Directory -Force -Path (Join-Path $DataDir 'status') | Out-Null
# Users + ALL APPLICATION PACKAGES được ghi (DLL trong shell ghi file trạng thái).
icacls $DataDir /grant '*S-1-5-32-545:(OI)(CI)M' '*S-1-15-2-1:(OI)(CI)M' /T | Out-Null

$settings = Join-Path $DataDir 'settings.ini'
if (-not (Test-Path $settings)) {
    # UTF-16 để Windows (GetPrivateProfileString) và Notepad đọc đúng tiếng Việt.
    Get-Content (Join-Path $Source 'settings.default.ini') -Encoding UTF8 |
        Set-Content $settings -Encoding Unicode
    Write-Host '    Đã tạo settings.ini mặc định.'
} else {
    Write-Host '    Giữ nguyên settings.ini hiện có.'
}

Write-Host '5/6 Bật DisableCompositionDiag cho cơ chế chẩn đoán XAML...'
$xamlKey = 'HKLM:\Software\Microsoft\XAML\Debug'
$hadValue = $false
if (Test-Path $xamlKey) {
    $hadValue = $null -ne (Get-ItemProperty $xamlKey -Name DisableCompositionDiag -ErrorAction SilentlyContinue)
}
New-Item -Path $xamlKey -Force | Out-Null
New-ItemProperty -Path $xamlKey -Name DisableCompositionDiag -Value 1 -PropertyType DWord -Force | Out-Null
if (-not $hadValue) {
    # Đánh dấu để gỡ cài đặt biết là do mình đặt.
    New-Item -Path 'HKLM:\Software\AmLichVN' -Force | Out-Null
    New-ItemProperty -Path 'HKLM:\Software\AmLichVN' -Name SetCompositionDiag -Value 1 -PropertyType DWord -Force | Out-Null
}

Write-Host '6/6 Khởi động AmLichVN (không dùng quyền Administrator)...'
# Mở qua explorer.exe để chương trình chạy với quyền người dùng thường.
Start-Process explorer.exe -ArgumentList "`"$(Join-Path $InstallDir 'AmLichVN.exe')`""

Write-Host ''
Write-Host 'XONG! Bây giờ hãy bấm vào đồng hồ ở góc phải taskbar để mở lịch.' -ForegroundColor Green
Write-Host 'Sau khoảng 5-10 giây, chạy KiemTra.cmd (hoặc check.ps1) để xem trạng thái.'
Write-Host "Cài đặt hiển thị: $settings"
Write-Host ''
Read-Host 'Nhấn Enter để đóng'
