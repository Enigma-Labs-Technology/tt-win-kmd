# ttkmd test rig

Windows 11 guest under QEMU/KVM inside WSL2, with test signing + Driver Verifier;
from M1 it gains a ttsim-backed virtual Blackhole PCIe endpoint (`test/qemu-ttsim/`).

## One-time host setup (requires sudo — the only manual step)

```bash
sudo apt-get update
sudo apt-get install -y qemu-system-x86 qemu-utils genisoimage ovmf socat
# /dev/kvm access: either add yourself to the kvm group (re-login) or run qemu via sudo
sudo usermod -aG kvm "$USER"
```

Verified present already: `/dev/kvm` (WSL2 nested virtualization),
Windows ISO at `/mnt/c/VMs/iso/Win11_24H2_English_x64.iso`.

## Create the VM (unattended, ~20–60 min)

```bash
cd test/vm
./make-vm.sh          # builds unattend.iso, installs Windows, provisions:
                      #   testsigning on, OpenSSH server, autologon ttdev
# progress: VNC on 127.0.0.1:5907 (e.g. wsl: vncviewer, or Windows-side TigerVNC)
# done when:  ssh -p 2222 ttdev@localhost "type C:\tt\provisioned.txt"
```

Credentials (lab-only, VM bound to 127.0.0.1): `ttdev` / `ttdev!Lab1`.

After install completes take a clean snapshot:

```bash
qemu-img snapshot -c clean win11.qcow2
```

## Day-to-day

```bash
cd test/vm && ./run-vm.sh &                         # boot
ssh -p 2222 ttdev@localhost                          # shell (powershell default)
scp -P 2222 -r ../../out/Release ttdev@localhost:C:/tt/drop   # push driver drop
```

Driver drop must also contain `devgen.exe` (copy once from the EWDK:
`D:\Program Files\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe`).

## M0 acceptance sequence

```bash
ssh -p 2222 ttdev@localhost "powershell -ExecutionPolicy Bypass -File C:/tt/drop/install-driver.ps1"
ssh -p 2222 ttdev@localhost "powershell -ExecutionPolicy Bypass -File C:/tt/drop/verifier-on.ps1"
# wait for reboot, then:
ssh -p 2222 ttdev@localhost "powershell -ExecutionPolicy Bypass -File C:/tt/drop/loadtest.ps1"
# PASS = JSON with "pass": true and zero bad events; record in docs/test-reports/M0.md
```

## ttsim glue (M1, `test/qemu-ttsim/` — not yet implemented)

Custom QEMU PCIe device `ttsim-bh` that dlopens `libttsim.so` (built from
`../../ttsim`), forwards BAR access to `libttsim_pci_mem_rd/wr_bytes`, implements
config space in QEMU (ttsim config writes are fatal — DD-5), maps DMA callbacks to
`pci_dma_read/write`, and pumps `libttsim_clock` from a timer. Attach with
`EXTRA_DEVICES="-device ttsim-bh,ttsim-lib=..." ./run-vm.sh`. BAR sizes come from
the Blackhole BAR table in `docs/linux-driver-analysis.md` (OQ-1).

## Known design risks

- Unattended-install bypass keys (`LabConfig`) are version-sensitive; if setup
  stalls, watch via VNC and adjust `autounattend.xml`.
- The "Press any key to boot from CD" UEFI prompt is auto-acked by `make-vm.sh`
  via QMP `send-key` for the first 40 s.
- libttsim is single-threaded and non-reentrant: all glue calls stay under the
  QEMU BQL; clock pumping must be tuned against tt-kmd's ARC timeout constants
  (analysis §09) so firmware messages don't spuriously time out.
