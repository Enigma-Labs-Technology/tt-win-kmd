# Real-silicon bring-up (p150a) — host-side scripts

Phased, checkpointed scripts for the FIRST run of ttkmd.sys on physical
Blackhole silicon. Everything here runs ELEVATED on the physical Windows host
(unlike `test/vm/`, which drives the QEMU/ttsim rig). Strict order; stop at
the first failed checkpoint.

| Phase | Script | Gate it proves |
|---|---|---|
| 0 | `00-preflight.ps1` | restore point; BitLocker safe; Secure Boot OFF; testsigning ON |
| 1 | `10-remount-card.ps1` | card returned from Hyper-V DDA (`PCIP\...`) to host PCI tree |
| 2 | `20-install-driver.ps1` | cert trusted; ttkmd bound + started on the REAL card |
| 3 | `30-verifier-arm.ps1` | Verifier 0x9BB armed with the device disabled (no bugcheck loop) |
| 4 | `40-first-contact.ps1` | device starts under Verifier; read-only rungs pass; no WHEA |

Then climb the ladder with ttinfo.exe selectors (least → most destructive):
`--only tlb` → `--only dma` → `--only pin` → `--reset restore-then-post` →
`--reset config-write` → `--reset asic-dmc` → `--reset pcie-link`, with a
heartbeat health check between rungs (ttinfo does this itself after resets).
Record every step in `docs/test-reports/real-silicon.md`.

## Hard preconditions for ANY reset rung (f–i)

- Card idle: no user mappings, no other clients (`ttinfo` default run shows
  open state); Verifier ON.
- Recovery staged: a warm reboot may NOT clear a wedged ASIC (PERST# is only
  re-asserted on a full power cycle), and firmware reflash (`tt-flash`) is
  LINUX-ONLY. Have the Linux boot/box ready before the first reset.
- Wedge signature: config vendor reads FFFF / NOC reads 0xFFFFFFFF / heartbeat
  frozen. Escalation: warm reboot → full power-off (PSU off ~10 s) → Linux +
  tt-flash. Record any wedge in the report's divergence log.

## Ground truth first

Before trusting any Windows telemetry value, capture the same card under Linux
tt-kmd: `tt-smi -s`, the `tt_*` sysfs attributes, hwmon channels, `lspci -vv`
(BAR sizes, MPS/MRRS), and note the FW bundle. The cross-check table in the
report template maps every field.
