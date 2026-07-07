# 12. KMD Tests and Tools (test/, tools/, docs/, contrib/)

## Scope

Files covered (all read in full; line counts from `wc -l`):

Test suite (`test/`):
- test/main.cpp (77), test/Makefile (37), test/.gitignore (2)
- test/enumeration.h (28), test/enumeration.cpp (167)
- test/devfd.h (20), test/devfd.cpp (30)
- test/util.h (72), test/util.cpp (210)
- test/test_failure.h (27), test/test_failure.cpp (14)
- test/aligned_allocator.h (38)
- test/get_driver_info.cpp (71), test/get_device_info.cpp (66)
- test/config_space.cpp (165), test/query_mappings.cpp (262)
- test/dma_buf.cpp (175), test/pin_pages.cpp (447)
- test/lock.cpp (490), test/hwmon.cpp (93)
- test/ioctl_overrun.cpp (212), test/ioctl_zeroing.cpp (107)
- test/map_peer_bar.cpp (167), test/tlbs.h (117), test/tlbs.cpp (634)
- test/dmabuf_export.cpp (350), test/release.cpp (126)
- test/mappings_debugfs.cpp (318), test/procfs_pids.cpp (164)
- test/excl.cpp (332)
- test/checkws (38), test/checkws-tests (8), test/pahole_check.sh (125)
- test/run-hardware-tests.sh (152), test/mass-build-test (163)

Tools (`tools/`):
- tools/reset.c (294), tools/power.c (283)
- tools/rdma/nic_bh_p2p_dma.c (657), tools/rdma/Makefile (15)
- tools/fix-tt-hotplug-bars (111)
- tools/current-version (28), tools/exclude-from-release (8)
- tools/installer-header.sh (35), tools/make-installer (60), tools/make-source-release (67)
- tools/build_debs.sh (154), tools/build_rpms.sh (192)

Docs (`docs/`):
- docs/sysfs-attributes.md (150)

Contrib (`contrib/`):
- contrib/packaging/MAINTAINERS.md (5), contrib/packaging/README.md (9)
- contrib/packaging/nix/ci.sh (11), contrib/packaging/nix/overlay.nix (32)

The test suite (`ttkmd_test`) is the closest thing tt-kmd has to executable ABI documentation. Each test below is enumerated with the exact semantics it asserts, including error codes, so a Windows port can treat these as the conformance contract.

---

## 1. Test harness structure

### 1.1 Build and entry point

`ttkmd_test` is a single C++17 userspace binary built with `-O2 -Wall -Wno-narrowing` (test/Makefile:19-20). It includes the driver's `ioctl.h` directly (e.g. test/get_driver_info.cpp:9), so it is compiled against the exact uAPI header of the driver under test.

`main()` (test/main.cpp:30-77):
- Accepts one optional argument `--skip-aer`, which disables the AER check because "When running inside a VM aer seems to be disabled" (test/main.cpp:34-37).
- Enumerates devices and runs, per device, in order: `TestGetDriverInfo, TestGetDeviceInfo, TestConfigSpace, TestQueryMappings, TestDmaBuf, TestNocDmaBuf, TestPinPages, TestLock, TestHwmon, TestIoctlOverrun, TestIoctlZeroing, TestTlbs, TestTlbExport, TestMappingsDebugfs, TestProcfsPids, TestExcl, TestDeviceRelease` (test/main.cpp:44-60).
- Then runs `TestMapPeerBar(devs[i], devs[j])` over the full cartesian product of devices, *including i==j* (test/main.cpp:65-71).
- Fails if no device was found: `THROW_TEST_FAILURE("No devices found.")` (test/main.cpp:73-74).

Failures are reported by throwing `test_failure` (a `std::runtime_error` carrying file/line/function; test/test_failure.h:9-27) — the first failure aborts the whole run (no catch in main).

### 1.2 Device enumeration (itself a test)

`EnumerateDevices()` (test/enumeration.cpp:142-167) cross-checks the driver's userspace surface:
- Every character-device entry in `/dev/tenstorrent/` must have a sysfs `subsystem` link resolving to `tenstorrent` (test/enumeration.cpp:30-36, 46-59); non-char-device entries are silently skipped (test/enumeration.cpp:53-54). A char device there that is *not* bound to the tenstorrent driver is a failure (test/enumeration.cpp:56-57).
- Every PCI device with vendor ID `0x1E52` (test/enumeration.cpp:82) must have exactly one `tenstorrent/` class-device subdirectory containing a `dev` file with `MAJOR:MINOR` (test/enumeration.cpp:93-109). Zero nodes (test/enumeration.cpp:94-95) or more than one (test/enumeration.cpp:97-98) is a failure.
- The set of dev_t from `/dev/tenstorrent` must exactly equal the set derived from PCI sysfs: "PCI devices and driver-reported devices do not match." (test/enumeration.cpp:155-156).
- Device type is determined by PCI device ID: `0x401e → Wormhole`, `0xb140 → Blackhole`; any other DID is a hard error (test/enumeration.cpp:133-139).
- IOMMU translation state is detected by reading `/sys/bus/pci/devices/<bdf>/iommu_group/type` and checking for a `"DMA"` prefix (test/enumeration.cpp:115-126); failure to read means "not translated". This flag changes expected PIN_PAGES semantics (section 2.7).

> **Porting note:** On Windows the enumeration layer maps to SetupDi/CM_* interface enumeration by device interface GUID plus querying bus/device/function via `DEVPKEY_Device_BusNumber`/`Address`. The invariant to preserve: every TT PCI function has exactly one user-visible device object, and device type is derived from PCI DID (0x401e/0xb140). The IOMMU-translated bit maps to whether DMA remapping (IOMMU/DMA guard) is active for the device — the port must expose or internally know the equivalent because PIN_PAGES contiguity rules depend on it.

### 1.3 Support utilities

- `DevFd` opens the node `O_RDWR | O_CLOEXEC` and closes on destruction (test/devfd.cpp:13-24).
- `util.cpp` provides sysfs helpers, `page_size()` via `sysconf(_SC_PAGE_SIZE)` (test/util.cpp:140-143), an unlinked temp file (test/util.cpp:156-178), and an anonymous POSIX shm object (test/util.cpp:194-210, used to construct physically-discontiguous mappings).
- `AlignedAllocator` wraps `std::aligned_alloc` for placing ioctl structs at controlled alignment (test/aligned_allocator.h:10-38).

---

## 2. Per-test ABI semantics (executable ABI documentation)

### 2.1 TestGetDriverInfo (test/get_driver_info.cpp)

Asserts, for `TENSTORRENT_IOCTL_GET_DRIVER_INFO` with `in.output_size_bytes = sizeof(out)`:
- ioctl returns 0 (test/get_driver_info.cpp:42-43).
- `out.output_size_bytes >= offsetof(out, driver_version) + sizeof(driver_version)` — the driver reports at least up through the legacy version field (test/get_driver_info.cpp:45-50).
- `out.output_size_bytes <= sizeof(out)` — the driver never claims more output than the current header defines (test/get_driver_info.cpp:52-53).
- `out.driver_version == TENSTORRENT_DRIVER_VERSION` (interface version 2; ioctl.h:10) (test/get_driver_info.cpp:55-56).
- `driver_version_major/minor/patch` must equal the semver parsed from `/sys/module/tenstorrent/version` (test/get_driver_info.cpp:58-70); parsing uses the full semver.org regex (test/get_driver_info.cpp:21).

> **Porting note:** The cross-check source (`/sys/module/tenstorrent/version`) is Linux-specific; a Windows conformance test would compare against the driver file version resource or a registry value. The ioctl-side assertions port directly.

### 2.2 TestGetDeviceInfo (test/get_device_info.cpp)

For `TENSTORRENT_IOCTL_GET_DEVICE_INFO`:
- ioctl returns 0; `out.output_size_bytes >= offsetof(out, pci_domain)+sizeof(pci_domain)` ("pci_domain has been present since 1.23", test/get_device_info.cpp:26-32).
- `vendor_id`, `device_id`, `subsystem_vendor_id`, `subsystem_id` must match PCI config space (read via sysfs) (test/get_device_info.cpp:34-50).
- BDF encoding contract (test/get_device_info.cpp:52-59):
  ```c
  unsigned bus = (get_device_info.out.bus_dev_fn >> 8) & 0xFF;
  unsigned device = (get_device_info.out.bus_dev_fn >> 3) & 0x1F;
  unsigned function = get_device_info.out.bus_dev_fn & 0x7;
  unsigned domain = get_device_info.out.pci_domain;
  ```
- `12 <= max_dma_buf_size_log2 <= 63` (test/get_device_info.cpp:61-65).

### 2.3 TestConfigSpace (test/config_space.cpp)

Verifies driver-programmed PCI config state by reading `/sys/bus/pci/devices/<bdf>/config`:
- `Command.MemorySpaceEnable` (bit 1) and `Command.BusMasterEnable` (bit 2) at config offset 4 must both be set (test/config_space.cpp:22-24, 88-98).
- MSI capability (cap ID 5) must exist, be enabled (`message_control & 1`), and have a nonzero message address (accounting for 64-bit addressing via `message_control & 0x80`) (test/config_space.cpp:31-36, 100-118).
- PCIe capability (cap ID 0x10): at Device Control (offset 8 into the cap), at least one of correctable/non-fatal/fatal/unsupported error-reporting-enable bits (0x1|0x2|0x4|0x8) must be set — "AER is disabled." otherwise; skipped with `--skip-aer` (test/config_space.cpp:38-43, 120-138).
- If the kernel rejects config reads beyond 64 bytes for non-root, the MSI/AER checks are skipped with a console message (test/config_space.cpp:148-164).

> **Porting note:** These are assertions about what the *driver* must program (memory space, bus mastering, MSI, AER), not about the ioctl surface. On Windows most of this is done by the PCI bus driver/KMDF (`WdfDeviceQueryInterruptProperty`, MSI configured via INF/interrupt resources); the conformance equivalent is verifying the device is receiving interrupts and BME is on.

### 2.4 TestQueryMappings (test/query_mappings.cpp)

The test's own header comment enumerates the contract (test/query_mappings.cpp:4-10). Query procedure: caller passes `in.output_mapping_count`; the buffer is `sizeof(tenstorrent_query_mappings) + count*sizeof(tenstorrent_mapping)` (test/query_mappings.cpp:182-199); the test doubles count starting at 16 until the last returned entry is `TENSTORRENT_MAPPING_UNUSED` (test/query_mappings.cpp:201-213). Asserted invariants:
- Only known mapping IDs appear: `UNUSED`, `RESOURCE{0,1,2}_UC`, `RESOURCE{0,1,2}_WC` (test/query_mappings.cpp:35-53).
- All `UNUSED` entries are at the end of the output array (test/query_mappings.cpp:55-63).
- No non-UNUSED mapping ID appears more than once (test/query_mappings.cpp:65-73).
- `RESOURCE0_UC` is always present (test/query_mappings.cpp:75-79).
- If `RESOURCEi_WC` is present, `RESOURCEi_UC` must also be present (test/query_mappings.cpp:81-101).
- Mappings must not overlap and must not wrap around 2^64 (test/query_mappings.cpp:104-129).
- Every non-UNUSED mapping: `mapping_size > 0`, `mapping_size % page_size == 0`, `mapping_base % page_size == 0` (test/query_mappings.cpp:133-147).
- `mapping_base + mapping_size < (1ULL << 44)` — "32 + log(PAGE_SIZE)" so a 32-bit `mmap` offset (in pages) can address every mapping (test/query_mappings.cpp:153-157).
- Prefix property: querying with `output_mapping_count = i` for every i up to the full count must return exactly the first i entries of the full result (id, base, and size identical) (test/query_mappings.cpp:215-229).
- Every non-UNUSED mapping must be mmap-able `PROT_READ|PROT_WRITE, MAP_SHARED` at `offset = mapping_base`, `length = mapping_size`, and munmap-able (test/query_mappings.cpp:231-245).

Note: `VerifyNoOverlap` builds a base-sorted copy (`base_sorted_mappings`) but then iterates the *unsorted* `mappings` vector in the overlap loop (test/query_mappings.cpp:114-128) — apparently a test bug; the intended invariant is clearly "no overlap among non-UNUSED mappings".

> **Porting note:** This is the core mmap-offset contract. A Windows port that keeps `QUERY_MAPPINGS` must keep all these invariants, with `mmap` replaced by whatever mapping call the port defines (e.g. an ioctl that returns a user VA, or a section object). The 2^44 limit exists for 32-bit Linux mmap offsets; a Windows port should decide whether to preserve it (harmless) or document divergence.

### 2.5 TestDmaBuf / TestNocDmaBuf (test/dma_buf.cpp)

For `TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF`:
- Allocation of index 0 at up to `1 << max_dma_buf_size_log2` bytes must succeed; on `ENOMEM` the test halves the size and retries down to one page (test/dma_buf.cpp:31-77) — i.e. `ENOMEM` is the expected error for allocation failure.
- Re-allocating an already-used `buf_index` fails with exactly `EINVAL` (test/dma_buf.cpp:144-150).
- One page can be allocated at *every* index `1..TENSTORRENT_MAX_DMA_BUFS-1` (256 buffers total; ioctl.h:41) (test/dma_buf.cpp:152-164).
- `buf_index == TENSTORRENT_MAX_DMA_BUFS` fails with exactly `EINVAL` (test/dma_buf.cpp:79-89).
- Every allocated buffer is mmap-able at `out.mapping_offset` for `out.size` bytes, `PROT_READ|PROT_WRITE, MAP_SHARED`; memory is writable and retains data across per-buffer mappings (test/dma_buf.cpp:91-116).
- `TestNocDmaBuf`: two buffers with `flags = TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA` (=2, ioctl.h:89) at indices 0 and 1 must both succeed on a fresh fd (test/dma_buf.cpp:118-127, 171-175) — i.e. multiple NOC-visible (iATU-mapped) buffers must coexist.

### 2.6 TestIoctlOverrun (test/ioctl_overrun.cpp)

Technique: place the ioctl input at the very end of a page followed by a `PROT_NONE` guard page (test/ioctl_overrun.cpp:31-86). Contract asserted (header comment, test/ioctl_overrun.cpp:1-9):
- For ioctls with an `output_size_bytes` field, passing `output_size_bytes = 0` with only the *in* struct mapped must produce **no output written and no error** — any `EFAULT` means the driver read or wrote beyond what userspace provided (test/ioctl_overrun.cpp:92-106).
- Applied with expected success (errno 0): `GET_DEVICE_INFO`, `QUERY_MAPPINGS` (with `output_mapping_count = 0`), `ALLOCATE_DMA_BUF` (full struct at page end), `GET_DRIVER_INFO`, `RESET_DEVICE` (flags `TENSTORRENT_RESET_DEVICE_RESTORE_STATE`), `PIN_PAGES`, `LOCK_CTL` (TEST op) (test/ioctl_overrun.cpp:108-178).
- `FREE_DMA_BUF` is expected to fail with `EINVAL` but must not `EFAULT` (test/ioctl_overrun.cpp:134-139).
- `MAP_PEER_BAR` with `peer_fd = <same fd>` is expected to fail with `EINVAL` but must not `EFAULT` (test/ioctl_overrun.cpp:180-194).
- `GET_HARVESTING` is skipped: "simply fails" (test/ioctl_overrun.cpp:203).

> **Porting note:** This maps naturally to METHOD_BUFFERED vs METHOD_NEITHER decisions. The Linux driver copies exactly `output_size_bytes` out; a KMDF port using METHOD_BUFFERED gets kernel-managed buffering, but the *semantic* contract — output truncated to caller-specified `output_size_bytes`, never touching memory beyond it — must be preserved because UMD passes these sizes. The guard-page test technique ports directly via `VirtualAlloc` + `PAGE_NOACCESS`.

### 2.7 TestIoctlZeroing (test/ioctl_zeroing.cpp)

Contract (test/ioctl_zeroing.cpp:1-2): "When the actual output data is smaller than output_size_bytes, the remainder must be zero-filled." Technique: buffer of `offsetof(IoctlData, out) + page_size()` filled with `0xFF`; after the ioctl, every byte past `sizeof(IoctlData)` must be zero (test/ioctl_zeroing.cpp:28-42). Applied to: `GET_DEVICE_INFO`, `GET_DRIVER_INFO`, `RESET_DEVICE` (RESTORE_STATE), `PIN_PAGES`, `LOCK_CTL(TEST)` with `output_size_bytes = page_size()` (test/ioctl_zeroing.cpp:44-89). Explicit exclusions documented in the test: GET_HARVESTING "simply fails"; QUERY_MAPPINGS "complicated, has its own test"; ALLOCATE_DMA_BUF, FREE_DMA_BUF, MAP_PEER_BAR "does not zero" (test/ioctl_zeroing.cpp:98-106).

### 2.8 TestPinPages (test/pin_pages.cpp)

Contract summary in header comment (test/pin_pages.cpp:4-11). For `TENSTORRENT_IOCTL_PIN_PAGES`:
- flags 0 and `TENSTORRENT_PIN_PAGES_CONTIGUOUS` (=1, ioctl.h:169) each succeed pinning a single page (test/pin_pages.cpp:49-73).
- `flags = ~TENSTORRENT_PIN_PAGES_CONTIGUOUS` must fail (returns -1; errno not asserted) (test/pin_pages.cpp:75-97).
- `size == 0` and `size == page_size/2` must fail (test/pin_pages.cpp:99-135) — size must be a nonzero page multiple.
- Pinning an unmapped VA range, or a range that is only partially mapped, must fail (test/pin_pages.cpp:137-179).
- 1024 separate single-page pins on one fd must all succeed (`max_pinned_ranges = 1024`, test/pin_pages.cpp:181-206).
- A hugepage (any size found under `/sys/kernel/mm/hugepages`) pins successfully with `CONTIGUOUS` (test/pin_pages.cpp:208-258); skipped with a message if no hugepages can be allocated.
- Discontiguity check (test/pin_pages.cpp:260-352): two shm pages are mapped twice, second time in swapped order, so at most one of the two orderings can be physically contiguous. Flags used: `0` when IOMMU-translated, `CONTIGUOUS` otherwise (test/pin_pages.cpp:301).
  - IOMMU on: **both** pins must succeed — discontiguous pinning is allowed (test/pin_pages.cpp:334-345).
  - IOMMU off: **at most one** may succeed; both may fail (test/pin_pages.cpp:347-351).
- `TENSTORRENT_IOCTL_UNPIN_PAGES`: unpinning with exact `virtual_address`/`size` of a prior pin succeeds (test/pin_pages.cpp:356-384); `size = 0`, `size = page_size/2` and `size = page_size*2` (superset of the pinned range) must all fail (test/pin_pages.cpp:386-434) — unpin must match the pinned region exactly.

Note test/pin_pages.cpp:270: "6.8 fails to pin temporary files but works with shared memory objects" — the pinnability of file-backed pages varies by kernel; the ABI contract is only asserted for anonymous/shm memory.

### 2.9 TestLock (test/lock.cpp)

For `TENSTORRENT_IOCTL_LOCK_CTL` (ops: ACQUIRE=0, RELEASE=1, TEST=2, ACQUIRE_BLOCKING=3; ioctl.h:211-214). Result convention: `out.value != 0` = acquired/released; TEST returns `bit0 = held by this fd (LOCK_LOCAL=0b01)`, `bit1 = held by anyone (LOCK_GLOBAL=0b10)` (test/lock.cpp:27-28). Assertions (test/lock.cpp:97-208):
1. Acquire then release works (out.value=1 both times).
2. Releasing an unheld lock returns out.value=0 (not an ioctl error).
3. An fd cannot release a lock held by a different fd (returns 0).
4. Locks are **not re-entrant**: second acquire on the same fd returns 0 (test/lock.cpp:120-126).
5. Locks are exclusive across fds.
6. TEST: holder sees `LOCK_LOCAL|LOCK_GLOBAL` (=3); non-holder sees `LOCK_GLOBAL` (=2) (test/lock.cpp:136-149).
7. Lock indices are independent.
8. Closing the fd auto-releases its locks; the lock is then free (`TEST == 0`) and acquirable by others (test/lock.cpp:161-184).
- Bounds: `index == TENSTORRENT_RESOURCE_LOCK_COUNT` (64; ioctl.h:44) fails with ioctl error and `errno == EINVAL`; index 63 works (test/lock.cpp:187-208).
- One fd can hold all 64 locks simultaneously (test/lock.cpp:210-232).
- `ACQUIRE_BLOCKING` blocks while another fd holds the lock and wakes when it is released (verified with a thread and a 50ms probe; test/lock.cpp:234-278). The semantics satisfy C++ `BasicLockable`/`Lockable` (test/lock.cpp:280-356).
- Process exit (child `_exit` without release) frees the lock (test/lock.cpp:358-385).
- A blocking acquire in one process **wakes when the holding process exits** — fd cleanup must wake waiters (test/lock.cpp:387-424; comment: "This tests that wake_up_interruptible is called during fd cleanup", test/lock.cpp:388).
- SA_RESTART semantics: a blocking acquire interrupted by a signal whose handler has `SA_RESTART` is transparently restarted; the kernel path is `-ERESTARTSYS` internally (documented in the test comment, test/lock.cpp:457-464). The handler releases the lock; the restarted ioctl then acquires and returns success (test/lock.cpp:428-476).

> **Porting note:** The lock array is pure driver state — 64 per-device flags with per-handle ownership, blocking waits, and wake-on-handle-cleanup. On Windows: KEVENT/wait queue keyed per lock index; `EvtFileCleanup` must release all locks held by the file object *and* wake waiters. The SA_RESTART/ERESTARTSYS test is Linux-signal-specific; the Windows analogue is alertable waits + `CancelSynchronousIo`/IRP cancellation — the blocking LOCK_CTL IRP must be cancelable, and cancellation must not leak lock state.

### 2.10 TestHwmon (test/hwmon.cpp)

Skipped when `/sys/bus/pci/devices/<bdf>/hwmon` doesn't exist (test/hwmon.cpp:78-79). Otherwise, in the `hwmonX` subdirectory:
- Label files must match: `curr1_label` ~ `current[0-9]*`, `in0_label` ~ `vcore[0-9]*`, `temp1_label` ~ `asic[0-9]*_temp`, `power1_label` ~ `power[0-9]*` (test/hwmon.cpp:14-31).
- On Wormhole only (`dev.type < Blackhole`, test/hwmon.cpp:89-90): each of `in0_input`, `curr1_input`, `temp1_input`, `power1_input` must parse as an integer strictly less than the corresponding `*_max` (test/hwmon.cpp:34-68).

> **Porting note:** hwmon is a Linux subsystem; a Windows port would surface telemetry via WMI/IOCTL instead. The load-bearing fact is the sensor set (voltage, current, ASIC temperature, power) and that per-sensor max values exist on Wormhole.

### 2.11 TestMapPeerBar (test/map_peer_bar.cpp)

For `TENSTORRENT_IOCTL_MAP_PEER_BAR` run over every ordered device pair:
- Same device (two fds to the same node): must fail (returns -1; errno not asserted) (test/map_peer_bar.cpp:110-125, dispatch test/map_peer_bar.cpp:153-156).
- Different chip generations (different PCI DIDs): must fail (test/map_peer_bar.cpp:127-142, 157-160).
- Same-type distinct devices: mapping each *memory* BAR of the peer with `peer_bar_offset = 0` and `peer_bar_length = min(bar_size, 0xFFFFF000)` must succeed ("Cap to the largest page-aligned size the u32 ABI field can hold", test/map_peer_bar.cpp:96-97) (test/map_peer_bar.cpp:83-108). BAR geometry is read from the undocumented sysfs `resource` file, with flags decoded per include/linux/ioport.h values 0x100 (IO), 0x200 (memory), 0x2000 (prefetchable) (test/map_peer_bar.cpp:37-81).
- Overrun variant asserts `EINVAL` when `peer_fd` refers to the same device (test/ioctl_overrun.cpp:180-194).

> **Porting note:** `peer_fd` is a file descriptor identifying the peer device's open handle. A Windows port needs a handle-based equivalent (e.g. passing a HANDLE and resolving via `ObReferenceObjectByHandle` to confirm it is a tenstorrent file object) — the tests demand the driver be able to identify "same underlying device" and "different chip type" from that handle.

### 2.12 TestTlbs (test/tlbs.cpp, test/tlbs.h)

TLB window helper (test/tlbs.h:19-117): `ALLOCATE_TLB(size)` → `{id, mmap_offset_uc, mmap_offset_wc}`; `CONFIGURE_TLB{id, config{addr, x_end, y_end, ...}}`; mmap of the UC offset; `FREE_TLB{id}` on destruction. Window sizes: `ONE_MEG=1<<20`, `TWO_MEG=1<<21`, `SIXTEEN_MEG=1<<24`, `FOUR_GIG=1ULL<<32` (test/tlbs.h:109-112).

Wormhole assertions:
- Window inventory: "Wormhole has 156x 1M, 10x 2M, and 20x 16M windows; all but the last 16M window should be available" (test/tlbs.cpp:98-99). The test allocates 156×1M, 10×2M, 19×16M successfully; the 20th 16M allocation must fail ("The last 16M window should be off-limits to userspace", test/tlbs.cpp:135-142); then frees all.
- Node-ID readback through windows of all three sizes: ARC tile at (x=0,y=10) via NOC address `0xFFFB2002C`, DDR at (0,11) via `0x10009002C`; node id register encodes `x = bits[5:0]`, `y = bits[11:6]` (test/tlbs.cpp:88-96, 167-186).
- 184 simultaneously-configured windows (156+10+18) all pointing at the same DRAM address read back identical random data written through a 185th window (test/tlbs.cpp:188-233).
- CONFIGURE_TLB rejections: `addr` not aligned to the window size (`size/2`) must fail; `addr = 1ULL << 36` ("Addresses must fit in 36 bits") must fail (test/tlbs.cpp:235-291). Errno is not asserted, only nonzero return.

Blackhole assertions:
- "Blackhole has 202x 2M and up to 8x 4G windows" (test/tlbs.cpp:293-295): 201×2M allocations succeed, the 202nd must fail (last 2M window kernel-reserved, test/tlbs.cpp:312-319); number of 4G windows = `st_size(resource4)/4GiB` (test/tlbs.cpp:70-85) and exactly that many 4G allocations succeed.
- NOC translation detection: BAR0 is mmapped (512MiB, `BAR0_SIZE = 1<<29`) and `NIU_CFG` is read at BAR0 offset `0x1FD04100`; bit 14 = translation enabled (test/tlbs.cpp:22-45).
- Tensix node-id sweep over a 17×12 grid (`x in [1,7]∪[10,16], y in [2,11]`) at NOC register `0xffb20148`, via 2M and (if present) 4G windows (test/tlbs.cpp:358-409).
- PCI tile node-id readback at `0xFFFFFFFFFF000148`: tile (19,24) when translated, (2,0) when not; ARC tile is (8,0) at register `0x0000000080050044` regardless of translation (test/tlbs.cpp:411-441).
- 200 simultaneous 2M windows aimed at DRAM ((17,12) translated / (0,0) untranslated) read back a pattern written through a 201st (test/tlbs.cpp:443-475).
- CONFIGURE_TLB misaligned-address rejection as on Wormhole (no 36-bit check on Blackhole) (test/tlbs.cpp:477-521).

Both device types:
- **Partial unmap of a TLB window must fail**: after mmapping a 2M window, `munmap` of each individual 4K page must fail (the Linux driver forbids VMA splits via a `.may_split`/`.split` vm_ops hook that returns `-EINVAL`, memory.c:1478-1490; the test asserts each per-page `munmap` returns nonzero) (test/tlbs.cpp:523-548). If `mremap` of a page succeeds (kernel-version dependent: "fails on 5.15.0 (fine), succeeds on 5.4.0"), the remapped page's reference must prevent `FREE_TLB` until it is unmapped (test/tlbs.cpp:550-577).
- **A window that is mmapped cannot be freed**: `FREE_TLB` fails while a mapping exists, succeeds after `munmap` (test/tlbs.cpp:581-605).

> **Porting note:** The reference counting contract — outstanding user mappings pin a TLB window; FREE_TLB fails (nonzero) while mapped — must hold on Windows via MDL/section lifetime tracking. "Partial unmap must fail" is enforced by the driver's `.may_split` rejection (memory.c:1478-1490); on Windows, mapping the window as a single section view naturally gives all-or-nothing unmap.

### 2.13 TestTlbExport (test/dmabuf_export.cpp)

For `TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF` (struct: `{argsz, flags, tlb_id, fd, offset, size}`):
- Probe: export of `{offset=0, size=0}` on a fresh window; `EOPNOTSUPP` means kernel < 5.8 support absent and the whole test is skipped (test/dmabuf_export.cpp:56-73, 335-343).
- Basic export of a whole window succeeds and yields a valid dma-buf fd (test/dmabuf_export.cpp:76-91). (`size=0` in the basic path exports successfully, implying size 0 = whole window.)
- `EINVAL` cases, each asserted exactly (test/dmabuf_export.cpp:94-154):
  - `argsz != sizeof(struct)` (test uses `sizeof+1`)
  - `flags != 0`
  - out-of-range `tlb_id` (0xFFFFFFFF)
  - `offset` not page-aligned (0x1)
  - `size` not page-aligned (0x1)
  - `offset >= window size` (offset = TWO_MEG on a 2M window)
- Ownership: exporting a window allocated on another fd fails with exactly `EPERM` (test/dmabuf_export.cpp:157-178).
- Lifetime, part 1 (test/dmabuf_export.cpp:185-259): with the 2M pool exhausted, `FREE_TLB` on an exported window *succeeds* (refcount model), but the window must **not** become allocatable until the dma-buf fd is closed; closing the dma-buf returns it to the pool.
- Lifetime, part 2 (test/dmabuf_export.cpp:266-331): closing the owning chardev fd (without FREE_TLB) while the export is live must not tear down the window; the dma-buf keeps it allocated; only closing the dma-buf frees it (observed from a second fd via pool exhaustion).

> **Porting note:** dma-buf is Linux-only. If the Windows port needs the equivalent (peer-to-peer DMA into a TLB window by another driver, cf. tools/rdma), the closest analogues are NtCreateSection-based shared mappings or a bus-interface contract; the *lifetime rules* (export holds a reference independent of the owning handle) are the part to preserve.

### 2.14 TestDeviceRelease (test/release.cpp)

For `TENSTORRENT_IOCTL_SET_NOC_CLEANUP` (struct `{argsz, flags, enabled, x, y, noc, reserved0, addr, data}`; ioctl.h:349-359):
- After arming `{enabled=true, data=0xDEADBEEF, x, y, addr}` and closing the fd, a fresh fd reading `(x,y,addr)` through a 2M TLB window must observe `0xDEADBEEF` — the driver performs the 32-bit NOC write during release (test/release.cpp:18-53).
- Arming then disarming (`enabled=false`) must leave the prior memory content (0x0DDBA115) untouched after close (test/release.cpp:55-91).
- Target tiles: Wormhole DRAM (0,0) addr 0 (test/release.cpp:93-98); Blackhole DRAM (17,12) translated / (0,0) untranslated (test/release.cpp:100-109).

> **Porting note:** This is a per-handle cleanup action; on Windows it belongs in `EvtFileCleanup` (guaranteed to run at last handle close, including process termination).

### 2.15 TestMappingsDebugfs (test/mappings_debugfs.cpp)

Reads `/sys/kernel/debug/tenstorrent/<ordinal>/mappings` (path built from the /dev node name, test/mappings_debugfs.cpp:34-44); skipped if not readable (e.g. non-root or debugfs unmounted, test/mappings_debugfs.cpp:303-307). Asserts textual content:
- Header contains "WARNING: This file is for diagnostic purposes only" and "not stable"; columns "PID", "Comm", "Type", "Mapping Details" (test/mappings_debugfs.cpp:60-83).
- Entry type strings, each triggered by the corresponding operation: `OPEN_FD` (open fd + caller PID visible), `PIN_PAGES`, `PIN_PAGES+IATU` (pin with `TENSTORRENT_PIN_PAGES_NOC_DMA` flag =2), `DMA_BUF` (+"ID: 0"), `DMA_BUF+IATU` (NOC_DMA alloc, +"ID: 2"), `BAR` (after mmap of BAR0 UC at offset 0), `TLB` (after ALLOCATE_TLB) (test/mappings_debugfs.cpp:85-249).
- Multiple resource types coexist in one read (test/mappings_debugfs.cpp:251-295).

### 2.16 TestProcfsPids (test/procfs_pids.cpp)

Reads `/proc/driver/tenstorrent/<ordinal>/pids` (test/procfs_pids.cpp:24-34); skipped if absent. Asserts:
- Format: one PID per line, strictly numeric, positive (test/procfs_pids.cpp:48-82).
- The caller's PID appears while it holds an open fd and disappears after close (test/procfs_pids.cpp:84-121).
- One entry **per open fd**: 3 fds ⇒ PID listed exactly 3 times (test/procfs_pids.cpp:123-147).

> **Porting note:** debugfs/procfs diagnostics have no direct Windows equivalent; candidates are a diagnostic IOCTL, WMI, or `!ttkmd` debugger extension. The functional requirement is: the driver tracks, per open handle, the owning PID/process name and every live resource (pins, DMA bufs, BAR mappings, TLBs, iATU regions) — needed anyway for cleanup.

### 2.17 TestExcl (test/excl.cpp)

Open-time reader/writer lock semantics, documented in the test header (test/excl.cpp:4-10): "O_EXCL is the writer, plain opens are readers, and O_NONBLOCK selects trylock behavior... Blocking waiters wake when open_fds_list becomes empty (i.e. on the last release)." Assertions:
- Two plain opens coexist (test/excl.cpp:53-67).
- O_EXCL on an idle device succeeds (test/excl.cpp:70-76).
- While O_EXCL is held: plain `O_NONBLOCK` and `O_EXCL|O_NONBLOCK` both fail immediately with exactly `EAGAIN` (test/excl.cpp:80-92).
- While a plain fd is held: `O_EXCL|O_NONBLOCK` fails with `EAGAIN` (test/excl.cpp:95-105).
- Closing the O_EXCL fd restores normal opens (test/excl.cpp:108-119).
- Blocking O_EXCL waits while a plain fd is open, and completes when it closes (test/excl.cpp:123-160). Symmetrically, a plain open blocks while O_EXCL is held (test/excl.cpp:164-201).
- A blocking O_EXCL wakes when the holder *process exits* (kernel fd cleanup), with a pipe handshake to avoid racing (test/excl.cpp:205-260).
- A signal without SA_RESTART interrupts a blocking O_EXCL open with exactly `EINTR` (test/excl.cpp:266-317).

> **Porting note:** O_EXCL/O_NONBLOCK are open(2) flags — the CreateFile equivalent must be designed (e.g. map exclusive-open to `FILE_SHARE_*=0` create semantics evaluated in `EvtDeviceFileCreate`, or an explicit flag in an ECP/first-ioctl). Windows CreateFile has no "block until available" mode, so either the create IRP is pended (cancelable) or the blocking mode is dropped and documented. EINTR-on-signal maps to IRP cancellation.

---

## 3. Non-runtime test tooling

- **test/checkws** (Python) — whitespace lint: rejects whitespace-only lines, trailing whitespace, tab-after-space in indentation, and 8+ consecutive leading spaces (test/checkws:10-13). `test/checkws-tests` is its fixture file.
- **test/pahole_check.sh** — compiles a dummy TU including `ioctl.h` and runs `pahole` on every struct; exits 1 if any struct has implicit padding: "Implicit padding in kernel-userspace ABI structures can lead to portability and security issues. All padding should be made explicit using reserved fields." (test/pahole_check.sh:7-9, 89-95).
  > **Porting note:** This is a strong statement that every `ioctl.h` struct is packed-by-construction with explicit reserved fields — MSVC will lay them out identically without pragma pack, but the port should replicate this check (e.g. static_asserts on sizeof/offsetof) to guarantee the shared ABI header stays hole-free.
- **test/run-hardware-tests.sh** — root-only CI driver: unloads existing module/DKMS versions, builds, `insmod`, verifies `/dev/tenstorrent/` exists, dumps `/sys/class/tenstorrent/tenstorrent!*/tt_*` telemetry, ensures ≥2 2MiB hugepages (`/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`, test/run-hardware-tests.sh:135-143), then runs `ttkmd_test` under a **120 s timeout** with dmesg/lscpu diagnostics on hang (test/run-hardware-tests.sh:16-47, 148).
- **test/mass-build-test** (Python) — builds the module against every kernel tag in a range (e.g. v4.18..v6.3) using `defconfig` + `modules_prepare`, with `KBUILD_MODPOST_WARN=1` (test/mass-build-test:40-44). Pure Linux build-matrix tool; the Windows analogue is building against multiple WDK/OS targets.

---

## 4. Tools

### 4.1 tools/reset.c — reset sequencing reference

Standalone C tool ("Iteratively AI-written with manual adjustments", tools/reset.c:6) that documents the full user-visible reset flow. It carries its own copies of the ioctl definitions: magic `0xFA`, `GET_DEVICE_INFO = _IO(0xFA, 0)`, `RESET_DEVICE = _IO(0xFA, 6)`; flags `ASIC_RESET 4`, `ASIC_DMC_RESET 5`, `POST_RESET 6` (tools/reset.c:38-45). Flow:
1. Open `/dev/tenstorrent/<id>` with `O_RDWR | O_APPEND` (power-aware open; tools/reset.c:196) and issue `RESET_DEVICE` with flags 4 (ASIC) or 5 (ASIC+DMC); a nonzero `out.result` is a failure distinct from an ioctl error (tools/reset.c:199-209).
2. Wormhole requires a settle delay before polling: "tt-smi uses 2 seconds... On my system, 20ms isn't long enough but 40ms is" — the tool sleeps 500 ms (tools/reset.c:218-223).
3. Poll for completion with timeout 5 s (ASIC) / 10 s (DMC) (tools/reset.c:225): either the PCI device disappears from `/sys/bus/pci/devices/<bdf>` and reappears, or an **in-place reset marker clears**: 1 byte read of config offset 4 (Command register), completion when `((cmd_reg >> 6) & 1) == 0` (tools/reset.c:236-246) — i.e. the driver uses Command register bit 6 (Parity Error Response) as a reset-in-progress marker.
4. Re-find the device by BDF (the /dev node index may change after reset; 10 s timeout, tools/reset.c:261-270) and issue `RESET_DEVICE` with `POST_RESET` (flags 6) on the new node, checking `out.result == 0` (tools/reset.c:273-289).

> **Porting note:** The disappearance/reappearance leg corresponds to a PCIe-link-level (hot-reset-like) path — on Windows this is surprise-removal + re-enumeration, and the tool's "re-find by BDF, node index may change" logic maps to re-opening by device interface after `CM_NOTIFY` arrival. The Command-register-bit-6 marker is a cross-cutting hardware/driver contract any port of RESET_DEVICE must reproduce or replace.

### 4.2 tools/power.c — SET_POWER_STATE reference client

Documents `TENSTORRENT_IOCTL_SET_POWER_STATE = _IO(0xFA, 15)` (tools/power.c:35) and the struct (tools/power.c:37-51):
```c
struct tenstorrent_power_state {
    __u32 argsz; __u32 flags;
    __u8 reserved0;
    __u8 validity;   // low nibble = # valid flag bits, high nibble = # valid settings
    __u16 power_flags;
    __u16 power_settings[14];
};
```
Flag bits: bit0 `MAX_AI_CLK` (1=max, 0=min), bit1 `MRISC_PHY_WAKEUP`, bit2 `TENSIX_ENABLE`, bit3 `L2CPU_ENABLE` (tools/power.c:46-49). Documented driver behavior (usage text, tools/power.c:160-176):
- The driver **aggregates** power requests from all clients; unspecified flag bits are treated as ON for that client (backward compatibility) — turning a feature off requires explicitly passing a 0 bit.
- Unknown settings are silently accepted for forward compatibility ("the driver will not return an error for unknown settings").
- Opening with `O_APPEND` marks the client power-aware (tools/power.c:266-267); the client's contribution is removed when the fd closes (tools/power.c:277-279). This matches driver behavior: `bool power_aware = file->f_flags & O_APPEND;` (chardev.c:795) and ioctl.h:374-376 (with O_APPEND initial state is all-off; without, legacy high-power).

> **Porting note:** `O_APPEND` as a "power-aware client" signal is a pure Linux open-flag hack; the Windows port needs an explicit mechanism (create option, ECP, or an ioctl handshake). The aggregation-with-remove-on-close semantics belong in file-object context + `EvtFileCleanup`.

### 4.3 tools/rdma/nic_bh_p2p_dma.c — dma-buf P2P demo

Two-node RDMA demo: the Blackhole node allocates a 4 GiB TLB window aimed at a GDDR tile (x=17, y=12, addr 0, ordering 0 = Relaxed; tools/rdma/nic_bh_p2p_dma.c:127-132, 504-513), exports 256 MiB of it as a dma-buf via `EXPORT_TLB_DMABUF = _IO(0xFA, 16)` (tools/rdma/nic_bh_p2p_dma.c:59, 515-520), and registers the dma-buf as an RDMA MR with `ibv_reg_dmabuf_mr` (tools/rdma/nic_bh_p2p_dma.c:529-533). The peer node then RDMA-WRITEs a random pattern into GDDR and RDMA-READs it back for verification, with the BH host CPU never touching the data (tools/rdma/nic_bh_p2p_dma.c:594-611). Before use it raises power via SET_POWER_STATE with all four flags (tools/rdma/nic_bh_p2p_dma.c:460-469) and validates the device is Blackhole (DID 0xb140, tools/rdma/nic_bh_p2p_dma.c:471-487). Local ioctl struct copies here match ioctl.h (tools/rdma/nic_bh_p2p_dma.c:63-123) and independently document the ioctl numbers 0, 11, 12, 13, 15, 16.

### 4.4 tools/fix-tt-hotplug-bars

Bash workaround for Thunderbolt-attached devices whose BARs get `<unassigned>`: requires booting with `pci=realloc`, then removes the Thunderbolt host bridge from sysfs and rescans so the kernel resizes bridge windows (tools/fix-tt-hotplug-bars:5-15, 88-101). Detects TT devices by vendor `0x1e52` (tools/fix-tt-hotplug-bars:21). Linux-only PCI plumbing; documents that the device has large BARs which can fail bridge allocation on hot-plug paths.

### 4.5 Packaging/release scripts

- **tools/current-version** parses `TENSTORRENT_DRIVER_VERSION_{MAJOR,MINOR,PATCH,SUFFIX}` out of `module.h` and prints e.g. `2.10.1-pre` (tools/current-version:18-28; module.h:19-22 currently 2/10/1/"-pre"). `module.h` is the single version source; build_debs.sh:22-30 and build_rpms.sh:20-28 hard-fail if `dkms.conf`'s `PACKAGE_VERSION` disagrees.
- **tools/make-source-release** creates a DKMS source tarball, excluding paths in **tools/exclude-from-release** (`tools`, `test`, `ttkmd_*.tar`, `modprobe.d-tenstorrent.conf`, `.git*`, `.cache`, `*.deb`, `*.rpm`; tools/exclude-from-release:1-8), and refuses to ship if any `.o` files leak in (tools/make-source-release:52-61).
- **tools/make-installer** + **tools/installer-header.sh** build a self-extracting shell installer (gzip'd DKMS tarball appended after a `__CUT_HERE__` marker) that removes old DKMS versions, `dkms ldtarball`/`install`, and reloads the module (tools/installer-header.sh:8-33).
- **tools/build_debs.sh** / **tools/build_rpms.sh** produce `tenstorrent-dkms` .deb/.rpm packages; both install `udev-50-tenstorrent.rules` into `/lib/udev/rules.d` (build_debs.sh:74-75; build_rpms.sh:95-97), convert `-` to `~` in versions for pre-release ordering (build_debs.sh:32-33; build_rpms.sh:31-35), and modprobe the module on install.

All of section 4.5 is Linux distribution machinery with no ABI content; the Windows analogue is the INF/CAT/MSI signing pipeline.

---

## 5. docs/ — sysfs ABI documentation

`docs/sysfs-attributes.md` is the only file in docs/ and documents two sysfs surfaces:

1. **Telemetry attributes** at `/sys/class/tenstorrent/tenstorrent!<N>/tt_*` (the `!` is literal; docs/sysfs-attributes.md:13-29). Attributes appear **only if the firmware reports the corresponding telemetry tag** (docs/sysfs-attributes.md:45-49). Table (docs/sysfs-attributes.md:54-69):
   - BH+WH: `tt_aiclk`, `tt_axiclk`, `tt_arcclk` (MHz), `tt_serial` (hex), `tt_card_type` (e.g. "p150a", "n150"), `tt_asic_id` (hex), `tt_fw_bundle_ver`, `tt_m3app_fw_ver`, `tt_heartbeat` ("changing = alive")
   - WH only: `tt_m3bl_fw_ver`, `tt_arc_fw_ver`, `tt_eth_fw_ver`, `tt_ttflash_ver`
   - BH only: `tt_therm_trip_count` (ASIC shutdowns due to critical temperature since power cycle)

2. **PCIe performance counters** at `.../pcie_perf_counters/` (docs/sysfs-attributes.md:88-109): 12 read-only counters (6 names × NOC0/NOC1 suffix 0/1): `mst_rd_data_word_received{0,1}`, `mst_nonposted_wr_data_word_sent{0,1}`, `mst_posted_wr_data_word_sent{0,1}`, `slv_nonposted_wr_data_word_received{0,1}`, `slv_posted_wr_data_word_received{0,1}`, `slv_rd_data_word_sent{0,1}` (docs/sysfs-attributes.md:129-142). Semantics: unsigned 32-bit, cumulative since last hardware reset, wrap around, **no reset mechanism**, counted in units of **32-byte flits**, read directly from a memory-mapped BAR segment (docs/sysfs-attributes.md:111-127).

> **Porting note:** These sysfs surfaces are consumed by tt-smi and monitoring tools. The Windows port should expose equivalents (WMI provider or query IOCTLs). Key facts to preserve: attribute presence is firmware-tag-conditional, heartbeat is the liveness signal, counters are 32-bit wrap-around flit counts with no reset.

---

## 6. contrib/

`contrib/packaging/` holds third-party (Nix) packaging: `overlay.nix` overlays `tt-kmd` into every nixpkgs `linuxKernel.packages` set, marking it broken for kernels older than 6.10 (`broken = linuxPackages.kernel.kernelOlder "6.10"` — note this reflects the *overlay dev build*, not the driver's actual minimum) (contrib/packaging/nix/overlay.nix:9-23). `ci.sh` builds all flake packages (contrib/packaging/nix/ci.sh:3-11). MAINTAINERS.md names the external nix maintainer. No ABI content.

---

## 7. Windows conformance-test portability assessment

| Test | Portable as Windows conformance test? | What it needs |
|---|---|---|
| Enumeration (enumeration.cpp) | Rewrite | SetupDi/CM_* by interface GUID; DID→type map (0x401e/0xb140) unchanged |
| GetDriverInfo | Yes (core) | ioctl → DeviceIoControl; replace sysfs version cross-check with file-version resource |
| GetDeviceInfo | Yes (core) | replace sysfs cross-check with CM_ bus/address properties; BDF-encoding and max_dma_buf_size_log2 range checks port verbatim |
| ConfigSpace | Partial | config space via SetupDi/ioctl instead of sysfs; MSI/BME assertions become "interrupts work / BME set" checks |
| QueryMappings | Yes | mmap → the port's mapping call; all invariants (2.4) unchanged |
| DmaBuf / NocDmaBuf | Yes | mmap → mapping call; EINVAL/ENOMEM mapped to NTSTATUS/Win32 equivalents |
| IoctlOverrun | Yes | VirtualAlloc + PAGE_NOACCESS guard page; asserts no out-of-bounds access + output truncation |
| IoctlZeroing | Yes | direct port; asserts zero-fill of `output_size_bytes` remainder |
| PinPages | Mostly | aligned_alloc→VirtualAlloc; hugepages→large pages (SeLockMemoryPrivilege); discontiguous double-map trick via CreateFileMapping/MapViewOfFile twice; IOMMU-conditional expectations need a Windows-side "is DMA remapped" probe |
| Lock | Mostly | threads/fds port directly; fork-based exit tests → spawn child process; SA_RESTART test replaced by IRP-cancellation test |
| Hwmon | No | replace with telemetry-IOCTL/WMI test |
| MapPeerBar | Yes (if ioctl kept) | peer_fd → peer HANDLE; BAR geometry from CM_ resource lists instead of sysfs `resource` |
| Tlbs | Yes | mmap → mapping call; sysfs resource4 size → BAR4 length from resources; partial-unmap semantics re-specified for section views |
| TlbExport (dmabuf) | No (mechanism) / Yes (lifetime) | dma-buf has no Windows equivalent; if a sharing mechanism exists, port the EINVAL/EPERM/lifetime matrix |
| DeviceRelease (NOC cleanup) | Yes | close fd → CloseHandle; also test process-kill path |
| MappingsDebugfs | No | replace with diagnostic IOCTL/WMI equivalent if implemented |
| ProcfsPids | No | same |
| Excl | Design-dependent | depends on the chosen Windows exclusive-open design; EAGAIN/EINTR map to sharing-violation / cancellation |

---

## Key constants table

| Name | Value | Source cite |
|---|---|---|
| TT PCI vendor ID | 0x1E52 | test/enumeration.cpp:82 |
| Wormhole PCI device ID | 0x401e | test/enumeration.cpp:135; tools/reset.c:51 |
| Blackhole PCI device ID | 0xb140 | test/enumeration.cpp:136; tools/reset.c:50 |
| TENSTORRENT_DRIVER_VERSION (interface) | 2 | ioctl.h:10 (asserted test/get_driver_info.cpp:55) |
| TENSTORRENT_MAX_DMA_BUFS | 256 | ioctl.h:41 (exercised test/dma_buf.cpp:156, 82) |
| TENSTORRENT_RESOURCE_LOCK_COUNT | 64 | ioctl.h:44 (exercised test/lock.cpp:195, 215) |
| max_dma_buf_size_log2 valid range | [12, 63] | test/get_device_info.cpp:61-65 |
| bus_dev_fn encoding | (bus<<8)\|(dev<<3)\|fn | test/get_device_info.cpp:52-54 |
| Mapping base+size limit | < 2^44 ("32 + log(PAGE_SIZE)") | test/query_mappings.cpp:153 |
| MAP_PEER_BAR max length used | 0xFFFFF000 (u32, page-aligned cap) | test/map_peer_bar.cpp:97 |
| Lock TEST result bits | LOCK_LOCAL=0b01, LOCK_GLOBAL=0b10 | test/lock.cpp:27-28 |
| Wormhole TLB inventory | 156×1M, 10×2M, 20×16M (last 16M reserved) | test/tlbs.cpp:98-99, 135-142 |
| Blackhole TLB inventory | 202×2M (last reserved) + BAR4_size/4G ×4G | test/tlbs.cpp:293-295, 312-319 |
| TLB window sizes | 1M=1<<20, 2M=1<<21, 16M=1<<24, 4G=1<<32 | test/tlbs.h:109-112 |
| WH CONFIGURE_TLB addr width | must fit 36 bits (1<<36 rejected) | test/tlbs.cpp:269-281 |
| BH NIU_CFG offset (BAR0) / translation bit | 0x1FD04100, bit 14 | test/tlbs.cpp:26, 40 |
| BH BAR0 mmap size in test | 1<<29 (512 MiB) | test/tlbs.cpp:25 |
| WH ARC node-id addr @(0,10) | 0xFFFB2002C | test/tlbs.cpp:169-172 |
| WH DDR node-id addr @(0,11) | 0x10009002C | test/tlbs.cpp:170-173 |
| BH tensix node-id reg | 0xffb20148 (grid 17×12; x∈[1,7]∪[10,16], y∈[2,11]) | test/tlbs.cpp:360-371 |
| BH PCI tile node-id reg | 0xFFFFFFFFFF000148; tile (19,24) translated, (2,0) not | test/tlbs.cpp:413, 421-429 |
| BH ARC tile / reg | (8,0), 0x0000000080050044 | test/tlbs.cpp:436-437 |
| node-id decode | x=bits[5:0], y=bits[11:6] | test/tlbs.cpp:92-93 |
| ioctl magic / RESET_DEVICE nr | 0xFA / 6 | tools/reset.c:38-40 |
| RESET flags ASIC / ASIC_DMC / POST_RESET | 4 / 5 / 6 | tools/reset.c:43-45; ioctl.h:149-151 |
| Reset poll timeouts | 5 s ASIC, 10 s DMC, 10 s re-find; 500 ms WH settle | tools/reset.c:222-225, 262 |
| Reset in-progress marker | PCI Command reg (offset 4) bit 6 | tools/reset.c:241 |
| SET_POWER_STATE nr / flag bits | 15; bit0 MAX_AI_CLK, bit1 MRISC_PHY_WAKEUP, bit2 TENSIX_ENABLE, bit3 L2CPU_ENABLE | tools/power.c:35, 46-49 |
| Power validity nibbles | low=count of flags, high=count of settings | tools/power.c:42-44 |
| power_settings capacity | 14 × u16 | tools/power.c:50 |
| Power-aware open marker | O_APPEND | tools/power.c:266-267; chardev.c:795 |
| EXPORT_TLB_DMABUF nr | 16 | tools/rdma/nic_bh_p2p_dma.c:59 |
| PIN_PAGES flags | CONTIGUOUS=1, NOC_DMA=2 | ioctl.h:169-170 (used test/pin_pages.cpp:58; test/mappings_debugfs.cpp:149) |
| max simultaneous pins tested | 1024 | test/pin_pages.cpp:183 |
| hwmon sensor set | curr1/in0/temp1/power1 + labels + maxes (WH) | test/hwmon.cpp:14-20, 37-42 |
| Hardware test timeout | 120 s for full ttkmd_test | test/run-hardware-tests.sh:148 |
| debugfs mappings path | /sys/kernel/debug/tenstorrent/<N>/mappings | test/mappings_debugfs.cpp:43 |
| procfs pids path | /proc/driver/tenstorrent/<N>/pids | test/procfs_pids.cpp:33 |
| PCIe perf counter unit | 32-byte flits, u32 wrap-around, no reset | docs/sysfs-attributes.md:111-127 |

## Open questions

1. **Errno for several rejection paths is unasserted.** PIN_PAGES bad-flags/bad-size/unmapped-range, UNPIN_PAGES bad-size, MAP_PEER_BAR same-device/different-chip (non-overrun variant), and CONFIGURE_TLB misaligned/oversized-address failures only assert `ioctl(...) == -1` / `!= 0` without checking errno (e.g. test/pin_pages.cpp:94, 118, 162; test/map_peer_bar.cpp:123, 140; test/tlbs.cpp:264, 280). The precise error codes must be taken from the driver sources (chardev/tlb sections of this analysis), not from the tests.
2. **`VerifyNoOverlap` iterates the unsorted mapping list** (test/query_mappings.cpp:114-128) despite building a sorted copy — likely a test bug. Whether QUERY_MAPPINGS results are guaranteed base-sorted (making the check correct as written) is not established by the test; check the driver's emit order before relying on it.
3. **EXPORT_TLB_DMABUF `size == 0`**: the basic-export and lifetime tests pass `offset=0, size=0` and expect success (test/dmabuf_export.cpp:63, 84, 217), implying size 0 = "whole window", but no test asserts what the exported length actually is. Confirm against the driver implementation.
4. **Reset marker semantics**: tools/reset.c:241 polls PCI Command register bit 6 ("marker cleared") to detect in-place reset completion. Whether the driver or firmware sets/clears this bit, and on which device generations the in-place (non-disappearing) path applies, is not documented in the tool — must be cross-checked with the reset section of the driver analysis.
5. **hwmon max-comparison excluded on Blackhole** (`dev.type < Blackhole`, test/hwmon.cpp:89): the tests don't say whether BH lacks the `*_max` files or merely has unreliable values. Affects what a Windows telemetry surface should expose per generation.
6. **TLB partial-unmap behavior is kernel-version dependent in the mremap leg** (test/tlbs.cpp:550-553: "fails on 5.15.0 (fine), succeeds on 5.4.0"); the ABI-stable requirement is only that any surviving user mapping blocks FREE_TLB. A Windows port should define its own all-or-nothing mapping semantics explicitly.
7. **`VerifyPinPagesMultipleRanges` uses 1024 pins** but calls the count `max_pinned_ranges` (test/pin_pages.cpp:183) — whether 1024 is an actual driver limit or an arbitrary test number is not established here; check the pin-pages section of the driver analysis.
8. **AER expectations in virtualized environments**: `--skip-aer` exists because AER "seems to be disabled" in VMs (test/main.cpp:34-36). For a Windows port, the equivalent expectation (AER handled by OS/platform, driver-visible or not) needs its own decision.
9. **nix overlay marks kernels < 6.10 broken** (contrib/packaging/nix/overlay.nix:19) while mass-build-test targets v4.18+ (test/mass-build-test:9) — the true minimum supported kernel is ambiguous from these directories alone (irrelevant to Windows except as a hint about which kernel-API fallbacks exist in the driver).
