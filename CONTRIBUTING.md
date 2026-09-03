# Contributing

This is a community port of Tenstorrent's Linux kernel driver (tt-kmd) to
Windows. It is not affiliated with or supported by Tenstorrent. Contributions
are welcome under the repository licence (GPL-2.0-only, see `LICENSE` and
`NOTICE`).

## Building

- Windows 11 x64 host. The supported build is the EWDK ISO configured in
  `build.ps1` (`C:\EWDK\ewdk_26100.6584.iso`); no installed Visual Studio or
  WDK is needed.
- `powershell -ExecutionPolicy Bypass -File build.ps1 -Configuration Release -Test`
  builds the driver, the test binaries and runs Driver Code Analysis with
  warnings as errors. `-Sdv` adds Static Driver Verifier.
- The Debug configuration produces the test package (`ttkmd-test.inf`), the
  Release configuration the p150a package (`ttkmd.inf`).
- The QEMU/ttsim test rig and the silicon ladder are described in
  `test/README.md` and `test/silicon/README.md`.

## Conventions

- `docs/linux-driver-analysis.md` is the reference for upstream behaviour;
  check it before changing a ported code path.
- Every deviation from a direct Linux-to-Windows mapping is recorded as a
  `DD-N` in `docs/design-decisions.md`; open ambiguities as `OQ-N` in
  `docs/open-questions.md`.
- `docs/ioctl-parity-matrix.md` rows change status in the same commit as the
  code they describe.
- Each driver source file starts with a `Maps to: tt-kmd/<file>` comment.
- Commit messages use the `[M<n>] <area>: <description>` form.
- Kernel code compiles at `/W4 /WX` with Driver Code Analysis clean; every
  IOCTL validates its input before touching state or hardware and returns the
  NTSTATUS mapped from the Linux errno in the parity matrix.
- The ABI header is checked against `docs/abi-ground-truth.txt` by
  `static_assert`; regenerate it with `tools/gen_abi_truth.sh` on every upstream
  rebase (`docs/maintenance-guide.md`).
- User-mode changes that touch the tt-umd contract are mirrored in
  `docs/tt-umd-porting-notes.md`.

## Testing a change

1. `build.ps1 -Configuration Debug -Test`.
2. On the QEMU rig under Driver Verifier: `ttinfo --all-legacy`, then
   `ttinfo --only dma,pin` and `ttinfo --multiproc 4`, then `ttconform`.
3. For changes to memory, reset or hardware paths: the silicon ladder on a
   p150a with Secure Boot and memory integrity enabled.

State in the pull request which of these ran and where.

## Reporting problems

Use the issue tracker for bugs and questions. For security-sensitive reports
see `SECURITY.md`.
