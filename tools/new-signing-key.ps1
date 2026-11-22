#Requires -Version 5.1
<#
.SYNOPSIS
  Tạo cặp khóa ký số (ECDSA P-256) cho bản cập nhật của AmLichVN.

.DESCRIPTION
  - Khóa RIÊNG (dùng để ký) được ghi ra file ngoài repo, mặc định
    %USERPROFILE%\.amlichvn-signing\signing-key.txt (dạng base64), chỉ người
    dùng hiện tại đọc được. Dán nội dung file này vào GitHub:
    Settings > Secrets and variables > Actions > New repository secret,
    tên AMLICH_SIGNING_KEY. Hãy sao lưu file này ở nơi an toàn (USB, trình
    quản lý mật khẩu). KHÔNG đưa nó vào repo.
  - Khóa CÔNG KHAI (dùng để kiểm tra) được ghi vào src\common\update_pubkey.h và
    biên dịch vào AmLichVN.exe.

  Chạy lại với -Force sẽ tạo khóa MỚI: các bản đã cài sẽ không nhận bản cập nhật
  ký bằng khóa mới nữa, phải cài tay một lần.
#>
param(
    [string]$KeyFile = (Join-Path $env:USERPROFILE '.amlichvn-signing\signing-key.txt'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$header = Join-Path $repoRoot 'src\common\update_pubkey.h'

if ((Test-Path $KeyFile) -and -not $Force) {
    throw "Đã có khóa tại '$KeyFile'. Dùng -Force nếu thật sự muốn tạo khóa mới."
}

$params = New-Object System.Security.Cryptography.CngKeyCreationParameters
$params.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
$key = [System.Security.Cryptography.CngKey]::Create(
    [System.Security.Cryptography.CngAlgorithm]::ECDsaP256, $null, $params)
$priv = $key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$pub = $key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
$key.Dispose()

# --- Khóa riêng ------------------------------------------------------------
$dir = Split-Path -Parent $KeyFile
New-Item -ItemType Directory -Force -Path $dir | Out-Null
[IO.File]::WriteAllText($KeyFile, [Convert]::ToBase64String($priv))
# Chỉ người dùng hiện tại (và SYSTEM) được đọc.
icacls $dir /inheritance:r /grant:r "${env:USERNAME}:(OI)(CI)F" '*S-1-5-18:(OI)(CI)F' | Out-Null
icacls $KeyFile /inheritance:r /grant:r "${env:USERNAME}:F" '*S-1-5-18:F' | Out-Null

# --- Khóa công khai --------------------------------------------------------
$hex = ($pub | ForEach-Object { '0x{0:X2}' -f $_ })
$lines = for ($i = 0; $i -lt $hex.Count; $i += 12) {
    '    ' + (($hex[$i..([Math]::Min($i + 11, $hex.Count - 1))]) -join ', ') + ','
}
$content = @"
#pragma once

// Khóa công khai ECDSA P-256 (định dạng BCRYPT_ECCPUBLIC_BLOB) dùng để kiểm tra
// chữ ký của bản cập nhật. Tạo bởi tools\new-signing-key.ps1.
// Khóa riêng tương ứng nằm trong GitHub Secret AMLICH_SIGNING_KEY.

static const unsigned char kUpdatePublicKey[$($pub.Length)] = {
$($lines -join "`n")
};
"@
[IO.File]::WriteAllText($header, $content.Replace("`r`n", "`n") + "`n")

$sha = [Security.Cryptography.SHA256]::Create()
$fp = ($sha.ComputeHash($pub)[0..7] | ForEach-Object { $_.ToString('x2') }) -join ''
Write-Host "Đã tạo khóa. Dấu vân tay khóa công khai: $fp"
Write-Host "Khóa riêng : $KeyFile  (SAO LƯU file này, không đưa vào repo)"
Write-Host "Khóa công khai: $header"
