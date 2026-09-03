# tt-umd on Windows: how the user-mode driver talks to ttkmd.sys

This note records the contract between this driver and the Windows backend of
tt-umd's `tt-kmd-lib` (branch `windows-port` of the sibling `../tt-umd`
checkout, file `tt-kmd-lib/src/tt_kmd_os_windows.c`). It replaces the earlier
plan of porting tt-umd over the `ttwin_compat` shim; the shim is now only used
by the driver's own conformance test.

## Transport

| Linux | Windows (this driver) |
|---|---|
| `open("/dev/tenstorrent/N")` | `CreateFileW` on the device interface instance of `GUID_DEVINTERFACE_TENSTORRENT` whose reference string is `TT<N>`; tt-umd keeps using the `/dev/tenstorrent/N` names and resolves N itself |
| `ioctl(fd, _IO(0xFA, nr), &s)` | `DeviceIoControl(h, CTL_CODE(0x80FA, 0x800 + nr, METHOD_BUFFERED, FILE_ANY_ACCESS), &s, sizeof s, &s, sizeof s)` with the unmodified Linux structure |
| `mmap(fd, offset)` | `IOCTL_TENSTORRENT_MAP {mmap_offset, length} -> user_va`; the offset tokens from `QUERY_MAPPINGS`, `ALLOCATE_TLB` and `ALLOCATE_DMA_BUF` are the Linux ones |
| `munmap` | `IOCTL_TENSTORRENT_UNMAP {user_va}`; any view left at handle close is torn down by the driver |
| `readdir("/dev/tenstorrent")` | `CM_Get_Device_Interface_ListW` (present instances only) |
| sysfs `revision`, `numa_node` | PnP properties `DEVPKEY_Device_HardwareIds` (`REV_xx`) and `DEVPKEY_Device_Numa_Node` |
| `/sys/module/tenstorrent/version` | `GET_DRIVER_INFO` on the first present device |
| `errno` | `GetLastError()` mapped back to errno (`ERROR_NOT_SUPPORTED` -> `ENOTSUP`, `ERROR_BUSY` -> `EBUSY`, ...) |

Every request tt-umd issues on Linux is issued unchanged on Windows: 0, 2, 3,
5, 6, 7, 8, 10, 11, 12, 13, 15. `EXPORT_TLB_DMABUF` (16) has no Windows meaning
and reports `ENOTSUP`.

## Host memory

Linux tt-umd backs sysmem with hugetlbfs pages (no IOMMU) or with one large
anonymous mapping pinned through the IOMMU. Neither exists on Windows: user
memory cannot be guaranteed physically contiguous and the driver does not hand
raw PFNs to the iATU in a DMA-remapped domain. The Windows path is:

1. `ALLOCATE_DMA_BUF` (index 1 upwards; index 0 is the PCIe DMA engine buffer)
   allocates a kernel-owned, physically contiguous buffer of up to 256 MiB
   (`max_dma_buf_size_log2 = 28`) and `MAP` exposes it to the process.
2. That view is pinned with `PIN_PAGES(CONTIGUOUS | NOC_DMA)`, the same call
   the hugepage path makes. The driver's NOC aperture allocator is bottom-up
   for `PIN_PAGES`, so the first channel lands at `pcie_base`, which tt-metal
   hard-codes.
3. tt-umd negotiates the channel size downwards (256 MiB, 128 MiB, ..., 16 MiB)
   until the driver can satisfy the contiguous allocation, or honours
   `TT_UMD_SYSMEM_CHANNEL_SIZE`. Channels smaller than 1 GiB cannot keep the
   `pcie_base + channel * 1 GiB` layout, so tt-umd clamps to one channel then.

Consequences for the driver:

- `PIN_PAGES` recognises a range inside a `MAP` view of one of the handle's
  own DMA buffers and backs the pinning with that buffer (DD-16): no pages are
  locked and the device address is the common buffer's bus address, valid
  with DMA remapping on or off. Only ranges outside such views take the
  direct, identity-domain-only path.
- A 1 GiB channel needs a contiguous 1 GiB common buffer. That is only
  realistic if the driver reserves it at boot; raising `TT_MAX_DMA_BUF_SIZE`
  and adding a boot-time reservation is tracked as OQ-10. The per-device
  ceiling (DD-17, default 4 GiB) bounds the total.
- Buffers are released early with the Windows-private `FREE_DMA_BUF_EX`
  (tt-kmd-lib `tt_free_dma_buf`), refused while a view or a backed pinning
  references the buffer, and otherwise at handle close. Linux's `FREE_DMA_BUF`
  is still a stub upstream, so `tt_free_dma_buf` reports -ENOTSUP there.
- `MAP`, `UNMAP`, `PIN_PAGES` and `UNPIN_PAGES` are only honoured from the
  process that opened the handle (DD-15). tt-kmd-lib opens non-inheritable
  handles, so this never triggers in tt-umd.

## What tt-umd does not do on Windows

- Warm reset through `RESET_DEVICE` flavours 2 and 4 (refused on silicon,
  DD-14) or 1 (PLDR unsupported on the p150a platform tested).
- Reading PCI configuration space, dma-buf export, JTAG, the simulation
  backends, and pinning arbitrary user memory (`map_user_buffer`).

## Verifying the contract

`src/tests/ttconform/ttconform.c` exercises every request through
`ttwin_compat` with the exact structures tt-umd sends. The tt-umd side is
covered by its `api_tests`, `unit_tests_blackhole` and `test_pcie_device`
binaries built on Windows (`../tt-umd/docs/windows.md`).
