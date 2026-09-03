# Build entry point (Windows side). Maps to: tt-kmd/Makefile.
# Uses the EWDK (DD-1 as amended): self-contained VS BuildTools + SDK + WDK from
# the mounted ISO — no installed VS/WDK required.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Configuration Release] [-Test] [-Sdv]
#
# -Test additionally runs Code Analysis for Drivers with warnings as errors.
# -Sdv runs Static Driver Verifier (scan + default rule set) on the driver
#      project after the build and fails on any rule defect. Slow (tens of
#      minutes); intended before a signing submission, not for every build.
# From WSL use ./build.sh, which stages the tree to NTFS and calls this script.

[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Sdv,
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

if ($Sdv) {
    # Static Driver Verifier: the scan pass discovers the role types, the check
    # pass runs the default KMDF rule set. Results land under src\driver\sdv\
    # (SDV-default.xml, smvstats.txt); the DVL is produced for a later WHQL
    # submission but is not required for attestation signing.
    cmd /c "`"$ewdk\BuildEnv\SetupBuildEnv.cmd`" && msbuild `"$proj`" /nologo /p:Configuration=$Configuration /p:Platform=x64 /t:sdv /p:Inputs=`"/scan`""
    if ($LASTEXITCODE -ne 0) {
        throw "SDV scan failed with exit code $LASTEXITCODE"
    }
    cmd /c "`"$ewdk\BuildEnv\SetupBuildEnv.cmd`" && msbuild `"$proj`" /nologo /p:Configuration=$Configuration /p:Platform=x64 /t:sdv /p:Inputs=`"/check:default.sdv`""
    if ($LASTEXITCODE -ne 0) {
        throw "SDV check failed with exit code $LASTEXITCODE"
    }
    $sdvResult = Join-Path $PSScriptRoot 'src\driver\sdv\SDV-default.xml'
    if (Test-Path $sdvResult) {
        $defects = Select-String -Path $sdvResult -Pattern 'Defect' -SimpleMatch
        if ($defects) {
            Write-Host "SDV reported defects; see $sdvResult"
            throw 'Static Driver Verifier found rule violations'
        }
        Write-Host "SDV: no defects ($sdvResult)"
    } else {
        Write-Warning "SDV result file not found at $sdvResult"
    }
}

# User-mode conformance tests (same warnings-as-errors bar).
$ttinfo = Join-Path $PSScriptRoot 'src\tests\ttinfo\ttinfo.vcxproj'
cmd /c "`"$ewdk\BuildEnv\SetupBuildEnv.cmd`" && msbuild `"$ttinfo`" /nologo /m /warnaserror /p:Configuration=$Configuration /p:Platform=x64"
if ($LASTEXITCODE -ne 0) {
    throw "ttinfo build failed with exit code $LASTEXITCODE"
}

# ttwin_compat shim + its conformance suite (M6).
$ttconform = Join-Path $PSScriptRoot 'src\tests\ttconform\ttconform.vcxproj'
cmd /c "`"$ewdk\BuildEnv\SetupBuildEnv.cmd`" && msbuild `"$ttconform`" /nologo /m /warnaserror /p:Configuration=$Configuration /p:Platform=x64"
if ($LASTEXITCODE -ne 0) {
    throw "ttconform build failed with exit code $LASTEXITCODE"
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
Get-ChildItem -Recurse $outDir -Include ttkmd.sys, *.inf, *.cat, tt-test.cer |
    Select-Object -ExpandProperty FullName |
    ForEach-Object { Write-Host "  $_" }
