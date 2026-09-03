// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd printk/dev_dbg logging surface (replaced by TraceLogging ETW, DD-3)
#pragma once

#include <TraceLoggingProvider.h>

// Provider "Tenstorrent.TtKmd" {dd99fa3c-98a4-479d-91d2-5b43ef8f5c22} (DD-2).
// Defined in driver.c; registered in DriverEntry, unregistered at driver cleanup.
TRACELOGGING_DECLARE_PROVIDER(g_TtTraceProvider);
