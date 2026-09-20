# Vendored anxs2ext.h provenance

`net/anxs2ext.h` is vendored **byte-identical** from the AmiNetXDuo
repository so the driver build never depends on an external checkout:

- Upstream: https://github.com/tinic/AmiNetXDuo `include/aminetxduo/anxs2ext.h`
- Branch: `main`
- Pinned commit: recorded at vendor time via
  `git ls-remote https://github.com/tinic/AmiNetXDuo main` — the vendoring
  commit's message carries the exact hash; U7's measurement matrix records
  it beside every row.
- License: MIT (SPDX-License-Identifier: MIT in the header itself).

Do not edit `net/anxs2ext.h` in place. To pick up upstream changes, re-copy
the file verbatim and update the hash here and in the vendoring commit
message; drift is auditable by diff against the pinned upstream commit.

## Documented driver deviations

These deviations from the header's published contract live in this driver
and are recorded here rather than edited into the vendored header:

1. **Hook execution context.** The header publishes "both hooks run at
   interrupt level, before the CMD_READ is replied". This driver calls
   `RX_DIRECT`/`RX_FILLED` from `frame_proc`, a task (KTD7) — a weaker
   guarantee, safe for hook code that tolerates interrupt context, which
   the reference opener does. The TX buffer-management copy hook likewise
   runs in whichever context drains the TX window.
2. **`ANXD_CMD_RX_POLL` answers `S2ERR_NOT_SUPPORTED`.** The ZZ9000's
   single-frame serial-acknowledge RX window cannot hold a frame for a
   late read: withholding the acknowledge would stall all reception while
   the firmware backlog fills (KTD6).
3. **`ANXD_CMD_RX_CAPACITY` answers 0** unless the firmware's backlog
   depth has been measured and the firmware does not pause the wire
   (KTD6; see OQ2 in the plan).
