#Requires -Version 5.1
<#
.SYNOPSIS
  Phát hành phiên bản mới: tăng AMLICH_VERSION, commit, tạo tag vX.Y.Z và đẩy
  lên GitHub. GitHub Actions sẽ build, ký số và tạo Release; các máy đã cài sẽ
  được hỏi cập nhật.

.EXAMPLE
  .\tools\release.ps1 -Version 1.2.0 -Notes "Sửa lỗi hiển thị tháng nhuận"
#>
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$Notes = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if (git status --porcelain) { throw 'Còn thay đổi chưa commit. Hãy commit hoặc stash trước.' }

$sharedPath = Join-Path $repoRoot 'src\common\shared.h'
$shared = [IO.File]::ReadAllText($sharedPath)
$updated = [regex]::Replace($shared, '#define AMLICH_VERSION L"[^"]+"', "#define AMLICH_VERSION L`"$Version`"")
if ($updated -ne $shared) {
    [IO.File]::WriteAllText($sharedPath, $updated)
    git add $sharedPath
    git commit -m "Phát hành v$Version"
    if ($LASTEXITCODE) { throw 'git commit thất bại' }
}

$message = if ($Notes) { $Notes } else { "AmLichVN v$Version" }
git tag -a "v$Version" -m $message
if ($LASTEXITCODE) { throw 'git tag thất bại' }
git push origin HEAD
git push origin "v$Version"
if ($LASTEXITCODE) { throw 'git push thất bại' }
Write-Host "Đã đẩy tag v$Version. Theo dõi tại tab Actions của repo."
