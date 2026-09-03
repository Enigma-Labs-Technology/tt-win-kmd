# Changelog

## 0.1.0-preview (unreleased)

Developer preview of the Windows KMDF port of tt-kmd for the Blackhole p150a.

### Driver

- ABI aligned with tt-kmd 2.11.0: byte-identical request structures, one
  control code per Linux request number, `SMC_MSG` carried but not yet
  implemented (returns not-supported).
- Windows-private requests: `MAP`/`UNMAP` (in place of mmap),
  `QUERY_TELEMETRY` (in place of hwmon), `FREE_DMA_BUF_EX` (early release of a
  DMA buffer).
- `PIN_PAGES` of a `MAP` view of the handle's own DMA buffer is backed by that
  buffer: no page locking, valid with DMA remapping on or off (DD-16).
- Handles are bound to the opening process; a process-exit callback releases a
  dying process's views and locked pages (DD-15).
- Per-device ceiling on DMA-buffer memory, `DmaBufferLimitMiB` (DD-17).
- Reset flavours `CONFIG_WRITE` and `ASIC_RESET` are refused on physical
  hardware after they wedged a p150a (DD-14).
- Release package binds the p150a only and targets KMDF 1.33 for all Windows 11
  builds; the test package (Debug) adds the ttsim/QEMU model and the soft
  load-test node (DD-18).
- Driver, VERSIONINFO and INF `DriverVer` share one version source.

### Tests and tooling

- `ttinfo`: per-request tests including backed pins, `FREE_DMA_BUF_EX`, the
  DMA ceiling, the process-ownership guard, `--soak`, `--multiproc`.
- `ttconform`: every request through the `ttwin_compat` shim.
- `build.ps1 -Test` (Driver Code Analysis) and `-Sdv` (Static Driver Verifier).
- QEMU/ttsim rig with Driver Verifier; silicon first-contact ladder.

### Known limitations

- p150a only; other Blackhole boards, Wormhole and Grayskull are not matched.
- Warm reset via PLDR needs platform support the tested board lacks.
- Sysmem channels are capped at 256 MiB per buffer (OQ-10).
- `SMC_MSG`, `MAP_PEER_BAR`, `EXPORT_TLB_DMABUF` are not implemented.
- Test-signed until the attestation-signed package is published.
