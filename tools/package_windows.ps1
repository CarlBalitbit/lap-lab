#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$ExecutablePath = 'build/f1_track_sim.exe',
    [string]$OutputDirectory = 'build/releases',
    [string]$ManifestPath = 'packaging/windows-manifest.json',
    [switch]$VerifyOnly,
    [string]$ArchivePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Get-ProjectPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path, $projectRoot)
    if (-not $full.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must stay inside this project: $Path"
    }
    # Reject junctions/symlinks on the path and all existing ancestors.
    $current = $full
    while ($current.Length -ge $projectRoot.Length) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse points are not supported: $current"
            }
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
    return $full
}

function Assert-RelativeName([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name) -or $Name -match '[\\:]' -or
        $Name.StartsWith('/') -or @($Name.Split('/') | Where-Object { $_ -in @('', '.', '..') }).Count) {
        throw "Unsafe manifest/archive path: $Name"
    }
}

function Get-StreamHash([IO.Stream]$Stream) {
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToHexString($hasher.ComputeHash($Stream)).ToLowerInvariant() }
    finally { $hasher.Dispose() }
}

function Assert-Archive([string]$Path, [string]$Prefix, $Expected) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read)
        try {
            $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($entry in $zip.Entries) {
                Assert-RelativeName $entry.FullName
                if (-not $seen.Add($entry.FullName)) { throw "Duplicate ZIP entry: $($entry.FullName)" }
                if (-not $entry.FullName.StartsWith($Prefix + '/', [StringComparison]::Ordinal)) {
                    throw "Unexpected ZIP root: $($entry.FullName)"
                }
                $relative = $entry.FullName.Substring($Prefix.Length + 1)
                if (-not $Expected.ContainsKey($relative)) { throw "Unexpected ZIP entry: $relative" }
                $content = $entry.Open()
                try { $hash = Get-StreamHash $content } finally { $content.Dispose() }
                if ($hash -cne $Expected[$relative].Hash -or $entry.Length -ne $Expected[$relative].Length) {
                    throw "ZIP content/hash mismatch: $relative"
                }
            }
            if ($seen.Count -ne $Expected.Count) { throw 'ZIP is missing allowlisted files' }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
}

$exe = Get-ProjectPath $ExecutablePath
$manifestFile = Get-ProjectPath $ManifestPath
$version = [IO.File]::ReadAllText((Join-Path $projectRoot 'VERSION')).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { throw 'Invalid release VERSION' }
$prefix = "lap-lab-$version-windows-x64"
$manifest = [IO.File]::ReadAllText($manifestFile) | ConvertFrom-Json
if ($manifest.format_version -ne 1) { throw 'Unsupported packaging manifest version' }
$rootSources = @{
    'f1_track_sim.exe' = '@executable'; 'LICENSE' = 'LICENSE'
    'CIRCUIT_DATA_LICENSE.txt' = 'CIRCUIT_DATA_LICENSE.txt'; 'VERSION' = 'VERSION'
    'START_HERE.txt' = 'packaging/START_HERE.txt'; 'README.md' = 'packaging/README.md'
    'RELEASE_NOTES.md' = 'packaging/RELEASE_NOTES.md'; 'CHANGELOG.md' = 'CHANGELOG.md'
}
$expected = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
$destinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($item in $manifest.files) {
    $destination = [string]$item.destination
    $source = [string]$item.source
    Assert-RelativeName $destination
    if (-not $destinations.Add($destination)) { throw "Duplicate manifest destination: $destination" }
    if ($rootSources.ContainsKey($destination)) {
        if ($source -cne $rootSources[$destination]) { throw "Disallowed source mapping: $source -> $destination" }
    } elseif ($destination -cmatch '^tracks/(?:[a-z0-9-]+\.track|README\.md|CATALOG\.md)$') {
        if ($source -cne $destination) { throw 'Track source must match destination' }
    } else { throw "Disallowed distribution file: $destination" }
    if ($source -eq '@executable') { $sourcePath = $exe }
    else { Assert-RelativeName $source; $sourcePath = Get-ProjectPath $source }
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { throw "Missing packaging input: $sourcePath" }
    $expected.Add($destination, [pscustomobject]@{
        Source = $sourcePath; Length = (Get-Item -LiteralPath $sourcePath).Length
        Hash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    })
}
foreach ($name in @($rootSources.Keys) + @('tracks/README.md', 'tracks/CATALOG.md')) {
    if (-not $expected.ContainsKey($name)) { throw "Missing required manifest entry: $name" }
}
$actualTracks = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'tracks') -File -Force |
    Where-Object { $_.Extension -ceq '.track' } | ForEach-Object { 'tracks/' + $_.Name } | Sort-Object)
$listedTracks = @($expected.Keys | Where-Object { $_ -clike 'tracks/*.track' } | Sort-Object)
if (-not $actualTracks.Count -or (Compare-Object $actualTracks $listedTracks)) { throw 'Manifest does not exactly cover runtime .track files' }

# Check the executable is a PE x64 image. This is not a signature or build-provenance check.
$binary = [IO.File]::OpenRead($exe)
try {
    $reader = [IO.BinaryReader]::new($binary)
    if ($reader.ReadUInt16() -ne 0x5a4d -or $binary.Length -lt 64) { throw 'Executable is not a PE image' }
    $binary.Position = 0x3c; $peOffset = $reader.ReadUInt32()
    if ($peOffset -gt $binary.Length - 6) { throw 'Invalid PE header offset' }
    $binary.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Executable must target Windows x64' }
} finally { $binary.Dispose() }

if ($VerifyOnly) {
    if (-not $ArchivePath) { throw '-VerifyOnly requires -ArchivePath' }
    Assert-Archive (Get-ProjectPath $ArchivePath) $prefix $expected
    [pscustomobject]@{ Verified = $true; Files = $expected.Count }
    return
}
if ($ArchivePath) { throw '-ArchivePath is only valid with -VerifyOnly' }
$output = Get-ProjectPath $OutputDirectory
$zipPath = Join-Path $output "$prefix.zip"
$checksumPath = Join-Path $output 'SHA256SUMS.txt'
if ((Test-Path -LiteralPath $zipPath) -or (Test-Path -LiteralPath $checksumPath)) {
    throw 'Refusing to overwrite an existing ZIP or checksum; select a new output directory'
}
$build = Get-ProjectPath 'build'
[void][IO.Directory]::CreateDirectory($build)
$stage = Join-Path $build ('.packaging-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($stage)
try {
    $payload = Join-Path $stage $prefix
    [string[]]$names = @($expected.Keys)
    [Array]::Sort($names, [StringComparer]::Ordinal)
    foreach ($name in $names) {
        $target = Join-Path $payload $name
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy($expected[$name].Source, $target, $false)
    }
    $stagedZip = Join-Path $stage "$prefix.zip"
    $stream = [IO.File]::Open($stagedZip, [IO.FileMode]::CreateNew)
    try {
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($name in $names) {
                $entry = $zip.CreateEntry("$prefix/$name", [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(2020,1,1,0,0,0,[TimeSpan]::Zero)
                $entry.ExternalAttributes = 0
                $input = [IO.File]::OpenRead((Join-Path $payload $name))
                $content = $entry.Open()
                try { $input.CopyTo($content) } finally { $content.Dispose(); $input.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
    Assert-Archive $stagedZip $prefix $expected
    $hash = (Get-FileHash -LiteralPath $stagedZip -Algorithm SHA256).Hash.ToLowerInvariant()
    [void][IO.Directory]::CreateDirectory($output)
    # No replace flag, including a second check against concurrent publishers.
    # A concurrently created file is never deleted or overwritten on failure.
    if ((Test-Path -LiteralPath $zipPath) -or (Test-Path -LiteralPath $checksumPath)) { throw 'Output appeared during packaging; refusing overwrite' }
    $checksum = [IO.File]::Open($checksumPath, [IO.FileMode]::CreateNew)
    try {
        [IO.File]::Copy($stagedZip, $zipPath, $false)
        $bytes = [Text.Encoding]::UTF8.GetBytes("$hash  $prefix.zip`n")
        $checksum.Write($bytes,0,$bytes.Length)
    } finally { $checksum.Dispose() }
    [pscustomobject]@{ ZipPath = $zipPath; ChecksumPath = $checksumPath; SHA256 = $hash; Files = $expected.Count }
} finally {
    # Only this invocation's generated stage is eligible for recursive cleanup.
    $safeStage = Get-ProjectPath $stage
    if ([IO.Path]::GetDirectoryName($safeStage) -cne $build -or
        [IO.Path]::GetFileName($safeStage) -notmatch '^\.packaging-[a-f0-9]{32}$') { throw 'Unsafe cleanup target' }
    Remove-Item -LiteralPath $safeStage -Recurse -Force
}
