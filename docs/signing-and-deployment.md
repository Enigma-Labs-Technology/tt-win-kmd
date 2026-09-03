# Signing and Deployment Guide

## Development (test signing)

Every `build.ps1` run produces a test-signed package in `out/<Config>/`:

- MSBuild's `TestSign` target signs `ttkmd.sys` with the auto-generated
  `WDKTestCert` from `Cert:\CurrentUser\My` (SHA-256 file digest, DD-1).
- Inf2Cat produces `ttkmd.cat`, signed with the same certificate.
- `build.ps1` exports the public certificate as `tt-test.cer` for guest trust.

Target machine (the QEMU test VM, or a lab host with Secure Boot off):

```powershell
bcdedit /set testsigning on          # reboot afterwards
certutil -addstore -f Root tt-test.cer
certutil -addstore -f TrustedPublisher tt-test.cer
pnputil /add-driver ttkmd.inf /install
```

`test/vm/guest/install-driver.ps1` automates this for the VM.

## Release (attestation signing)

Test-signed drivers do not load on end-user machines: Secure Boot and the
kernel code-integrity policy require a Microsoft signature. A PnP driver that
does not go through WHQL is signed by Microsoft through **attestation** on the
Partner Center hardware dashboard. Prerequisites, both already in hand for this
project: an **EV code-signing certificate** for the publishing legal entity and
a **Partner Center hardware program account** registered with that certificate.

Runbook (one submission per release):

1. Build the release package with Driver Code Analysis and Static Driver
   Verifier: `build.ps1 -Configuration Release -Test -Sdv`. The Release
   configuration does not compile the debug-only IOCTLs (`TT_DEBUG_INTERFACES`
   is Debug-only in `ttkmd.vcxproj`). SDV is not required for attestation but
   it is the standard bar for a KMDF driver; the run takes tens of minutes.
2. Run `InfVerif /h /w out\Release\ttkmd.inf` from the EWDK; attestation
   rejects INF errors. `PnpLockdown=1` stays in the INF.
3. Sign the binaries with the EV certificate (the test signature is replaced):

   ```powershell
   signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 `
       /n "<EV certificate subject>" out\Release\ttkmd.sys
   inf2cat /driver:out\Release /os:10_X64
   signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 `
       /n "<EV certificate subject>" out\Release\ttkmd.cat
   ```

   With a hardware-token EV certificate `signtool` prompts for the token PIN.
4. Package `ttkmd.sys`, `ttkmd.inf`, `ttkmd.cat` in a `.cab` (`makecab` with a
   DDF listing the three files) and sign the cab with the EV certificate.
5. Partner Center: Hardware, "Submit new hardware", upload the cab, select
   the Windows 11 x64 attestation target(s), request signature. No HLK
   package is needed for attestation.
6. Download the returned package: it contains the Microsoft-signed `ttkmd.cat`
   and `ttkmd.sys`. Install it on a stock Windows 11 machine with Secure Boot
   and memory integrity (HVCI) on, with `pnputil /add-driver ttkmd.inf /install`,
   and rerun `ttinfo.exe` and `ttconform.exe` before publishing.

Attestation-signed drivers are not distributed through Windows Update; ship
the signed package with an installer or instructions to run `pnputil`.

Constraints to keep for attestation compatibility:

- `PnpLockdown=1` in the INF, no `CopyFiles` outside the driver store.
- No dependence on test-signing behaviour, no debug IOCTLs in Release.
- The driver requests CET compatibility and links with `/INTEGRITYCHECK`
  (also required for its process-exit callback, DD-15); keep both,
  HVCI-enabled machines are the default target.
- Before submitting, run `ttinfo --only dma,pin` and `ttinfo --multiproc 4`
  on the rig and on the card: they cover the DMA ceiling, the backed-pin path
  and the process-ownership guard that this release depends on.

## Optional: WHQL

A full HLK run against the custom device class only matters if Windows Update
distribution or a logo is wanted. It is not required for the p150a release.

## Kernel DMA Protection

Both INFs opt the device into DMA remapping (`DmaRemappingCompatible = 1`).
`ALLOCATE_DMA_BUF` uses WDF common buffers, which are valid with remapping on
or off. `PIN_PAGES` is refused in a translated domain because the iATU is
programmed with the addresses the driver obtains, so on a host with Kernel DMA
Protection active tt-umd's sysmem must come from driver buffers, which it does
(see `docs/tt-umd-porting-notes.md`).
