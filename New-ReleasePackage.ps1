[CmdletBinding()]
param(
    [ValidatePattern('^v?\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')]
    [string]$Version = '',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$OutputDirectory = '',

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceDirectory = [IO.Path]::GetDirectoryName($PSCommandPath)
$moduleSourcePath = Join-Path $sourceDirectory 'native\src\module.cpp'
$moduleSource = [IO.File]::ReadAllText($moduleSourcePath, [Text.Encoding]::UTF8)
$versionMatch = [regex]::Match(
    $moduleSource,
    'push_result_string\("(?<version>\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?)"\)'
)
if (-not $versionMatch.Success) {
    throw "Native module version was not found: $moduleSourcePath"
}
$sourceVersion = $versionMatch.Groups['version'].Value
$packageVersion = if ($Version -eq '') { $sourceVersion } else { $Version.TrimStart('v') }
if ($packageVersion -cne $sourceVersion) {
    throw "Package version $packageVersion does not match native module version $sourceVersion"
}

if ($OutputDirectory -eq '') {
    $OutputDirectory = Join-Path $sourceDirectory 'dist'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

if (-not $SkipBuild) {
    & (Join-Path $sourceDirectory 'Build-LipSyncAviUtl1.ps1') `
        -Configuration $Configuration
}

$nativeModule = Join-Path $sourceDirectory (
    "native\build\$Configuration\LipSyncAviUtl1.mod2"
)
if (-not (Test-Path -LiteralPath $nativeModule -PathType Leaf)) {
    throw "Native module was not found: $nativeModule"
}

$stagingDirectory = Join-Path ([IO.Path]::GetTempPath()) (
    'PSDToolKit_AviUtl1LipSync_' + [guid]::NewGuid().ToString('N')
)
$runtimeDirectory = Join-Path $stagingDirectory 'Script\PSDToolKit'
$toolsDirectory = Join-Path $runtimeDirectory 'AviUtl1LipSyncTools'
$archiveName = "PSDToolKit_AviUtl1LipSync_v$packageVersion.au2pkg.zip"
$archivePath = Join-Path $OutputDirectory $archiveName

$runtimeFiles = @(
    'LipSyncAviUtl1.lua',
    'LipSyncAviUtl1Legacy.lua'
)
$toolFiles = @(
    'Convert-PSDToolKitLipSyncDefinition.ps1',
    'Invoke-DroppedDefinitionConversion.ps1',
    'PsdToolKitChecksum.ps1',
    'PSDToolKit口パク変換へドロップ.cmd',
    'README.md',
    'LICENSE',
    'CHANGELOG.md'
)

try {
    New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null
    foreach ($name in $runtimeFiles) {
        Copy-Item -LiteralPath (Join-Path $sourceDirectory $name) `
            -Destination (Join-Path $runtimeDirectory $name)
    }
    Copy-Item -LiteralPath $nativeModule `
        -Destination (Join-Path $runtimeDirectory 'LipSyncAviUtl1.mod2')
    foreach ($name in $toolFiles) {
        Copy-Item -LiteralPath (Join-Path $sourceDirectory $name) `
            -Destination (Join-Path $toolsDirectory $name)
    }
    $noticeDirectory = Join-Path $toolsDirectory 'native'
    New-Item -ItemType Directory -Path $noticeDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceDirectory 'native\THIRD_PARTY_NOTICES.md') `
        -Destination (Join-Path $noticeDirectory 'THIRD_PARTY_NOTICES.md')

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }
    Compress-Archive -Path (Join-Path $stagingDirectory '*') `
        -DestinationPath $archivePath `
        -CompressionLevel Optimal
}
finally {
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
}

[pscustomobject]@{
    Version = $packageVersion
    Archive = $archivePath
    ArchiveSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    ModuleSha256 = (Get-FileHash -LiteralPath $nativeModule -Algorithm SHA256).Hash
} | ConvertTo-Json -Depth 3
