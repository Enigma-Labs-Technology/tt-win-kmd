# Maintenance Guide — tracking upstream tt-kmd

This port is pinned to upstream tag `ttkmd-2.11.0` (commit `c05f224`, sibling
checkout `../tt-kmd`). Every document and source comment cites file:line against
that tag. This guide is the procedure for absorbing a new upstream tag.

## 1. Diff the upstream delta

```bash
cd ../tt-kmd
git fetch --tags origin
git diff ttkmd-2.11.0..<new-tag> --stat
git diff ttkmd-2.11.0..<new-tag> -- ioctl.h        # ABI first
```

Triage order (highest blast radius first):

| Upstream file | Port artifacts to revisit |
|---|---|
| `ioctl.h` | `src/include/ttkmd_ioctl.h`, `docs/ioctl-parity-matrix.md`, `docs/abi-ground-truth.txt`, conformance tests |
| `chardev.c` | ioctl dispatch/validation parity, mmap-offset scheme (MAP/UNMAP ioctls), post-reset fd semantics |
| `memory.c`, `tlb.c`, `sg_helpers.c` | M3 memory manager, pin/unpin, TLB pools |
| `blackhole.c/.h`, `wormhole.c/.h` | hardware layer: registers, BAR maps, iATU, reset sequences; ttsim glue BAR table |
| `msgqueue.c`, `telemetry.c` | M2 ARC messaging, M5 telemetry names/units |
| `enumerate.c`, `pcie.c`, `interrupt.c` | lifecycle, reset flavors, interrupt usage |
| `module.c`, packaging files | module params → registry parameters, INF/version |

## 2. Regenerate ABI ground truth

```bash
tools/gen_abi_truth.sh      # recompiles against the NEW ../tt-kmd/ioctl.h
git diff docs/abi-ground-truth.txt
```

Any size/offset change is an ABI break upstream chose to make — mirror it in
`ttkmd_ioctl.h` (the static_asserts will fail the build until the header matches)
and record the row change in the parity matrix.

## 3. Refresh the analysis

`docs/linux-driver-analysis.md` is assembled from `docs/analysis/NN-*.md`. For each
upstream file with a non-trivial diff, update the owning section (the section's
"Scope" header lists its files), re-verifying citations against the new tag. Update
the baseline tag in the master document header. Small deltas: edit in place. Large
deltas: redo the analysis for the affected sections only, keeping the layout
and citation style of the existing section files.

## 4. Update the parity matrix and code

For every changed ioctl/behavior: matrix row → `design-pending`/`stub` until the
handler is re-ported; implement; re-run the conformance suite (M6) plus the
affected milestone's acceptance tests in the VM; only then set `tested` again.

## 5. Version stamping

Upstream driver version lives in `dkms.conf`/`module.h` (see open question on the
authoritative source in `docs/analysis/open-questions-raw.md`). The port carries it
in `GET_DRIVER_INFO` out fields (`driver_version_major/minor/patch`) — keep these
equal to the upstream tag being tracked, and bump `DriverVer` via stampinf
automatically at build.

## 6. Rebaseline checklist (copy into the PR description)

- [ ] `tools/gen_abi_truth.sh` re-run; static_asserts green
- [ ] parity matrix rows updated in the same commits as code
- [ ] analysis sections touched by the diff re-verified (file:line cites against new tag)
- [ ] open-questions triaged: new ones filed, resolved ones moved to Resolved
- [ ] `./build.sh Release --test` clean; VM acceptance suite green under Verifier
- [ ] master analysis header + this guide's pinned tag updated
