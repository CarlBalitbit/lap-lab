#requires -Version 7.0
[CmdletBinding()]
param([string]$ExecutablePath = 'build/f1_track_sim.exe')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$packageScript = Join-Path $PSScriptRoot 'package_windows.ps1'
$testRoot = Join-Path $root ('build/package-tests-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$timer = [Diagnostics.Stopwatch]::StartNew()
$script:checks = 0

function Assert-Test([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "TEST FAILED: $Name" }
    $script:checks++
    Write-Host "PASS: $Name"
}
function Expect-Rejection([string]$Name, [scriptblock]$Work, [string]$Message) {
    $rejected = $false
    try { & $Work | Out-Null }
    catch {
        if ($_.Exception.Message -notmatch $Message) { throw "Wrong rejection for ${Name}: $($_.Exception.Message)" }
        $rejected = $true
    }
    Assert-Test $rejected $Name
}
function Verify-TestArchive([string]$Path) {
    & $packageScript -ExecutablePath $ExecutablePath -VerifyOnly -ArchivePath $Path
}
function Make-BadArchive([string]$Name, [scriptblock]$Mutation) {
    $path = Join-Path $testRoot "$Name.zip"
    [IO.File]::Copy($first.ZipPath,$path,$false)
    $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite)
    try {
        $zip = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Update)
        try { & $Mutation $zip } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
    return $path
}
function Make-BadManifest([string]$Name, [scriptblock]$Mutation) {
    $manifest = Get-Content -LiteralPath (Join-Path $root 'packaging/windows-manifest.json') -Raw | ConvertFrom-Json
    & $Mutation $manifest
    $path = Join-Path $testRoot "$Name.json"
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $path -Encoding utf8NoBOM
    return $path
}
function Run-Extracted([string]$Exe, [string]$WorkingDirectory, [string]$InputText) {
    $info = [Diagnostics.ProcessStartInfo]::new($Exe)
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.Write($InputText)
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) { $process.Kill($true); throw 'Extracted smoke test timed out' }
        return [pscustomobject]@{ ExitCode=$process.ExitCode; Output=$stdout.GetAwaiter().GetResult(); Error=$stderr.GetAwaiter().GetResult() }
    } finally { $process.Dispose() }
}

$first = & $packageScript -ExecutablePath $ExecutablePath -OutputDirectory (Join-Path $testRoot 'first')
$second = & $packageScript -ExecutablePath $ExecutablePath -OutputDirectory (Join-Path $testRoot 'second')
Assert-Test ($first.Files -eq 50) 'exact 50-file allowlist'
Assert-Test ($first.SHA256 -ceq $second.SHA256) 'byte-identical repeated packaging'
Assert-Test ((Get-FileHash -LiteralPath $first.ZipPath).Hash.ToLowerInvariant() -ceq $first.SHA256) 'independent ZIP SHA-256'
$checksum = [IO.File]::ReadAllText($first.ChecksumPath)
Assert-Test ($checksum -ceq "$($first.SHA256)  $([IO.Path]::GetFileName($first.ZipPath))`n") 'checksum file contents'
$verified = Verify-TestArchive $first.ZipPath
Assert-Test ($verified.Verified -and $verified.Files -eq 50) 'all archive file hashes verified'
$prefix = [IO.Path]::GetFileNameWithoutExtension($first.ZipPath)
Expect-Rejection 'existing ZIP/checksum never overwritten' {
    & $packageScript -ExecutablePath $ExecutablePath -OutputDirectory (Join-Path $testRoot 'first')
} 'Refusing to overwrite'
Assert-Test ([IO.File]::ReadAllText($first.ChecksumPath) -ceq $checksum -and
    (Get-FileHash -LiteralPath $first.ZipPath).Hash.ToLowerInvariant() -ceq $first.SHA256) 'existing outputs unchanged'
$checksumOnly = Join-Path $testRoot 'checksum-only'
[void][IO.Directory]::CreateDirectory($checksumOnly)
[IO.File]::WriteAllText((Join-Path $checksumOnly 'SHA256SUMS.txt'),'existing')
Expect-Rejection 'existing checksum alone prevents publication' {
    & $packageScript -ExecutablePath $ExecutablePath -OutputDirectory $checksumOnly
} 'Refusing to overwrite'
Assert-Test (-not (Test-Path -LiteralPath (Join-Path $checksumOnly "$prefix.zip"))) 'no ZIP published after checksum conflict'
Expect-Rejection 'missing executable' {
    & $packageScript -ExecutablePath (Join-Path $testRoot 'missing.exe') -OutputDirectory (Join-Path $testRoot 'missing')
} 'Missing packaging input'
$invalidExe = Join-Path $testRoot 'invalid.exe'
[IO.File]::WriteAllBytes($invalidExe,[byte[]](0..63))
Expect-Rejection 'invalid executable format' {
    & $packageScript -ExecutablePath $invalidExe -OutputDirectory (Join-Path $testRoot 'invalid-exe')
} 'not a PE image'
Expect-Rejection 'output path outside project' {
    & $packageScript -ExecutablePath $ExecutablePath -OutputDirectory (Join-Path $root '../outside-package-test')
} 'inside this project'
$bad = Make-BadManifest 'unsafe-path' { param($m) $m.files[0].destination='../f1_track_sim.exe' }
Expect-Rejection 'manifest traversal path' { & $packageScript -ExecutablePath $ExecutablePath -ManifestPath $bad } 'Unsafe manifest'
$bad = Make-BadManifest 'missing-runtime' { param($m) $m.files=@($m.files | Where-Object destination -ne 'tracks/monza.track') }
Expect-Rejection 'missing circuit in manifest' { & $packageScript -ExecutablePath $ExecutablePath -ManifestPath $bad } 'exactly cover runtime'
$bad = Make-BadManifest 'duplicate-manifest' { param($m) $m.files+= $m.files[0] }
Expect-Rejection 'duplicate manifest destination' { & $packageScript -ExecutablePath $ExecutablePath -ManifestPath $bad } 'Duplicate manifest'
$bad = Make-BadManifest 'dev-source' { param($m) ($m.files | Where-Object destination -eq 'README.md').source='AGENTS.md' }
Expect-Rejection 'developer source disguised as README' { & $packageScript -ExecutablePath $ExecutablePath -ManifestPath $bad } 'Disallowed source mapping'
$bad = Make-BadManifest 'dev-destination' { param($m) $m.files+= [pscustomobject]@{source='AGENTS.md';destination='AGENTS.md'} }
Expect-Rejection 'developer distribution entry' { & $packageScript -ExecutablePath $ExecutablePath -ManifestPath $bad } 'Disallowed distribution'
$bad = Make-BadArchive 'unexpected' { param($z) [void]$z.CreateEntry("$prefix/AGENTS.md") }
Expect-Rejection 'unexpected ZIP entry' { Verify-TestArchive $bad } 'Unexpected ZIP entry'
$bad = Make-BadArchive 'duplicate' { param($z) [void]$z.CreateEntry("$prefix/README.md") }
Expect-Rejection 'duplicate ZIP entry' { Verify-TestArchive $bad } 'Duplicate ZIP entry'
$bad = Make-BadArchive 'case-collision' { param($z) [void]$z.CreateEntry("$prefix/readme.md") }
Expect-Rejection 'case-colliding ZIP entry' { Verify-TestArchive $bad } 'Duplicate ZIP entry'
$bad = Make-BadArchive 'unsafe-zip' { param($z) [void]$z.CreateEntry('../unsafe.txt') }
Expect-Rejection 'ZIP traversal path' { Verify-TestArchive $bad } 'Unsafe manifest/archive'
$bad = Make-BadArchive 'missing-zip' { param($z) $z.GetEntry("$prefix/README.md").Delete() }
Expect-Rejection 'missing ZIP entry' { Verify-TestArchive $bad } 'missing allowlisted'
$bad = Make-BadArchive 'hash-mismatch' {
    param($z) $z.GetEntry("$prefix/README.md").Delete()
    $entry=$z.CreateEntry("$prefix/README.md"); $s=$entry.Open()
    try { $bytes=[Text.Encoding]::UTF8.GetBytes('tampered'); $s.Write($bytes,0,$bytes.Length) } finally { $s.Dispose() }
}
Expect-Rejection 'tampered ZIP content/hash' { Verify-TestArchive $bad } 'content/hash mismatch'

$extracted = Join-Path $testRoot 'extracted'
[IO.Compression.ZipFile]::ExtractToDirectory($first.ZipPath,$extracted)
$appFolder = Join-Path $extracted $prefix
$exe = Join-Path $appFolder 'f1_track_sim.exe'
# Launch outside the application folder to exercise executable-relative defaults.
$smoke = Run-Extracted $exe $testRoot "Q`n"
Assert-Test ($smoke.ExitCode -eq 0 -and $smoke.Output -match 'LAP LAB' -and $smoke.Output -match 'Simulator closed') 'extracted launch and clean quit'
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
Assert-Test ($smoke.Output -match ('SIMULATOR v' + [regex]::Escape($version) + '\s')) 'extracted application banner matches VERSION'
# Basic, solo, balanced car, real circuits, first alphabetical circuit, qualifying, quit.
$smoke = Run-Extracted $exe $testRoot "1`n1`n2`n3`n1`n1`nQ`n"
Assert-Test ($smoke.ExitCode -eq 0 -and $smoke.Output -match 'Loaded ' -and $smoke.Output -match 'Session saved to:') 'extracted runtime circuit and qualifying session'
$sessions = @(Get-ChildItem -LiteralPath (Join-Path $appFolder 'LapData') -Directory)
Assert-Test ($sessions.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $sessions[0].FullName 'COMPLETE.txt')) -and
    (Test-Path -LiteralPath (Join-Path $sessions[0].FullName 'session_report.html'))) 'extracted session exports completed'
Write-Host "SUMMARY: $checks checks passed; runtime_s=$($timer.Elapsed.TotalSeconds)"
[pscustomobject]@{Checks=$checks;RuntimeSeconds=$timer.Elapsed.TotalSeconds;TestDirectory=$testRoot;ZipSHA256=$first.SHA256}
