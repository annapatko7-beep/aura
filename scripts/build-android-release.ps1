# Selfvoy Android (.aab & .apk) Release Build Script
param(
    [string]$ServerUrl = "selfvoy://standalone"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir
$WebDir = Join-Path $RootDir "web"
$AndroidDir = Join-Path $WebDir "android"
$ReleaseDir = Join-Path $RootDir "release"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Building Selfvoy Android Release" -ForegroundColor Cyan
Write-Host " Production WebSocket Server: $ServerUrl" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# 1. Build web bundle
$env:VITE_SELFVOY_SERVER_URL = $ServerUrl
$env:NODE_ENV = "production"

Set-Location $WebDir
Write-Host "`n[1/4] Building web bundle..." -ForegroundColor Yellow
npm run build

# 2. Sync with Android Capacitor project
Write-Host "`n[2/4] Syncing Capacitor (cap sync android)..." -ForegroundColor Yellow
npx cap sync android

# 3. Check keystore
Set-Location $AndroidDir
$KeystoreFile = Join-Path $AndroidDir "keystore.properties"
if (-not (Test-Path $KeystoreFile)) {
    Write-Host "`nWARNING: web/android/keystore.properties was not found." -ForegroundColor Yellow
    Write-Host "The build will be unsigned or use default signing." -ForegroundColor Yellow
    Write-Host "To generate a release key, run:" -ForegroundColor Yellow
    Write-Host "  keytool -genkey -v -keystore web/android/app/release-key.jks -keyalg RSA -keysize 2048 -validity 10000 -alias selfvoy" -ForegroundColor Yellow
}

# 4. Build AAB (Google Play Store) and APK
Write-Host "`n[3/4] Building Google Play App Bundle (bundleRelease)..." -ForegroundColor Yellow
$wslCmd = "export ANDROID_HOME=/opt/android-sdk && export ANDROID_SDK_ROOT=/opt/android-sdk && export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 && cd /mnt/c/Users/Konstantin/Desktop/selfvoy/web/android && /opt/gradle/gradle-8.14.3/bin/gradle bundleRelease assembleRelease --no-daemon"
wsl -u root -e sh -c $wslCmd

if (-not (Test-Path $ReleaseDir)) {
    New-Item -ItemType Directory -Path $ReleaseDir | Out-Null
}

$AabOutput = Join-Path $AndroidDir "app\build\outputs\bundle\release"
$ApkOutput = Join-Path $AndroidDir "app\build\outputs\apk\release"

if (Test-Path $AabOutput) {
    Get-ChildItem -Path $AabOutput -Filter "*.aab" | ForEach-Object {
        $dest = Join-Path $ReleaseDir "selfvoy-release.aab"
        Copy-Item $_.FullName -Destination $dest -Force
        $sizeMb = [math]::Round((Get-Item $dest).Length / 1MB, 2)
        Write-Host "Artifact: release/selfvoy-release.aab ($sizeMb MB)" -ForegroundColor Green
    }
}

if (Test-Path $ApkOutput) {
    Get-ChildItem -Path $ApkOutput -Filter "*.apk" | ForEach-Object {
        $dest = Join-Path $ReleaseDir "selfvoy-release.apk"
        Copy-Item $_.FullName -Destination $dest -Force
        $sizeMb = [math]::Round((Get-Item $dest).Length / 1MB, 2)
        Write-Host "Artifact: release/selfvoy-release.apk ($sizeMb MB)" -ForegroundColor Green
    }
}

Write-Host "`n======================================================" -ForegroundColor Green
Write-Host " Android build completed successfully! Check release/ directory." -ForegroundColor Green
Write-Host "======================================================" -ForegroundColor Green
