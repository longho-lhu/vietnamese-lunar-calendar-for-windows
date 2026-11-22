#Requires -Version 5.1
<#
.SYNOPSIS
  Đóng gói dist\ thành gói MSIX (dùng cho Microsoft Store).

.DESCRIPTION
  Chạy build.cmd trước. Gửi lên Store: dùng gói KHÔNG ký (Store tự ký lại),
  với -IdentityName / -Publisher / -PublisherDisplayName lấy từ Partner Center
  (Product management > Product identity).
  Thử trên máy: thêm -CertThumbprint của một chứng chỉ ký mã trong
  Cert:\CurrentUser\My có Subject trùng -Publisher.

.EXAMPLE
  .\tools\build-msix.ps1 -IdentityName 12345Name.AmLichVN -Publisher "CN=XXXXXXXX-..." -PublisherDisplayName "Tên hiển thị"
#>
param(
    [string]$IdentityName = 'AmLichVN.Test',
    [string]$Publisher = 'CN=AmLichVN Test',
    [string]$PublisherDisplayName = 'AmLichVN',
    [string]$DistDir = 'dist',
    [string]$CertThumbprint = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makeappx.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Không tìm thấy makeappx.exe (Windows SDK).' }
$makeappx = $sdk.FullName
$signtool = Join-Path $sdk.DirectoryName 'signtool.exe'

$shared = Get-Content 'src\common\shared.h' -Raw
if ($shared -notmatch '#define AMLICH_VERSION L"(\d+\.\d+\.\d+)"') { throw 'Không đọc được AMLICH_VERSION' }
$version = "$($Matches[1]).0"

foreach ($f in 'AmLichVN.exe', 'AmLichTap.dll', 'settings.default.ini') {
    if (-not (Test-Path (Join-Path $DistDir $f))) { throw "Thiếu $DistDir\$f - hãy chạy build.cmd trước." }
}

$layout = Join-Path $repoRoot 'build\msix-layout'
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $layout 'Assets') | Out-Null
foreach ($f in 'AmLichVN.exe', 'AmLichTap.dll', 'settings.default.ini') {
    Copy-Item (Join-Path $DistDir $f) $layout
}
Copy-Item 'LICENSE' $layout
Copy-Item 'packaging\msix\Assets\*.png' (Join-Path $layout 'Assets')

$manifest = [IO.File]::ReadAllText((Join-Path $repoRoot 'packaging\msix\AppxManifest.xml'))
$escape = { param($s) [Security.SecurityElement]::Escape($s) }
$manifest = $manifest.Replace('{{IDENTITY_NAME}}', (& $escape $IdentityName)).
    Replace('{{PUBLISHER}}', (& $escape $Publisher)).
    Replace('{{PUBLISHER_DISPLAY_NAME}}', (& $escape $PublisherDisplayName)).
    Replace('{{VERSION}}', $version)
[IO.File]::WriteAllText((Join-Path $layout 'AppxManifest.xml'), $manifest, (New-Object Text.UTF8Encoding($false)))

$out = Join-Path $repoRoot "$DistDir\AmLichVN-$version.msix"
& $makeappx pack /o /d $layout /p $out | Out-Host
if ($LASTEXITCODE) { throw 'makeappx thất bại' }

if ($CertThumbprint) {
    & $signtool sign /fd SHA256 /sha1 $CertThumbprint /s My $out | Out-Host
    if ($LASTEXITCODE) { throw 'signtool thất bại' }
}
Write-Host "Đã tạo $out"
