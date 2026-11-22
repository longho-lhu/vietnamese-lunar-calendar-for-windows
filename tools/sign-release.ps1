#Requires -Version 5.1
<#
.SYNOPSIS
  Tạo manifest + chữ ký cho một bản phát hành (chạy trong GitHub Actions).

.DESCRIPTION
  Đọc khóa riêng từ biến môi trường AMLICH_SIGNING_KEY, tạo trong -OutDir:
    manifest.txt   phiên bản, tag, kích thước + SHA-256 của từng file
    manifest.sig   chữ ký ECDSA P-256 / SHA-256 (64 byte r||s) của manifest.txt
    AmLichVN.exe, AmLichTap.dll   (bản sao từ -DistDir)
    AmLichVN-<phiên bản>.zip      toàn bộ thư mục dist để cài tay
    notes.md       ghi chú phát hành

  Script dừng lại nếu phiên bản trong tag khác AMLICH_VERSION trong shared.h,
  hoặc nếu khóa riêng không khớp khóa công khai trong update_pubkey.h.
#>
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$DistDir = 'dist',
    [string]$OutDir = 'release',
    [string]$Notes = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# --- Phiên bản ---------------------------------------------------------------
if ($Tag -notmatch '^v(\d+\.\d+\.\d+)$') { throw "Tag '$Tag' phải có dạng vX.Y.Z" }
$version = $Matches[1]
$shared = Get-Content (Join-Path $repoRoot 'src\common\shared.h') -Raw
if ($shared -notmatch '#define AMLICH_VERSION L"([^"]+)"') { throw 'Không đọc được AMLICH_VERSION' }
if ($Matches[1] -ne $version) {
    throw "Tag $Tag không khớp AMLICH_VERSION=$($Matches[1]) trong shared.h"
}

# --- Khóa --------------------------------------------------------------------
$keyB64 = $env:AMLICH_SIGNING_KEY
if ([string]::IsNullOrWhiteSpace($keyB64)) { throw 'Chưa có secret AMLICH_SIGNING_KEY' }
$key = [System.Security.Cryptography.CngKey]::Import(
    [Convert]::FromBase64String($keyB64.Trim()),
    [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$ecdsa = New-Object System.Security.Cryptography.ECDsaCng($key)

$pub = $key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
$hdr = Get-Content (Join-Path $repoRoot 'src\common\update_pubkey.h') -Raw
$embedded = [regex]::Matches($hdr, '0x([0-9A-Fa-f]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) }
if ((@($embedded) -join ',') -ne (@($pub) -join ',')) {
    throw 'Khóa riêng (AMLICH_SIGNING_KEY) không khớp khóa công khai trong update_pubkey.h'
}

# --- Manifest ----------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$sha = [Security.Cryptography.SHA256]::Create()
$lines = @('amlich-update-manifest=1', "version=$version", "tag=$Tag")
foreach ($f in 'AmLichVN.exe', 'AmLichTap.dll') {
    $src = Join-Path $DistDir $f
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $src))
    $hash = ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join ''
    $lines += "file=$f;$($bytes.Length);$hash"
    Copy-Item $src (Join-Path $OutDir $f) -Force
}
$escaped = $Notes.Trim().Replace('\', '\\').Replace("`r`n", "`n").Replace("`n", '\n')
if ($escaped) { $lines += "notes=$escaped" }

$manifestBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($lines -join "`n") + "`n")
$sig = $ecdsa.SignData($manifestBytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
if ($sig.Length -ne 64) { throw "Chữ ký có độ dài lạ: $($sig.Length)" }
if (-not $ecdsa.VerifyData($manifestBytes, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) {
    throw 'Tự kiểm tra chữ ký thất bại'
}

$outFull = (Resolve-Path $OutDir).Path
[IO.File]::WriteAllBytes((Join-Path $outFull 'manifest.txt'), $manifestBytes)
[IO.File]::WriteAllBytes((Join-Path $outFull 'manifest.sig'), $sig)

$zip = Join-Path $outFull "AmLichVN-$version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $zip

$body = if ($Notes.Trim()) { $Notes.Trim() } else { "AmLichVN $version" }
$body += "`n`n---`nCài mới: tải ``AmLichVN-$version.zip``, giải nén, chạy ``CaiDat.cmd``.`n" +
         "Đã cài bản có cập nhật tự động: ứng dụng sẽ tự hỏi khi có bản này.`n" +
         'Các file `manifest.txt` / `manifest.sig` là manifest đã ký số mà ứng dụng dùng để kiểm tra.'
[IO.File]::WriteAllText((Join-Path $outFull 'notes.md'), $body)

Write-Host "Đã ký manifest cho $Tag"
Get-Content (Join-Path $outFull 'manifest.txt')
