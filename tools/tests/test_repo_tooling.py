import pathlib
import re
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class RepoToolingTests(unittest.TestCase):
    def read(self, relpath):
        return (ROOT / relpath).read_text(encoding="utf-8")

    def test_root_makefile_exposes_common_entrypoints(self):
        text = self.read("Makefile")
        for target in (
            "build-all:",
            "package-local:",
            "check-release:",
            "quality:",
            "rtg-tests:",
            "ZZNetStats:",
            "ZZDiag:",
        ):
            self.assertIn(target, text)

    def test_shared_hardware_header_is_used_by_small_tools(self):
        header = self.read("include/zz9000_hw.h")
        self.assertIn("ZZ9000_MNT_MANUFACTURER", header)
        self.assertIn("ZZ_REG_VIDEOCAP_STATS", header)
        self.assertIn("zz9000_find_board", header)

        for relpath in (
            "ZZFwUpdate/ZZFwUpdate.c",
            "ZZScanlines/ZZScanlines.c",
            "net/ZZNetStats/ZZNetStats.c",
            "ZZDiag/ZZDiag.c",
        ):
            self.assertIn('#include "zz9000_hw.h"', self.read(relpath))

    def test_build_scripts_have_shebangs_and_use_common_docker_image(self):
        scripts = (
            "tools/amiga-docker.sh",
            "tools/build-all.sh",
            "tools/package-local.sh",
            "tools/check-release.sh",
            "tools/check-quality.sh",
            "usb-poseidon/build.sh",
            "sd-boot/build.sh",
            "ZZDiag/build.sh",
        )
        for relpath in scripts:
            path = ROOT / relpath
            first_line = path.read_text(encoding="utf-8").splitlines()[0]
            self.assertTrue(first_line.startswith("#!"), relpath)

        self.assertIn("sacredbanana/amiga-compiler:m68k-amigaos",
                      self.read("tools/amiga-docker.sh"))

    def test_ci_runs_host_checks_and_builds_zzdiag(self):
        ci = self.read(".github/workflows/ci.yml")
        self.assertIn("host-checks:", ci)
        self.assertIn("make rtg-tests", ci)
        self.assertIn("ZZDiag:", ci)
        self.assertIn("ZZDiag/ZZDiag", ci)

    def test_release_script_mentions_every_packaged_artifact(self):
        script = self.read("tools/check-release.sh")
        for artifact in (
            "ZZ9000.card",
            "ZZ9000Net.device",
            "zz9000ax.audio",
            "mhizz9000.library",
            "zzusbhw.device",
            "ZZTop",
            "ZZScanlines",
            "ZZFwUpdate",
            "ZZNetStats",
            "ZZDiag",
            "axmp3",
        ):
            self.assertIn(artifact, script)

    def test_shell_scripts_parse(self):
        scripts = (
            "tools/amiga-docker.sh",
            "tools/build-all.sh",
            "tools/package-local.sh",
            "tools/check-release.sh",
            "tools/check-quality.sh",
            "rtg/build.sh",
            "net/build.sh",
            "usb-poseidon/build.sh",
            "sd-boot/build.sh",
            "ZZDiag/build.sh",
        )
        for relpath in scripts:
            subprocess.run(["sh", "-n", str(ROOT / relpath)], check=True)

    def test_sd_boot_header_generation_does_not_require_xxd(self):
        script = self.read("sd-boot/build.sh")
        self.assertNotIn("xxd", script)
        self.assertIn("od -An -tx1", script)

    def test_build_outputs_are_not_tracked(self):
        tracked = subprocess.check_output(
            ["git", "ls-files"], cwd=ROOT, text=True
        ).splitlines()
        artifact_pattern = re.compile(
            r"^(rtg/ZZ9000\.card|"
            r"usb-poseidon/zzusbhw\.device|"
            r"net/ZZ9000Net\.device(\.68000)?|"
            r"ahi/driver/zz9000ax\.audio|"
            r"ZZFwUpdate/ZZFwUpdate|"
            r"ZZScanlines/ZZScanlines|"
            r"ZZTop/ZZTop|"
            r"ZZDiag/ZZDiag|"
            r"ahi/driver/ZZ9000AX|"
            r"ahi/axtest/axtest|"
            r"ax-direct/axtest|"
            r"ax-direct/axmp3|"
            r"mhi/mhizz9000\.library(\.debug)?|"
            r"net/ZZNetStats/ZZNetStats|"
            r"sd-boot/zzsd\.device|"
            r"sd-boot/boot-rom/boot\.bin)$"
        )
        artifacts = [path for path in tracked if artifact_pattern.match(path)]
        self.assertEqual([], artifacts)


if __name__ == "__main__":
    unittest.main()
