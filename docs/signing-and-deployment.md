# Signing and Deployment Guide

## Development (test signing) — current state

Every `./build.sh` produces a test-signed package in `out/<Config>/`:

- MSBuild's `TestSign` target signs `ttkmd.sys` with the auto-generated
  `WDKTestCert` from `Cert:\CurrentUser\My` (SHA256 file digest via
  `DriverSign.FileDigestAlgorithm`, see DD-1).
- The driver targets run Inf2Cat and sign `ttkmd.cat` with the same certificate.
- `build.ps1` exports the public certificate as `tt-test.cer` for guest trust.

Target machine (the QEMU test VM, never the development host — OQ-3):

```powershell
bcdedit /set testsigning on          # done by test/vm/firstlogon.ps1
certutil -addstore -f Root tt-test.cer
certutil -addstore -f TrustedPublisher tt-test.cer
pnputil /add-driver ttkmd.inf /install
```

`test/vm/guest/install-driver.ps1` automates all of this.

## Release (attestation signing) — the path when this ships

Test-signed drivers never load on end-user machines (Secure Boot + code-integrity
policy). The supported route for a non-WHQL PnP driver:

1. **EV code-signing certificate** for the publishing legal entity (Tenstorrent or
   the community org shipping the port). Required to onboard the Partner Center
   hardware program.
2. **Microsoft Partner Center hardware dashboard**: create a hardware submission,
   upload a `.cab` containing `ttkmd.sys`, `ttkmd.inf`, `ttkmd.cat` (cab itself
   EV-signed). Choose **attestation signing** (no HLK required; Windows 10/11
   desktop only — matches this driver's support matrix).
3. Microsoft returns the package Microsoft-signed; that package installs on
   standard Secure-Boot machines.
4. Optional later: full **WHQL** via HLK playlist for the custom device class if
   logo requirements ever matter.

Constraints to preserve for attestation compatibility:

- `PnpLockdown=1` stays in the INF (already present).
- INF must pass `InfVerif /h` with no errors (driver-package rules; the build's
  Inf2Cat already enforces structural validity — add `InfVerif` to `build.ps1
  -Test` when targeting a submission).
- No custom kernel-mode signing exemptions; the driver must not rely on
  test-signing-only behavior.
- Attestation-signed drivers are not distributed via Windows Update by default —
  ship via installer (parallel of tt-kmd's DKMS packaging; a future
  `tt-win-installer` can wrap `pnputil`).

## Kernel DMA Protection note

On hosts with Kernel DMA Protection / IOMMU enabled, external-enclosure PCIe
devices may be blocked until the driver declares DMA remapping compatibility.
When M3 lands, evaluate adding the `DmaRemappingCompatible` device property in the
INF (`HKR,Parameters,DmaRemappingCompatible,0x00010001,1`) after verifying the
driver's DMA paths are remapping-safe — tracked as an M3 design item in the parity
matrix notes and to be tested with DMA remapping both on and off (spec hard
constraint 4/IOMMU row of the architecture table).
