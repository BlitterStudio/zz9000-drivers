import importlib.util
import os
import pathlib
import shutil
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "check-usb-proxy-contract.py"
SPEC = importlib.util.spec_from_file_location("usb_proxy_contract", SCRIPT)
CONTRACT = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(CONTRACT)


class USBProxyContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        firmware = os.environ.get("ZZ9K_FIRMWARE_DIR")
        if firmware:
            cls.firmware_root = pathlib.Path(firmware).resolve()
        else:
            cls.firmware_root = (ROOT.parent / "zz9000-firmware").resolve()
        cls.firmware_header = cls.firmware_root / CONTRACT.FIRMWARE_HEADER_REL
        cls.firmware_iso_header = (
            cls.firmware_root / CONTRACT.FIRMWARE_ISO_HEADER_REL
        )
        if (not cls.firmware_header.is_file() or
                not cls.firmware_iso_header.is_file()):
            raise unittest.SkipTest(
                "set ZZ9K_FIRMWARE_DIR to a zz9000-firmware checkout"
            )

    def test_current_headers_match(self):
        self.assertEqual(
            CONTRACT.compare_headers(self.firmware_header),
            [],
        )
        self.assertEqual(
            CONTRACT.compare_iso_headers(self.firmware_iso_header),
            [],
        )

    def test_command_prefix_and_extension_fields_are_frozen(self):
        firmware_text = self.firmware_header.read_text(encoding="utf-8")
        self.assertEqual(
            [name for _, name in CONTRACT.extract_struct(
                firmware_text, "ZZUSBCommand"
            )],
            [
                "cmd", "status", "dev_addr", "endpoint", "direction",
                "xfer_type", "max_pkt_size", "data_length",
                "actual_length", "timeout_ms", "speed", "interval",
                "setup_bRequestType", "setup_bRequest", "setup_wValue",
                "setup_wIndex", "setup_wLength", "split_hub_addr",
                "split_hub_port", "flags", "reserved",
            ],
        )
        self.assertEqual(
            [name for _, name in CONTRACT.extract_struct(
                firmware_text, "ZZUSBProtocolExtension"
            )],
            [
                "version", "header_size", "request_id",
                "controller_epoch", "capabilities",
            ],
        )

    def test_drift_is_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            copied = pathlib.Path(directory) / "usb_proxy.h"
            shutil.copyfile(self.firmware_header, copied)
            text = copied.read_text(encoding="utf-8")
            copied.write_text(
                text.replace(
                    "#define ZZUSB_CMD_QUERY_CAPS     0x0D",
                    "#define ZZUSB_CMD_QUERY_CAPS     0x7D",
                ),
                encoding="utf-8",
            )
            errors = CONTRACT.compare_headers(copied)
        self.assertTrue(any("ZZUSB_CMD_QUERY_CAPS" in item for item in errors))

    def test_iso_drift_is_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            copied = pathlib.Path(directory) / "usb_proxy_iso.h"
            shutil.copyfile(self.firmware_iso_header, copied)
            text = copied.read_text(encoding="utf-8")
            copied.write_text(
                text.replace(
                    "#define ZZUSB_ISO_PACKET_BABBLE     9U",
                    "#define ZZUSB_ISO_PACKET_BABBLE     10U",
                ),
                encoding="utf-8",
            )
            errors = CONTRACT.compare_iso_headers(copied)
        self.assertTrue(
            any("ZZUSB_ISO_PACKET_BABBLE" in item for item in errors)
        )


if __name__ == "__main__":
    unittest.main()
