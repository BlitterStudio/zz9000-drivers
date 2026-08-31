#!/usr/bin/env python3
"""Fail when firmware and driver USB proxy wire definitions diverge."""

from __future__ import annotations

import os
import pathlib
import re
import sys

DRIVERS_ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER_HEADER = DRIVERS_ROOT / "usb-poseidon" / "zzusbhw.h"
DRIVER_ISO_HEADER = DRIVERS_ROOT / "usb-poseidon" / "zzusb_iso.h"
FIRMWARE_HEADER_REL = pathlib.Path(
    "ZZ9000_proto.sdk/ZZ9000OS/src/usb_proxy.h"
)
FIRMWARE_DIAG_HEADER_REL = pathlib.Path(
    "ZZ9000_proto.sdk/ZZ9000OS/src/usb_proxy_diag.h"
)
FIRMWARE_ISO_HEADER_REL = pathlib.Path(
    "ZZ9000_proto.sdk/ZZ9000OS/src/usb_proxy_iso.h"
)

DEFINE_NAMES = (
    "ZZUSB_CMD_CONTROL_XFER", "ZZUSB_CMD_BULK_XFER",
    "ZZUSB_CMD_INT_XFER", "ZZUSB_CMD_ISO_XFER",
    "ZZUSB_CMD_RESET_PORT", "ZZUSB_CMD_RESUME_PORT",
    "ZZUSB_CMD_SUSPEND_PORT", "ZZUSB_CMD_ENUMERATE",
    "ZZUSB_CMD_QUERY_DEVICE", "ZZUSB_CMD_SET_ADDRESS",
    "ZZUSB_CMD_CLEAR_STALL", "ZZUSB_CMD_CHECK_PORT",
    "ZZUSB_CMD_QUERY_CAPS", "ZZUSB_CMD_RETIRE_EP",
    "ZZUSB_CMD_CANCEL_EP", "ZZUSB_CMD_DIAG_SNAPSHOT",
    "ZZUSB_CMD_PERIODIC_ARM", "ZZUSB_CMD_PERIODIC_REAP",
    "ZZUSB_CMD_PERIODIC_STOP", "ZZUSB_CMD_ISO_QUEUE",
    "ZZUSB_CMD_ISO_REAP", "ZZUSB_CMD_ISO_STOP",
    "ZZUSB_STATUS_OK", "ZZUSB_STATUS_PENDING", "ZZUSB_STATUS_ERROR",
    "ZZUSB_STATUS_TIMEOUT", "ZZUSB_STATUS_STALL", "ZZUSB_STATUS_NAK",
    "ZZUSB_STATUS_CRC", "ZZUSB_STATUS_BABBLE", "ZZUSB_STATUS_OVERRUN",
    "ZZUSB_STATUS_UNDERRUN", "ZZUSB_STATUS_OFFLINE",
    "ZZUSB_STATUS_BADPARAM", "ZZUSB_STATUS_UNSUPPORTED",
    "ZZUSB_STATUS_STALE", "ZZUSB_STATUS_CANCELLED",
    "ZZUSB_STATUS_HOSTERROR", "ZZUSB_STATUS_BUSY", "ZZUSB_STATUS_NOMEM",
    "ZZUSB_SPEED_LOW", "ZZUSB_SPEED_FULL", "ZZUSB_SPEED_HIGH",
    "ZZUSB_FLAG_SPLIT", "ZZUSB_FLAG_RESET_FSLS", "ZZUSB_FLAG_MULTI_TT",
    "ZZUSB_FLAG_TT_THINK_SHIFT", "ZZUSB_FLAG_TT_THINK_MASK",
    "ZZUSB_XFER_CONTROL", "ZZUSB_XFER_BULK", "ZZUSB_XFER_INTERRUPT",
    "ZZUSB_XFER_ISO", "ZZUSB_PROTOCOL_VERSION", "ZZUSB_CMD_SIZE",
    "ZZUSB_V2_HEADER_SIZE", "ZZUSB_DATA_OFFSET", "ZZUSB_APERTURE_SIZE",
    "ZZUSB_MAX_XFER", "ZZUSB_V2_DATA_MAX", "ZZUSB_DIAG_SIZE",
    "ZZUSB_DIAG_OFFSET", "ZZUSB_MAINT_HEADER_OFFSET",
    "ZZUSB_MAINT_DATA_OFFSET", "ZZUSB_MAINT_DATA_MAX",
    "ZZUSB_DOORBELL_V2",
    "ZZUSB_CAP_PROTOCOL_V2", "ZZUSB_CAP_REQUEST_ID",
    "ZZUSB_CAP_CONTROLLER_EPOCH", "ZZUSB_CAP_VALIDATION",
    "ZZUSB_CAP_DIAGNOSTICS", "ZZUSB_CAP_PERIODIC",
    "ZZUSB_CAP_ISO_SIMPLE", "ZZUSB_CAP_ISO_REALTIME",
    "ZZUSB_CAP_EVENT_IRQ", "ZZUSB_CAP_PRECISE_ERRORS",
    "ZZUSB_CAP_MAINTENANCE",
)

DIAG_DEFINE_NAMES = (
    "ZZUSB_DIAG_MAGIC", "ZZUSB_DIAG_VERSION", "ZZUSB_DIAG_PAGE_SIZE",
    "ZZUSB_DIAG_EVENT_COUNT", "ZZUSB_DIAG_EVENT_SIZE",
    "ZZUSB_DIAG_COUNTER_COUNT", "ZZUSB_DIAG_OFF_MAGIC",
    "ZZUSB_DIAG_OFF_GENERATION", "ZZUSB_DIAG_OFF_VERSION",
    "ZZUSB_DIAG_OFF_HEADER_SIZE", "ZZUSB_DIAG_OFF_TOTAL_SIZE",
    "ZZUSB_DIAG_OFF_CAPABILITIES", "ZZUSB_DIAG_OFF_EPOCH",
    "ZZUSB_DIAG_OFF_LAST_ID", "ZZUSB_DIAG_OFF_EVENT_NEXT",
    "ZZUSB_DIAG_OFF_EVENT_COUNT", "ZZUSB_DIAG_OFF_LOST_EVENTS",
    "ZZUSB_DIAG_OFF_QUEUE_STATE", "ZZUSB_DIAG_OFF_SCHEDULE_BITS",
    "ZZUSB_DIAG_OFF_COUNTERS", "ZZUSB_DIAG_OFF_EVENTS",
    "ZZUSB_DIAG_EVT_OFF_SEQUENCE", "ZZUSB_DIAG_EVT_OFF_REQUEST",
    "ZZUSB_DIAG_EVT_OFF_EPOCH", "ZZUSB_DIAG_EVT_OFF_DETAIL",
    "ZZUSB_DIAG_EVT_OFF_TIMESTAMP", "ZZUSB_DIAG_EVT_OFF_TYPE",
    "ZZUSB_DIAG_EVT_OFF_STATUS", "ZZUSB_DIAG_EVT_OFF_ADDRESS",
    "ZZUSB_DIAG_EVT_OFF_TOPOLOGY", "ZZUSB_DIAG_EVT_OFF_ENDPOINT",
    "ZZUSB_DIAG_EVT_OFF_DIRECTION", "ZZUSB_DIAG_EVT_OFF_SCHEDULE",
)

ISO_DEFINE_NAMES = (
    "ZZUSB_ISO_MAGIC", "ZZUSB_ISO_VERSION", "ZZUSB_ISO_HEADER_SIZE",
    "ZZUSB_ISO_PACKET_SIZE", "ZZUSB_ISO_MAX_PACKETS",
    "ZZUSB_ISO_MAX_BATCHES", "ZZUSB_ISO_DATA_MAX",
    "ZZUSB_ISO_FLAG_ASAP", "ZZUSB_ISO_HDR_OFF_MAGIC",
    "ZZUSB_ISO_HDR_OFF_VERSION", "ZZUSB_ISO_HDR_OFF_FLAGS",
    "ZZUSB_ISO_HDR_OFF_BATCH_ID", "ZZUSB_ISO_HDR_OFF_START",
    "ZZUSB_ISO_HDR_OFF_COUNT", "ZZUSB_ISO_HDR_OFF_DATA_LEN",
    "ZZUSB_ISO_HDR_OFF_START_UFRAME",
    "ZZUSB_ISO_PKT_OFF_REQUESTED", "ZZUSB_ISO_PKT_OFF_ACTUAL",
    "ZZUSB_ISO_PKT_OFF_STATUS", "ZZUSB_ISO_PKT_OFF_FRAME",
    "ZZUSB_ISO_PKT_OFF_DATA", "ZZUSB_ISO_PKT_OFF_UFRAME",
    "ZZUSB_ISO_PACKET_OK", "ZZUSB_ISO_PACKET_PENDING",
    "ZZUSB_ISO_PACKET_SHORT", "ZZUSB_ISO_PACKET_MISSED",
    "ZZUSB_ISO_PACKET_UNDERRUN", "ZZUSB_ISO_PACKET_OVERRUN",
    "ZZUSB_ISO_PACKET_CANCELLED", "ZZUSB_ISO_PACKET_OFFLINE",
    "ZZUSB_ISO_PACKET_XACT", "ZZUSB_ISO_PACKET_BABBLE",
)


def _logical_lines(text: str) -> list[str]:
    return text.replace(chr(92) + "\r\n", "").replace(
        chr(92) + "\n", ""
    ).splitlines()


def _normalize_expression(value: str) -> str:
    value = re.split(r"/\*|//", value, maxsplit=1)[0]
    value = re.sub(r"(?<=[0-9A-Fa-f])(?:UL|LU|U|L)\b", "", value,
                   flags=re.IGNORECASE)
    return re.sub(r"\s+", "", value)


def extract_defines(text: str,
                    names: tuple[str, ...] = DEFINE_NAMES) -> dict[str, str]:
    wanted = set(names)
    found: dict[str, str] = {}
    for line in _logical_lines(text):
        match = re.match(r"^\s*#define\s+(ZZUSB_[A-Z0-9_]+)\s+(.+)$", line)
        if match and match.group(1) in wanted:
            found[match.group(1)] = _normalize_expression(match.group(2))
    return found


def extract_struct(text: str, name: str) -> list[tuple[str, str]]:
    match = re.search(
        rf"struct\s+{re.escape(name)}\s*\{{(?P<body>.*?)\}}\s*"
        r"__attribute__\(\(packed\)\)\s*;",
        text,
        re.DOTALL,
    )
    if not match:
        raise ValueError(f"missing packed struct {name}")
    fields = re.findall(
        r"\b(uint(?:8|16|32)_t)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;",
        match.group("body"),
    )
    return fields


def compare_headers(firmware_header: pathlib.Path,
                    driver_header: pathlib.Path = DRIVER_HEADER) -> list[str]:
    firmware_text = firmware_header.read_text(encoding="utf-8")
    driver_text = driver_header.read_text(encoding="utf-8")
    firmware_defines = extract_defines(firmware_text)
    driver_defines = extract_defines(driver_text)
    errors: list[str] = []

    for name in DEFINE_NAMES:
        left = firmware_defines.get(name)
        right = driver_defines.get(name)
        if left is None or right is None:
            errors.append(f"{name}: missing in " +
                          ("firmware" if left is None else "driver"))
        elif left != right:
            errors.append(f"{name}: firmware={left} driver={right}")

    for struct_name in ("ZZUSBCommand", "ZZUSBProtocolExtension"):
        try:
            left_fields = extract_struct(firmware_text, struct_name)
            right_fields = extract_struct(driver_text, struct_name)
        except ValueError as error:
            errors.append(str(error))
            continue
        if left_fields != right_fields:
            errors.append(
                f"{struct_name}: firmware={left_fields} driver={right_fields}"
            )

    firmware_diag_header = firmware_header.with_name("usb_proxy_diag.h")
    if firmware_diag_header.is_file():
        firmware_diag_defines = extract_defines(
            firmware_diag_header.read_text(encoding="utf-8"),
            DIAG_DEFINE_NAMES,
        )
        driver_diag_defines = extract_defines(
            driver_text, DIAG_DEFINE_NAMES
        )
        for name in DIAG_DEFINE_NAMES:
            left = firmware_diag_defines.get(name)
            right = driver_diag_defines.get(name)
            if left is None or right is None:
                errors.append(
                    f"{name}: missing in " +
                    ("firmware diagnostics" if left is None else "driver")
                )
            elif left != right:
                errors.append(
                    f"{name}: firmware diagnostics={left} driver={right}"
                )

    return errors


def compare_iso_headers(
    firmware_header: pathlib.Path,
    driver_header: pathlib.Path = DRIVER_ISO_HEADER,
) -> list[str]:
    firmware_defines = extract_defines(
        firmware_header.read_text(encoding="utf-8"), ISO_DEFINE_NAMES
    )
    driver_defines = extract_defines(
        driver_header.read_text(encoding="utf-8"), ISO_DEFINE_NAMES
    )
    errors: list[str] = []

    for name in ISO_DEFINE_NAMES:
        left = firmware_defines.get(name)
        right = driver_defines.get(name)
        if left is None or right is None:
            errors.append(
                f"{name}: missing in " +
                ("firmware ISO" if left is None else "driver ISO")
            )
        elif left != right:
            errors.append(f"{name}: firmware ISO={left} driver ISO={right}")
    return errors

def resolve_firmware_root(argument: str | None = None) -> pathlib.Path:
    candidate = argument or os.environ.get("ZZ9K_FIRMWARE_DIR")
    if candidate:
        return pathlib.Path(candidate).resolve()
    return (DRIVERS_ROOT.parent / "zz9000-firmware").resolve()


def main(argv: list[str]) -> int:
    firmware_root = resolve_firmware_root(argv[1] if len(argv) > 1 else None)
    firmware_header = firmware_root / FIRMWARE_HEADER_REL
    firmware_diag_header = firmware_root / FIRMWARE_DIAG_HEADER_REL
    firmware_iso_header = firmware_root / FIRMWARE_ISO_HEADER_REL
    if (not firmware_header.is_file() or
            not firmware_diag_header.is_file() or
            not firmware_iso_header.is_file() or
            not DRIVER_HEADER.is_file() or
            not DRIVER_ISO_HEADER.is_file()):
        print("check-usb-proxy-contract: cannot read firmware/driver headers",
              file=sys.stderr)
        print("set ZZ9K_FIRMWARE_DIR or pass the firmware checkout path",
              file=sys.stderr)
        return 2

    errors = compare_headers(firmware_header)
    errors.extend(compare_iso_headers(firmware_iso_header))
    if errors:
        for error in errors:
            print(f"check-usb-proxy-contract: {error}", file=sys.stderr)
        return 1
    print("USB proxy firmware/driver contract matches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
