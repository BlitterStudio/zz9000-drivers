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

    def test_ci_audio_jobs_use_build_scripts(self):
        ci = self.read(".github/workflows/ci.yml")
        self.assertIn('-w /src/mhi "$AMIGA_IMAGE" ./build.sh', ci)
        self.assertIn('-w /src/ahi/driver "$AMIGA_IMAGE" ./build.sh', ci)

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

    def test_shell_scripts_avoid_ci_shellcheck_patterns(self):
        bad_cdpath = []
        bad_path_export = []
        for path in ROOT.rglob("*.sh"):
            relpath = path.relative_to(ROOT).as_posix()
            text = path.read_text(encoding="utf-8")
            if "CDPATH= cd" in text:
                bad_cdpath.append(relpath)
            if ("export PATH=/opt/amiga/bin:$PATH" in text and
                    "shellcheck disable=SC2016" not in text):
                bad_path_export.append(relpath)
            if "export PATH=$PATH:/opt/amiga/bin" in text:
                bad_path_export.append(relpath)
            if 'export PATH=/opt/amiga/bin:""$PATH' in text:
                bad_path_export.append(relpath)

        self.assertEqual([], bad_cdpath)
        self.assertEqual([], bad_path_export)

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

    def test_audio_stack_uses_shared_ax_header(self):
        header = self.read("include/zz9000_ax.h")
        for token in (
            "ZZ_AX_BYTES_PER_PERIOD",
            "ZZ_AX_AUDIO_BUFSZ",
            "ZZ_AX_MIX_LEVELS_ENV",
            "ZZ_AX_IRQ_NAME_AHI",
            "ZZ_AX_IRQ_NAME_MHI",
            "ZZ_AX_AP_DSP_SET_VOLUMES",
        ):
            self.assertIn(token, header)

        for relpath in (
            "ahi/driver/zz9000ax-ahi.c",
            "mhi/mhizz9000.c",
        ):
            source = self.read(relpath)
            self.assertIn('#include "zz9000_ax.h"', source)
            self.assertNotIn("#define ZZ_BYTES_PER_PERIOD", source)
            self.assertNotIn("#define AUDIO_BUFSZ", source)
            self.assertNotIn("#define REG_ZZ_AUDIO_CONFIG", source)
            self.assertNotIn('"ENV:ZZ9K_MIX_LEVELS"', source)

    def test_ahi_initializes_period_size_before_enabling_interrupt(self):
        source = self.read("ahi/driver/zz9000ax-ahi.c")
        assign = source.index("AudioCtrl->ahiac_BuffSamples =")
        enable = source.index("enable_hw_interrupt(ahi_data);")
        self.assertLess(assign, enable)

    def test_mhi_queue_rejects_empty_buffers(self):
        source = self.read("mhi/mhizz9000.c")
        self.assertIn("!mhi_buffer", source)
        self.assertIn("mhi_size == 0", source)
        self.assertLess(source.index("!mhi_buffer"),
                        source.index("AllocVec(sizeof(struct ListNode)"))

    def test_mhi_get_empty_returns_one_buffer_per_call(self):
        source = self.read("mhi/mhizz9000.c")
        body = source[
            source.index("APTR i_MHIGetEmpty"):
            source.index("UBYTE i_MHIGetStatus")
        ]
        self.assertNotIn("for(;;)", body)
        self.assertIn("BufferNode->Played != FALSE", body)
        self.assertIn("mhi_buffer = BufferNode->Buffer;", body)
        self.assertIn("RemHead((struct List *)mp->BufferList);", body)

    def test_ahi_frequency_attr_rejects_bad_indices(self):
        source = self.read("ahi/driver/zz9000ax-ahi.c")
        body = source[
            source.index("case AHIDB_Frequency:"):
            source.index("case AHIDB_Index:")
        ]
        self.assertIn("arg < 0", body)
        self.assertIn("arg >= ZZ_NUM_FREQS", body)
        self.assertLess(body.index("arg < 0"), body.index("freqs[arg]"))

    def test_ahi_stop_start_silences_and_resets_ring(self):
        header = self.read("ahi/driver/zz9000ax-ahi.h")
        source = self.read("ahi/driver/zz9000ax-ahi.c")
        stop_body = source[
            source.index("intAHIsub_Stop"):
            source.index("static uint32_t __attribute__((used)) intAHIsub_Start")
        ]
        start_body = source[
            source.index("intAHIsub_Start"):
            source.index("static int32_t __attribute__((used)) intAHIsub_GetAttr")
        ]

        self.assertIn("uint32_t buf_offset;", header)
        self.assertIn("static void zero_hw_audio_ring", source)
        self.assertIn("disable_hw_interrupt(ahi_data);", stop_body)
        self.assertIn("ahi_data->buf_offset = 0;", stop_body)
        self.assertIn("zero_hw_audio_ring(ahi_data);", stop_body)
        self.assertIn("ahi_data->buf_offset = 0;", start_body)
        self.assertIn("zero_hw_audio_ring(ahi_data);", start_body)
        self.assertIn("enable_hw_interrupt(ahi_data);", start_body)


if __name__ == "__main__":
    unittest.main()
