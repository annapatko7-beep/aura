# Selfvoy Windows (.exe) Release Build Script
param(
    [string]$ServerUrl = "selfvoy://standalone"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir
$WebDir = Join-Path $RootDir "web"
$ReleaseDir = Join-Path $RootDir "release"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Building Selfvoy Windows Release" -ForegroundColor Cyan
Write-Host " Production WebSocket Server: $ServerUrl" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

$env:VITE_SELFVOY_SERVER_URL = $ServerUrl
$env:NODE_ENV = "production"

Set-Location $WebDir

Write-Host "`n[1/3] Building web bundle (TypeScript + Vite)..." -ForegroundColor Yellow
npm run build

Write-Host "`n[2/3] Building Windows binaries (electron-builder)..." -ForegroundColor Yellow
npm run dist:win

Write-Host "`n[3/3] Copying output binaries to release/ ..." -ForegroundColor Yellow
if (-not (Test-Path $ReleaseDir)) {
    New-Item -ItemType Directory -Path $ReleaseDir | Out-Null
}

$WebRelease = Join-Path $WebDir "release"
$exeFiles = Get-ChildItem -Path $WebRelease -Filter "*.exe"
foreach ($exe in $exeFiles) {
    Move-Item $exe.FullName -Destination $ReleaseDir -Force
    $dest = Join-Path $ReleaseDir $exe.Name
    $sizeMb = [math]::Round((Get-Item $dest).Length / 1MB, 2)
    Write-Host "Artifact: $($exe.Name) ($sizeMb MB)" -ForegroundColor Green
}
Remove-Item -Recurse -Force "$WebRelease\win-unpacked" -ErrorAction SilentlyContinue

Write-Host "`n======================================================" -ForegroundColor Green
Write-Host " Windows build completed successfully! Check release/ directory." -ForegroundColor Green
Write-Host "======================================================" -ForegroundColor Green
