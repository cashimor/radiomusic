$ErrorActionPreference = 'Stop'
$env:JAVA_HOME = Join-Path $env:LOCALAPPDATA 'Programs\Microsoft\jdk-21'
$env:ANDROID_HOME = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
$env:GRADLE_USER_HOME = Join-Path $env:LOCALAPPDATA 'Gradle'
if (!(Test-Path -LiteralPath (Join-Path $env:JAVA_HOME 'bin\java.exe'))) { throw 'Microsoft OpenJDK 21 is not installed in this Windows profile.' }
if (!(Test-Path -LiteralPath (Join-Path $env:ANDROID_HOME 'platforms\android-36\android.jar'))) { throw 'Android SDK Platform 36 is not installed in this Windows profile.' }
& (Join-Path $PSScriptRoot 'gradlew.bat') assembleDebug
if ($LASTEXITCODE) { throw 'Android APK build failed.' }
