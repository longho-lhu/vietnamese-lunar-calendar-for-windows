#Requires -Version 5.1
<#
.SYNOPSIS
  Kiểm tra Âm lịch Việt Nam cho lịch Taskbar có đang hoạt động không.
  Dùng sau mỗi lần Windows cập nhật, hoặc khi thấy âm lịch biến mất.
#>

$InstallDir = Join-Path $env:ProgramFiles 'AmLichVN'
$DataDir    = Join-Path $env:ProgramData 'AmLichVN'

function Line($label, $value, $color = 'Gray') {
    Write-Host ("{0,-28}" -f $label) -NoNewline
    Write-Host $value -ForegroundColor $color
}

$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$build = "$($cv.CurrentBuild).$($cv.UBR)"
$app = Get-ItemProperty 'HKCU:\Software\AmLichVN' -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '=== KIỂM TRA ÂM LỊCH VIỆT NAM ===' -ForegroundColor Cyan
Line 'Windows:' "$($cv.DisplayVersion) - build $build"
Line 'Build đã chạy tốt gần nhất:' $(if ($app.LastGoodBuild) { $app.LastGoodBuild } else { '(chưa có)' })

$problems = @()

# 1. File cài đặt
foreach ($f in 'AmLichVN.exe', 'AmLichTap.dll') {
    if (Test-Path (Join-Path $InstallDir $f)) {
        Line "File ${f}:" 'có' 'Green'
    } else {
        Line "File ${f}:" 'THIẾU' 'Red'
        $problems += "Thiếu $f - hãy chạy lại CaiDat.cmd"
    }
}

# 2. Ứng dụng nền
$proc = Get-Process AmLichVN -ErrorAction SilentlyContinue
if ($proc) { Line 'AmLichVN.exe đang chạy:' "có (pid $($proc.Id -join ', '))" 'Green' }
else {
    Line 'AmLichVN.exe đang chạy:' 'KHÔNG' 'Red'
    $problems += "AmLichVN.exe không chạy - mở '$InstallDir\AmLichVN.exe'"
}
$run = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name AmLichVN -ErrorAction SilentlyContinue
Line 'Khởi động cùng Windows:' $(if ($run) { 'bật' } else { 'tắt' })

$diag = Get-ItemProperty 'HKLM:\Software\Microsoft\XAML\Debug' -Name DisableCompositionDiag -ErrorAction SilentlyContinue
Line 'DisableCompositionDiag:' $(if ($diag.DisableCompositionDiag -eq 1) { '1 (đúng)' } else { 'chưa đặt' }) $(if ($diag.DisableCompositionDiag -eq 1) { 'Green' } else { 'Yellow' })

# 3. Trạng thái do DLL ghi trong tiến trình bảng lịch
$overall = 'PENDING'
$statusFiles = Get-ChildItem (Join-Path $DataDir 'status') -Filter 'status-*.txt' -ErrorAction SilentlyContinue
foreach ($sf in $statusFiles) {
    $kv = @{}
    Get-Content $sf.FullName -Encoding UTF8 | ForEach-Object {
        $i = $_.IndexOf('=')
        if ($i -gt 0) { $kv[$_.Substring(0, $i)] = $_.Substring($i + 1) }
    }
    $alive = $false
    if ($kv.pid) { $alive = [bool](Get-Process -Id ([int]$kv.pid) -ErrorAction SilentlyContinue) }
    $fresh = ($kv.build -eq $build) -and $alive

    Write-Host ''
    Write-Host "[$($kv.process)]" -ForegroundColor Cyan
    $color = switch ($kv.state) { 'OK' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Line '  Trạng thái:' "$($kv.state) - $($kv.detail)" $color
    Line '  Cập nhật lúc:' "$($kv.time) (build $($kv.build), pid $($kv.pid)$(if (-not $alive) { ', tiến trình đã tắt' }))"
    Line '  Bảng lịch / ô ngày:' "$($kv.calendarViews) / $($kv.dayItems)"
    if ([int]$kv.errors -gt 0) { Line '  Lỗi:' "$($kv.errors) - lỗi cuối: $($kv.lastError)" 'Yellow' }
    if ($kv.calendarLikeTypes) { Line '  Kiểu giao diện "Calendar":' $kv.calendarLikeTypes 'DarkGray' }
    if (-not $fresh) { Line '  Ghi chú:' 'file cũ (build khác hoặc tiến trình đã tắt)' 'DarkGray'; continue }

    if ($kv.state -eq 'OK') { $overall = 'OK' }
    elseif ($kv.state -eq 'FAIL' -and $overall -ne 'OK') {
        $overall = 'FAIL'
        $problems += "$($kv.process): $($kv.detail)"
    }
}

if (-not $statusFiles) {
    $problems += 'Chưa có file trạng thái: DLL chưa được nạp vào bảng lịch lần nào.'
}

Write-Host ''
switch ($overall) {
    'OK' { Write-Host 'KẾT LUẬN: Âm lịch ĐANG HOẠT ĐỘNG.' -ForegroundColor Green }
    'FAIL' { Write-Host 'KẾT LUẬN: Âm lịch KHÔNG hoạt động.' -ForegroundColor Red }
    default { Write-Host 'KẾT LUẬN: Chưa xác minh - hãy bấm vào đồng hồ để mở lịch, đợi 5 giây rồi chạy lại.' -ForegroundColor Yellow }
}
foreach ($p in $problems) { Write-Host " - $p" -ForegroundColor Yellow }

$log = Join-Path $DataDir 'app-log.txt'
if (Test-Path $log) {
    Write-Host ''
    Write-Host '--- 10 dòng log cuối của AmLichVN.exe ---' -ForegroundColor DarkGray
    Get-Content $log -Encoding UTF8 -Tail 10 | ForEach-Object { Write-Host $_ -ForegroundColor DarkGray }
}
Write-Host ''
if ($Host.Name -eq 'ConsoleHost') { Read-Host 'Nhấn Enter để đóng' | Out-Null }
