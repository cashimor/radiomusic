$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Microsoft C++ Build Tools with Desktop development with C++ first.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!(Test-Path -LiteralPath $cmake)) { throw 'CMake is missing from the Build Tools installation.' }
& $cmake -S $PSScriptRoot -B (Join-Path $PSScriptRoot 'build') -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& $cmake --build (Join-Path $PSScriptRoot 'build') --config Release
if ($LASTEXITCODE) { throw 'Compilation failed.' }
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir (Join-Path $PSScriptRoot 'build') -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed.' }
