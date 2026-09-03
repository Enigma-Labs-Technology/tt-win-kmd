# Security policy

This repository contains a Windows kernel-mode driver. Bugs in it can affect
the whole machine, so please report anything with a security impact privately.

## Reporting

Use GitHub's private vulnerability reporting ("Report a vulnerability" under
the repository's Security tab) rather than a public issue. Include the driver
version (`GET_DRIVER_INFO` or the file version of `ttkmd.sys`), the Windows
build, the card, and steps or a program that reproduces the problem. You will
get an acknowledgement within a few days; there is no bounty programme.

## Scope and threat model

- The device is accessible to interactive users (the INF grants them
  read/write on the device object, the Windows counterpart of the upstream
  udev rule). Anything an interactive user can trigger through the IOCTL
  surface that crashes the kernel, corrupts memory outside the device's own
  buffers, or escapes the per-device DMA ceiling is in scope.
- Device-side effects (a wedged card, firmware state) reachable only through
  operations the driver already refuses on physical hardware are out of scope.
- Test-signed preview builds require test signing to be enabled on the host,
  which lowers the host's code-integrity posture; that is a documented
  property of preview builds, not a defect.

## Supported versions

Only the most recent preview release receives fixes.
