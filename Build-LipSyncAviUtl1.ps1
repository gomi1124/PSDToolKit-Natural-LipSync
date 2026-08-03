[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$VisualStudioRoot = '',

    [string]$BuildDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceDirectory = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$nativeDirectory = Join-Path $sourceDirectory 'native'
if ($BuildDirectory -eq '') {
    $BuildDirectory = Join-Path $nativeDirectory 'build'
}
$configurationDirectory = Join-Path $BuildDirectory $Configuration
New-Item -ItemType Directory -Path $configurationDirectory -Force | Out-Null

if ($VisualStudioRoot -eq '') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere.exe was not found: $vswhere"
    }
    $VisualStudioRoot = (& $vswhere `
        -latest `
        -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if ($VisualStudioRoot -eq '') {
        throw 'Visual Studio with the Desktop development with C++ workload was not found'
    }
}

$msvcRoot = Join-Path $VisualStudioRoot 'VC\Tools\MSVC'
$msvcTools = Get-ChildItem -LiteralPath $msvcRoot -Directory |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if ($null -eq $msvcTools) {
    throw "MSVC tools were not found: $msvcRoot"
}

$windowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$windowsSdk = Get-ChildItem -LiteralPath (Join-Path $windowsSdkRoot 'Include') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if ($null -eq $windowsSdk) {
    throw "Windows SDK was not found: $windowsSdkRoot"
}

$compiler = Join-Path $msvcTools.FullName 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    throw "x64 compiler was not found: $compiler"
}

$includeArguments = @(
    "/I$(Join-Path $msvcTools.FullName 'include')"
    "/I$(Join-Path $windowsSdk.FullName 'ucrt')"
    "/I$(Join-Path $windowsSdk.FullName 'um')"
    "/I$(Join-Path $windowsSdk.FullName 'shared')"
)
$sdkVersion = $windowsSdk.Name
$linkArguments = @(
    "/LIBPATH:$(Join-Path $msvcTools.FullName 'lib\x64')"
    "/LIBPATH:$(Join-Path $windowsSdkRoot "Lib\$sdkVersion\ucrt\x64")"
    "/LIBPATH:$(Join-Path $windowsSdkRoot "Lib\$sdkVersion\um\x64")"
)
$commonCpp = @(
    '/nologo',
    '/c',
    '/std:c++17',
    '/EHsc',
    '/MT',
    '/utf-8',
    '/W4',
    '/permissive-',
    '/Zc:__cplusplus'
)
$optimization = if ($Configuration -eq 'Release') {
    @('/O2', '/DNDEBUG')
} else {
    @('/Od', '/Zi')
}

function Invoke-Compiler {
    param(
        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [Parameter(Mandatory)]
        [string]$Description
    )

    & $compiler @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

$analyzerObject = Join-Path $configurationDirectory 'lipsync_analyzer.obj'
$japaneseReadingObject = Join-Path $configurationDirectory 'japanese_reading.obj'
$wavAudioSourceObject = Join-Path $configurationDirectory 'wav_audio_source.obj'
$fftObject = Join-Path $configurationDirectory 'fftsg.obj'
$moduleObject = Join-Path $configurationDirectory 'module.obj'
$testObject = Join-Path $configurationDirectory 'lipsync_analyzer_tests.obj'
$wavProbeObject = Join-Path $configurationDirectory 'analyze_wav.obj'
$adaptiveProbeObject = Join-Path $configurationDirectory 'analyze_adaptive.obj'
$japaneseReadingProbeObject = Join-Path $configurationDirectory 'analyze_japanese_reading.obj'
$module = Join-Path $configurationDirectory 'LipSyncAviUtl1.mod2'
$testExecutable = Join-Path $configurationDirectory 'LipSyncAviUtl1Tests.exe'
$wavProbeExecutable = Join-Path $configurationDirectory 'LipSyncAviUtl1WavProbe.exe'
$adaptiveProbeExecutable = Join-Path $configurationDirectory 'LipSyncAdaptiveWavProbe.exe'
$japaneseReadingProbeExecutable = Join-Path $configurationDirectory 'JapaneseReadingProbe.exe'

Invoke-Compiler -Description 'Analyzer compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    "/I$(Join-Path $nativeDirectory 'src')"
    "/Fo$analyzerObject"
    (Join-Path $nativeDirectory 'src\lipsync_analyzer.cpp')
)

Invoke-Compiler -Description 'Japanese reading compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    '/DUNICODE'
    '/D_UNICODE'
    "/I$(Join-Path $nativeDirectory 'src')"
    "/Fo$japaneseReadingObject"
    (Join-Path $nativeDirectory 'src\japanese_reading.cpp')
)

Invoke-Compiler -Description 'WAV audio source compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    "/I$(Join-Path $nativeDirectory 'src')"
    "/Fo$wavAudioSourceObject"
    (Join-Path $nativeDirectory 'src\wav_audio_source.cpp')
)

Invoke-Compiler -Description 'Ooura RDFT compilation' -Arguments @(
    '/nologo'
    '/c'
    '/TC'
    '/MT'
    '/O2'
    '/W3'
    $includeArguments
    "/Fo$fftObject"
    (Join-Path $nativeDirectory 'third_party\ooura\fftsg.c')
)

Invoke-Compiler -Description 'Script module compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    '/DUNICODE'
    '/D_UNICODE'
    '/DWIN32_LEAN_AND_MEAN'
    '/DNOMINMAX'
    "/I$(Join-Path $nativeDirectory 'src')"
    "/I$(Join-Path $nativeDirectory 'third_party\aviutl2_sdk\include\aviutl2_sdk')"
    "/Fo$moduleObject"
    (Join-Path $nativeDirectory 'src\module.cpp')
)

Invoke-Compiler -Description 'Script module link' -Arguments @(
    '/nologo'
    '/LD'
    '/MT'
    "/Fe$module"
    $moduleObject
    $analyzerObject
    $japaneseReadingObject
    $wavAudioSourceObject
    $fftObject
    '/link'
    '/NOLOGO'
    '/MACHINE:X64'
    '/INCREMENTAL:NO'
    'ole32.lib'
    'oleaut32.lib'
    $linkArguments
)

Invoke-Compiler -Description 'Test compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    "/I$(Join-Path $nativeDirectory 'src')"
    "/Fo$testObject"
    (Join-Path $nativeDirectory 'tests\lipsync_analyzer_tests.cpp')
)

Invoke-Compiler -Description 'Test link' -Arguments @(
    '/nologo'
    '/EHsc'
    '/MT'
    "/Fe$testExecutable"
    $testObject
    $analyzerObject
    $japaneseReadingObject
    $wavAudioSourceObject
    $fftObject
    '/link'
    '/NOLOGO'
    '/MACHINE:X64'
    '/INCREMENTAL:NO'
    'ole32.lib'
    'oleaut32.lib'
    $linkArguments
)

Invoke-Compiler -Description 'WAV probe compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    '/DNOMINMAX'
    "/I$(Join-Path $nativeDirectory 'src')"
    "/Fo$wavProbeObject"
    (Join-Path $nativeDirectory 'tools\analyze_wav.cpp')
)

Invoke-Compiler -Description 'WAV probe link' -Arguments @(
    '/nologo'
    '/EHsc'
    '/MT'
    "/Fe$wavProbeExecutable"
    $wavProbeObject
    $analyzerObject
    $wavAudioSourceObject
    $fftObject
    '/link'
    '/NOLOGO'
    '/MACHINE:X64'
    '/INCREMENTAL:NO'
    $linkArguments
)

Invoke-Compiler -Description 'Adaptive probe compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    '/DNOMINMAX'
    "/I$(Join-Path $nativeDirectory 'src')"
    "/Fo$adaptiveProbeObject"
    (Join-Path $nativeDirectory 'tools\analyze_adaptive.cpp')
)

Invoke-Compiler -Description 'Adaptive probe link' -Arguments @(
    '/nologo'
    '/EHsc'
    '/MT'
    "/Fe$adaptiveProbeExecutable"
    $adaptiveProbeObject
    $analyzerObject
    $wavAudioSourceObject
    $fftObject
    '/link'
    '/NOLOGO'
    '/MACHINE:X64'
    '/INCREMENTAL:NO'
    $linkArguments
)

Invoke-Compiler -Description 'Japanese reading probe compilation' -Arguments @(
    $commonCpp
    $optimization
    $includeArguments
    '/DUNICODE'
    '/D_UNICODE'
    "/I$(Join-Path $nativeDirectory 'src')"
    "/Fo$japaneseReadingProbeObject"
    (Join-Path $nativeDirectory 'tools\analyze_japanese_reading.cpp')
)

Invoke-Compiler -Description 'Japanese reading probe link' -Arguments @(
    '/nologo'
    '/EHsc'
    '/MT'
    "/Fe$japaneseReadingProbeExecutable"
    $japaneseReadingProbeObject
    $japaneseReadingObject
    '/link'
    '/NOLOGO'
    '/MACHINE:X64'
    '/INCREMENTAL:NO'
    'ole32.lib'
    'oleaut32.lib'
    $linkArguments
)

& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Native tests failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path -LiteralPath $module)) {
    throw "Built module was not found: $module"
}

[pscustomobject]@{
    Configuration = $Configuration
    Module = $module
    TestExecutable = $testExecutable
    WavProbeExecutable = $wavProbeExecutable
    AdaptiveProbeExecutable = $adaptiveProbeExecutable
    JapaneseReadingProbeExecutable = $japaneseReadingProbeExecutable
    Sha256 = (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash
} | ConvertTo-Json -Depth 3
