# Build entry point (Windows side). Maps to: tt-kmd/Makefile.
# Uses the EWDK (DD-1 as amended): self-contained VS BuildTools + SDK + WDK from
# the mounted ISO — no installed VS/WDK required.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Configuration Release] [-Test]
#
# -Test additionally runs Code Analysis for Drivers with warnings as errors.
# From WSL use ./build.sh, which stages the tree to NTFS and calls this script.

[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',
    [switch]$Test,
    [string]$EwdkIso = 'C:\EWDK\ewdk_26100.6584.iso'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $EwdkIso)) {
    throw "EWDK ISO not found at $EwdkIso (see docs/design-decisions.md DD-1)"
}

$img = Get-DiskImage -ImagePath $EwdkIso -ErrorAction SilentlyContinue
if (-not $img -or -not $img.Attached) {
    $img = Mount-DiskImage -ImagePath $EwdkIso -PassThru
}
$drive = ($img | Get-Volume).DriveLetter
if (-not $drive) {
    throw 'EWDK ISO mounted but no volume/drive letter found'
}
$ewdk = "${drive}:"
Write-Host "EWDK: $ewdk ($(Get-Content "$ewdk\Version.txt" -ErrorAction SilentlyContinue))"

$proj = Join-Path $PSScriptRoot 'src\driver\ttkmd.vcxproj'

$msbuildArgs = @(
    "`"$proj`"",
    '/nologo', '/m', '/warnaserror',
    "/p:Configuration=$Configuration",
    '/p:Platform=x64'
)
if ($Test) {
    # Rebuild forces ClCompile to re-run; incremental builds would silently skip
    # analysis and report a hollow pass.
    $msbuildArgs += '/t:Rebuild'
    $msbuildArgs += '/p:RunCodeAnalysis=true'
    $msbuildArgs += '/p:CodeAnalysisTreatWarningsAsErrors=true'
}

# SetupBuildEnv.cmd defines the EWDK environment for this cmd instance only.
cmd /c "`"$ewdk\BuildEnv\SetupBuildEnv.cmd`" && msbuild $($msbuildArgs -join ' ')"
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

# User-mode conformance tests (same warnings-as-errors bar).
$ttinfo = Join-Path $PSScriptRoot 'src\tests\ttinfo\ttinfo.vcxproj'
cmd /c "`"$ewdk\BuildEnv\SetupBuildEnv.cmd`" && msbuild `"$ttinfo`" /nologo /m /warnaserror /p:Configuration=$Configuration /p:Platform=x64"
if ($LASTEXITCODE -ne 0) {
    throw "ttinfo build failed with exit code $LASTEXITCODE"
}

$outDir = Join-Path $PSScriptRoot "src\driver\x64\$Configuration"

# The driver targets stage the package (x64\<config>\ttkmd\), run Inf2Cat, and
# test-sign both .sys and catalog. Here we only export the public test cert so
# the test VM can trust it (Root + TrustedPublisher stores in the guest).
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.Subject -like '*WDKTestCert*' } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if ($cert) {
    Export-Certificate -Cert $cert -FilePath (Join-Path $outDir 'tt-test.cer') | Out-Null
} else {
    Write-Warning 'No WDKTestCert found in Cert:\CurrentUser\My; VM trust setup will need the signing cert'
}

Write-Host "`nArtifacts under $outDir :"
Get-ChildItem -Recurse $outDir -Include ttkmd.sys, ttkmd.inf, *.cat, tt-test.cer |
    Select-Object -ExpandProperty FullName |
    ForEach-Object { Write-Host "  $_" }
