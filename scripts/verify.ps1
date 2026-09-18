#requires -Version 5.1
<#
.SYNOPSIS
    端到端验收测试：用合成缓存文件跑完整流程并断言结果。

.DESCRIPTION
    不用真实的启动器缓存（内容会随启动器更新而变），而是生成一个合成文件，
    里面混入真实可下载的 webp/webm URL、缓存分片条目、以及被扩展名过滤项，
    然后断言：

      - 退出码符合契约（0 成功 / 1 输入缺失 / 2 参数错误）
      - URL 提取数量与内容正确
      - 只下载匹配扩展名的真实资源，且内容长度与服务器一致
      - webp 是 RIFF/WEBP、webm 是 EBML 魔数
      - 失败项不留下空文件或 .part 残片
      - all_urls.txt 行数等于提取到的 URL 数

    需要网络（会真实拉取两个约 22 MB 的米哈游静态资源）。

.EXAMPLE
    pwsh -File scripts\verify.ps1
    pwsh -File scripts\verify.ps1 -KeepWork
#>
[CmdletBinding()]
param(
    [string]$Exe,
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build\dmbg.exe' }
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "找不到 $Exe，请先运行 scripts\build.ps1"
}

$work = Join-Path $env:TEMP 'dmbg_verify'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Assert-True([bool]$condition, [string]$message) {
    $script:checks++
    if ($condition) {
        Write-Host "  [PASS] $message" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] $message" -ForegroundColor Red
        $script:failures.Add($message)
    }
}

function Assert-Equal($expected, $actual, [string]$message) {
    Assert-True ($expected -eq $actual) "$message (期望=$expected 实际=$actual)"
}

# Runs dmbg.exe and stores the result in $script:RunExit / $script:RunOutput.
# Deliberately avoids returning a value: PowerShell would happily merge any stray
# pipeline output into the returned object and turn it into an array, so the
# caller reads two plain scalars instead.
function Invoke-Dmbg([string[]]$argv) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue' # the process is expected to fail sometimes
    try {
        $script:RunOutput = (& $Exe @argv 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $previous
    }
    $script:RunExit = $LASTEXITCODE
    $script:RunOutput = [string](($script:RunOutput -replace "`e\[[0-9;]*m", ''))
}

$script:RunExit = 0
$script:RunOutput = ''

function Get-Magic([string]$path) {
    $stream = [System.IO.File]::OpenRead($path)
    try {
        $buffer = New-Object byte[] 12
        $read = $stream.Read($buffer, 0, 12)
        return , @($buffer[0..([Math]::Min($read, 12) - 1)])
    } finally {
        $stream.Close()
    }
}

if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

# --- synthetic cache file -----------------------------------------------------
# A compact stand-in for the launcher's Chromium cache: raw bytes with URLs
# embedded exactly the way the real cache stores them.
$realWebp = 'https://launcher-webstatic.mihoyo.com/launcher-public/2026/07/16/988ae3ab0ad327cfac9dcce9fc4eea1f_8664023930787869334.webp'
$realWebm = 'https://launcher-webstatic.mihoyo.com/launcher-public/2026/08/26/4cce94f51e9c284d57aca5b1e8eb4402_3502545310538625055.webm'
$cacheEntry = 'https://launcher-webstatic.mihoyo.com/launcher-public/2026/07/16/e.webm:2fba45501d8d99:5'
$pngUrl = 'https://launcher-webstatic.mihoyo.com/launcher-public/2026/08/17/deadbeef.png'
$noise = 'https://api-takumi.mihoyo.com/ptolemaios_api/api/getLatestRelease?app_id=1'

$cacheFile = Join-Path $work 'synthetic_cache_data_1'
# Every `'text' * n` is parenthesised on purpose: inside an array literal
# PowerShell parses the bare form as the ARRAY-repeat operator and then tries to
# index the array with the left-hand side, failing with a baffling
# "cannot convert System.Object[] to System.Int32".
$payload = New-Object System.Collections.Generic.List[byte]
foreach ($chunk in @(('PADDING-A' * 40), "`0", $realWebp, "`0", ('PADDING-B' * 30), "`0",
                     $realWebm, "`0", ('PADDING-C' * 20), "`0", $cacheEntry, "`0",
                     $pngUrl, "`0", $noise, "`0")) {
    foreach ($b in [System.Text.Encoding]::ASCII.GetBytes([string]$chunk)) { $payload.Add($b) }
}
[System.IO.File]::WriteAllBytes($cacheFile, $payload.ToArray())

Write-Host "== 1. 参数与错误路径 ==" -ForegroundColor Cyan
Invoke-Dmbg @('--help')
Assert-Equal 0 $RunExit '--help 退出码 0'
Assert-True ($RunOutput -match '米哈游启动器') '--help 输出中文说明'

Invoke-Dmbg @('--version')
Assert-Equal 0 $RunExit '--version 退出码 0'
Assert-True ($RunOutput -match 'dmbg \d+\.\d+\.\d+') '--version 输出语义化版本'

Invoke-Dmbg @('--nope');                  Assert-Equal 2 $RunExit '未知选项退出码 2'
Invoke-Dmbg @('-t', '0');                 Assert-Equal 2 $RunExit '线程数 0 退出码 2'
Invoke-Dmbg @('-t', 'abc');               Assert-Equal 2 $RunExit '线程数非数字退出码 2'
Invoke-Dmbg @('-t', '65');                Assert-Equal 2 $RunExit '线程数超上限退出码 2'
Invoke-Dmbg @('-o');                      Assert-Equal 2 $RunExit '-o 缺参数退出码 2'
Invoke-Dmbg @('-i', 'C:\no\such.dat', '-o', $work)
Assert-Equal 1 $RunExit '输入缺失退出码 1'

Write-Host "== 2. 提取与筛选 ==" -ForegroundColor Cyan
$outDir = Join-Path $work 'out'
Invoke-Dmbg @('-i', $cacheFile, '-o', $outDir, '-f', 'res', '-t', '4', '-ot')
Assert-Equal 0 $RunExit '正常下载退出码 0'
Assert-True ($RunOutput -match '共提取到 5 个唯一 URL') '提取到 5 个唯一 URL'
Assert-True ($RunOutput -match '跳过 1 个缓存分片条目') '识别出 1 个缓存分片条目'
Assert-True ($RunOutput -match '开始下载 2 个文件') '只下载 2 个匹配扩展名的资源'
Assert-True ($RunOutput -match '成功 2 失败 0') '2 个全部成功'

$resultDir = Join-Path $outDir 'res'
# all_urls.txt is written by -ot into the same folder; it is not a download.
$files = @(Get-ChildItem -LiteralPath $resultDir -File |
    Where-Object { $_.Name -ne 'all_urls.txt' } | Sort-Object Name)
Assert-Equal 2 $files.Count '产出 2 个下载文件'
Assert-Equal 0 @($files | Where-Object { $_.Length -eq 0 }).Count '没有空文件'
Assert-Equal 0 @(Get-ChildItem -LiteralPath $resultDir -File -Filter '*.part').Count '没有 .part 残片'

$webp = @($files | Where-Object { $_.Extension -eq '.webp' })
$webm = @($files | Where-Object { $_.Extension -eq '.webm' })
Assert-Equal 1 $webp.Count '有 1 个 webp'
Assert-Equal 1 $webm.Count '有 1 个 webm'

Write-Host "== 3. 内容完整性 ==" -ForegroundColor Cyan
$webpMagic = Get-Magic $webp[0].FullName
Assert-True (($webpMagic[0..3] -join ',') -eq '82,73,70,70') 'webp 魔数是 RIFF'
Assert-True (($webpMagic[8..11] -join ',') -eq '87,69,66,80') 'webp 魔数是 WEBP'
$webmMagic = Get-Magic $webm[0].FullName
Assert-True (($webmMagic[0..3] -join ',') -eq '26,69,223,163') 'webm 魔数是 EBML'

# Cross-check the byte counts against the server's Content-Length.
$expected = @{}
foreach ($url in @($realWebp, $realWebm)) {
    $name = [System.IO.Path]::GetFileName(([Uri]$url).AbsolutePath)
    $lengthHeader = (Invoke-WebRequest -Uri $url -Method Head -UseBasicParsing).Headers['Content-Length']
    $expected[$name] = [int64]("$lengthHeader")
}
foreach ($file in $files) {
    Assert-True ($expected.ContainsKey($file.Name)) "文件名与 URL 一致: $($file.Name)"
    if ($expected.ContainsKey($file.Name)) {
        Assert-Equal $expected[$file.Name] $file.Length "字节数与服务器一致: $($file.Name)"
    }
}

Write-Host "== 4. all_urls.txt ==" -ForegroundColor Cyan
$listFile = Join-Path $resultDir 'all_urls.txt'
Assert-True (Test-Path -LiteralPath $listFile) 'all_urls.txt 已生成'
Assert-Equal 5 @(Get-Content -LiteralPath $listFile).Count 'all_urls.txt 有 5 行'

Write-Host "== 5. 输出目录行为 ==" -ForegroundColor Cyan
$nested = Join-Path $work 'a\b\c'
Invoke-Dmbg @('-i', $cacheFile, '-o', $nested, '-f', 'x', '--ext', 'webp')
Assert-Equal 0 $RunExit '多级输出目录自动创建'
Assert-Equal 1 @(Get-ChildItem -LiteralPath (Join-Path $nested 'x') -File).Count '仅下载 webp'

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "全部通过：$checks 项断言" -ForegroundColor Green
} else {
    Write-Host "$($failures.Count)/$checks 项断言失败：" -ForegroundColor Red
    foreach ($failure in $failures) { Write-Host "  - $failure" -ForegroundColor Red }
}

if ($KeepWork) {
    Write-Host "工作目录保留在: $work" -ForegroundColor Yellow
} else {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

$verificationExitCode = 0
if ($failures.Count -gt 0) { $verificationExitCode = 1 }
exit $verificationExitCode
