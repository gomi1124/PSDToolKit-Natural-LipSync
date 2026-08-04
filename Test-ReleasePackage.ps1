[CmdletBinding()]
param(
    [string]$ArchivePath = '',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceDirectory = [IO.Path]::GetDirectoryName($PSCommandPath)
if ($ArchivePath -eq '') {
    $archive = Get-ChildItem -LiteralPath (Join-Path $sourceDirectory 'dist') `
        -File `
        -Filter '*.au2pkg.zip' |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $archive) {
        throw '配布パッケージが見つかりません。'
    }
    $ArchivePath = $archive.FullName
}
$ArchivePath = [IO.Path]::GetFullPath($ArchivePath)
if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
    throw "配布パッケージが見つかりません: $ArchivePath"
}

$expectedEntries = @(
    'Script/PSDToolKit/AviUtl1LipSyncTools/CHANGELOG.md',
    'Script/PSDToolKit/AviUtl1LipSyncTools/Convert-PSDToolKitLipSyncDefinition.ps1',
    'Script/PSDToolKit/AviUtl1LipSyncTools/Invoke-DroppedDefinitionConversion.ps1',
    'Script/PSDToolKit/AviUtl1LipSyncTools/LICENSE',
    'Script/PSDToolKit/AviUtl1LipSyncTools/PsdToolKitChecksum.ps1',
    'Script/PSDToolKit/AviUtl1LipSyncTools/PSDToolKit口パク変換へドロップ.cmd',
    'Script/PSDToolKit/AviUtl1LipSyncTools/README.md',
    'Script/PSDToolKit/AviUtl1LipSyncTools/native/THIRD_PARTY_NOTICES.md',
    'Script/PSDToolKit/LipSyncAviUtl1.lua',
    'Script/PSDToolKit/LipSyncAviUtl1.mod2',
    'Script/PSDToolKit/LipSyncAviUtl1Legacy.lua'
) | Sort-Object
$privateTerms = @(
    ('D:' + '\Users'),
    ('C:' + '\Users'),
    ('_' + '動画制作'),
    ('Codex_' + 'LipSync'),
    ('14_' + 'スーサイド'),
    ('97_' + 'テスト')
)
$privatePattern = ($privateTerms | ForEach-Object { [regex]::Escape($_) }) -join '|'

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
try {
    $fileEntries = @($archive.Entries | Where-Object { $_.Length -gt 0 })
    $actualEntries = @(
        $fileEntries |
            ForEach-Object { $_.FullName.Replace([char]92, [char]47) } |
            Sort-Object
    )
    if (($actualEntries | ConvertTo-Json -Compress) -cne
        ($expectedEntries | ConvertTo-Json -Compress)) {
        throw "配布パッケージの内容が想定と異なります:`n$($actualEntries -join "`n")"
    }

    foreach ($entry in $fileEntries) {
        if ($entry.FullName -like '*.mod2') {
            continue
        }
        $reader = [IO.StreamReader]::new($entry.Open(), [Text.Encoding]::UTF8, $true)
        try {
            $content = $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
        if ($content -match $privatePattern) {
            throw "個人環境の情報が配布物に含まれています: $($entry.FullName)"
        }
    }

    $moduleEntry = $fileEntries | Where-Object {
        $_.FullName.Replace([char]92, [char]47) -eq
            'Script/PSDToolKit/LipSyncAviUtl1.mod2'
    } | Select-Object -First 1
    $builtModule = Join-Path $sourceDirectory (
        "native\build\$Configuration\LipSyncAviUtl1.mod2"
    )
    if (-not (Test-Path -LiteralPath $builtModule -PathType Leaf)) {
        throw "比較対象のビルド済みモジュールがありません: $builtModule"
    }
    $memory = [IO.MemoryStream]::new()
    try {
        $entryStream = $moduleEntry.Open()
        try {
            $entryStream.CopyTo($memory)
        }
        finally {
            $entryStream.Dispose()
        }
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            $archiveHash = [BitConverter]::ToString(
                $sha256.ComputeHash($memory.ToArray())
            ).Replace('-', '')
        }
        finally {
            $sha256.Dispose()
        }
    }
    finally {
        $memory.Dispose()
    }
    $builtHash = (Get-FileHash -LiteralPath $builtModule -Algorithm SHA256).Hash
    if ($archiveHash -cne $builtHash) {
        throw '配布パッケージ内のネイティブモジュールがビルド結果と一致しません。'
    }
}
finally {
    $archive.Dispose()
}

[pscustomobject]@{
    Archive = $ArchivePath
    Files = $actualEntries.Count
    ModuleSha256 = $builtHash
    Status = 'Passed'
} | ConvertTo-Json -Depth 3
