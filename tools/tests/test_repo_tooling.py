import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
GENERATED_ARTIFACT_PATHS = (
    "rtg/ZZ9000.card",
    "usb-poseidon/zzusbhw.device",
    "net/ZZ9000Net.device",
    "net/ZZ9000Net.device.68000",
    "ahi/driver/zz9000ax.audio",
    "ahi/driver/ZZ9000AX",
    "mhi/mhizz9000.library",
    "mhi/mhizz9000.library.debug",
    "mhi/mhizz9000.library.trace",
    "mhi/mhizz9000.library.trace.debug",
    "sd-boot/zzsd.device",
    "sd-boot/boot-rom/boot.bin",
    "ZZFwUpdate/ZZFwUpdate",
    "ZZScanlines/ZZScanlines",
    "ZZTop/ZZTop",
    "net/ZZNetStats/ZZNetStats",
    "ZZDiag/ZZDiag",
    "ahi/axtest/axtest",
    "ahi/duplextest/ZZAXDuplexTest",
)


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

    def test_live_videocap_contract_has_one_shared_definition(self):
        header = self.read("include/zz9000_hw.h")
        model = self.read("common/zz_vcap_live.c")
        test_stub = self.read("common/tests/stubs/zz9000_hw.h")

        for token in (
            "ZZ_VCAP_LIVE_CAPABILITY",
            "ZZ_VCAP_LIVE_STATUS",
            "ZZ_VCAP_LIVE_APPLIED_RAW",
            "ZZ_VCAP_LIVE_EFFECTIVE_CROP",
            "ZZ_VCAP_LIVE_STAGED_RAW_HI",
            "ZZ_VCAP_LIVE_STAGED_RAW_LO",
            "ZZ_VCAP_LIVE_COMMIT",
            "ZZ_VCAP_LIVE_CAPABILITY_VALUE",
            "ZZ_VCAP_LIVE_COMMIT_TOKEN",
        ):
            self.assertIn(token, header)
            self.assertIn(token, test_stub)
            self.assertNotIn(f"#define {token}", model)

    def test_centered_videocap_is_capability_gated_end_to_end(self):
        common_header = self.read("common/zzcfg_amiga.h")
        common_model = self.read("common/zzcfg_amiga.c")
        hw_header = self.read("include/zz9000_hw.h")
        zztop = self.read("ZZTop/Sources/ZZTop.c")
        rtg = self.read("rtg/mntgfx-gcc.c")

        self.assertIn("ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P (1U << 3)",
                      hw_header)
        self.assertLess(common_header.index("ZZCFG_VCAP_FILTERED_NTSC_EXACT"),
                        common_header.index("ZZCFG_VCAP_CENTERED_1080P_60"))
        self.assertIn('"centered_1080p_60"', common_model)
        self.assertIn("ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P, ZZCFG_VCAP_FULL_60",
                      common_model)
        self.assertIn("if (native_override)", zztop)
        self.assertIn("zzcfg_profile_supported(imsgCode, fw_capabilities)",
                      zztop)
        self.assertIn("vcapmode_legacy_labels", zztop)
        self.assertIn("ZZ_CARD_DATA_VCAP_MODE", rtg)
        self.assertNotIn("ZZ_CARD_DATA_SCANDBL_800X600", rtg)
        self.assertIn("zz_vcap_mode_uses_native_pan", rtg)
        self.assertIn("mode == ZZ_VMODE_CENTERED_1080P_60", rtg)

    def test_zztop_full_detail_labels_explain_refresh_behavior(self):
        zztop = self.read("ZZTop/Sources/ZZTop.c")

        self.assertEqual(2, zztop.count("1280x1024 Fixed 60Hz (Full detail)"))
        self.assertEqual(3,
                         zztop.count("1280x1024 Match PAL/NTSC (Full detail)"))
        self.assertNotIn("1280x1024 60Hz (Full)", zztop)
        self.assertNotIn("1280x1024 Exact (Full)", zztop)

    def test_cfg_guard_rejects_profile_value_drift(self):
        candidates = (
            pathlib.Path(os.environ.get("ZZ9K_FIRMWARE_DIR", "")),
            ROOT.parent / "zz9000-firmware",
            ROOT / "zz9000-firmware",
        )
        firmware_root = next(
            (path for path in candidates if (path / "ZZ9000.CFG").is_file()),
            None,
        )
        if firmware_root is None:
            self.skipTest("companion zz9000-firmware checkout unavailable")

        sources = (
            pathlib.Path("ZZ9000.CFG"),
            pathlib.Path("README.md"),
            pathlib.Path("ZZ9000_proto.sdk/ZZ9000OS/src/zz_config.c"),
        )
        marker = "#   filtered_ntsc_exact -"

        for label in ("missing", "extra"):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as tmp:
                fixture = pathlib.Path(tmp)
                for relpath in sources:
                    destination = fixture / relpath
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(firmware_root / relpath, destination)

                sample = fixture / "ZZ9000.CFG"
                text = sample.read_text(encoding="utf-8")
                self.assertIn(marker, text)
                profile_line = next(
                    line for line in text.splitlines(keepends=True)
                    if line.startswith(marker)
                )
                replacement = "" if label == "missing" else (
                    profile_line +
                    "#   unexpected_profile    - parity-guard fixture\n"
                )
                sample.write_text(
                    text.replace(profile_line, replacement, 1),
                    encoding="utf-8",
                )

                result = subprocess.run(
                    ["sh", str(ROOT / "tools/check-cfg-keys.sh"), str(fixture)],
                    cwd=ROOT,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
                self.assertEqual(1, result.returncode, result.stdout)
                self.assertIn("videocap_profile values", result.stdout)

    def test_zztop_live_calibration_uses_native_v37_contract(self):
        source = self.read("ZZTop/Sources/ZZTop.c")
        build = self.read("ZZTop/build-gcc.sh")

        for token in (
            '#include "zz_vcap_live.h"',
            "AGAD_BTN_CALIBRATE",
            "PAL_MONITOR_ID | HIRES_KEY",
            "NTSC_MONITOR_ID | HIRES_KEY",
            "ModeNotAvailable(display_id)",
            "ModeNotAvailable(HIRES_KEY)",
            "DTAG_DISP, HIRES_KEY",
            "DIPF_IS_PAL",
            "DIPF_IS_FOREIGN",
            "zz_vcap_calibration_standard(",
            "vcap_screen_is_foreign(return_screen)",
            "GetVPModeID(&screen->ViewPort)",
            "detected_standard == ZZ_VCAP_STANDARD_UNKNOWN",
            "Native default Hires unavailable (mode error %lu)",
            "Native %s Hires unavailable (error %lu, %s, lines %u)",
            "Native %s screen open failed (Intuition error %lu)",
            "SA_Overscan, OSCAN_TEXT",
            "WA_IDCMP, IDCMP_RAWKEY",
            "IECODE_UP_PREFIX",
            "IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT",
            "ZZ_VCAP_LIVE_COMMIT_TOKEN",
            "ZZ_VCAP_ANCHOR_SETTINGS",
            "ZZ_VCAP_ANCHOR_ADVANCED",
            "ZZ_VCAP_ANCHOR_CALIBRATION",
            "ZZ_VCAP_ANCHOR_PREVIEW",
            "VCAP_APPLY_CONFLICT",
            "static BOOL settings_save",
        ):
            self.assertIn(token, source)
        self.assertNotIn("DEFAULT_MONITOR_ID", source)
        self.assertNotIn("SA_Draggable", source)
        self.assertNotIn("SA_Exclusive", source)
        self.assertIn("if (live_session.preview_valid) {", source)
        self.assertIn("../common/zz_vcap_live.c", build)

    def test_zzdiag_capture_dump_has_fixed_header_and_size_gate(self):
        text = self.read("ZZDiag/ZZDiag.c")
        self.assertIn('"P6\\n1280 320\\n255\\n"', text)
        self.assertIn("DeleteFile((CONST_STRPTR)path)", text)
        self.assertIn("Seek(file, 0, OFFSET_END)", text)
        self.assertNotIn('sprintf(header, "P6', text)

        capture_block = text.index("if (capture_path) {")
        aperture_guard = text.index("if (board.zorro_version != 3)",
                                    capture_block)
        probe = text.index("arm_videocap_probe(board.address)",
                           capture_block)
        self.assertLess(aperture_guard, probe)

    def test_zzdiag_compares_pre_ddr_probe_with_capture_memory(self):
        text = self.read("ZZDiag/ZZDiag.c")
        header = self.read("include/zz9000_hw.h")
        self.assertIn("ZZ_REG_VCAP_PROBE_DATA_BASE", header)
        self.assertIn("ZZ_REG_VCAP_PROBE_SAMPLER_DATA_BASE", header)
        self.assertIn("ZZ_REG_VCAP_PROBE_OWNER_BASE", header)
        self.assertIn("ZZ_REG_VCAP_PRE_CROP_PROBE_DATA_BASE", header)
        self.assertIn("ZZ_REG_VCAP_PRE_CROP_PROBE_META", header)
        self.assertIn("ZZ_REG_VCAP_PROBE_CONTROL", header)
        self.assertIn("arm_videocap_probe", text)
        self.assertIn("print_videocap_probe_comparison", text)
        self.assertIn("VideoCapProbeMatch", text)
        self.assertIn("VideoCapSamplerMatch", text)
        self.assertIn("VideoCapSamplerFirst", text)
        self.assertIn("VideoCapSamplerShift+1", text)
        self.assertIn("VideoCapSamplerShift-1", text)
        self.assertIn("later live DDR differs (advisory)", text)
        self.assertIn("VideoCapPreCropTarget", text)
        self.assertIn("VideoCapPreCropTransitions", text)
        self.assertIn("VideoCapPreCrop[", text)
        self.assertIn('#define ZZDIAG_VERSION "1.11"', text)
        self.assertIn("VideoCapOwnerStable", text)
        self.assertIn("ZZ_VCAP_PROBE_TARGET_X", header)
        self.assertIn("928U", header)
        self.assertIn("ZZ_VCAP_PROBE_SOURCE_X", header)
        self.assertIn("928U", header)

    def test_z2_aperture_handshake_uses_board_offsets_and_two_sided_gate(self):
        abi = self.read("include/zz9000_aperture.h")
        driver = self.read("rtg/mntgfx-gcc.c")
        diag = self.read("ZZDiag/ZZDiag.c")

        self.assertIn("ZZ_REG_Z2_APERTURE_INFO_HI 0x111cUL", abi)
        self.assertIn("ZZ_REG_Z2_APERTURE_INFO_LO 0x111eUL", abi)
        self.assertIn("ZZ_FW_CAP_Z2_APERTURE_LAYOUT", abi)
        self.assertIn("zz_z2_aperture_negotiate(aperture_info", driver)
        self.assertIn("aperture_status == ZZ_APERTURE_VALID", driver)
        self.assertIn("ZZ_Z2_APERTURE_ACK_TOKEN", driver)
        self.assertIn("ZZ_CARD_DATA_TEMPLATE_OFFSET", driver)
        self.assertNotIn("zz_template_addr = b->MemorySize", driver)
        self.assertIn("AutoConfigBoardSize", diag)
        self.assertIn("INVALID (descriptor/profile/AutoConfig mismatch)", diag)

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

    def test_ci_checks_matching_firmware_branch_with_master_fallback(self):
        ci = self.read(".github/workflows/ci.yml")
        self.assertIn("id: firmware-ref", ci)
        self.assertIn("git ls-remote --exit-code --heads", ci)
        self.assertIn('"refs/heads/$CANDIDATE_REF"', ci)
        self.assertIn("firmware_ref=master", ci)
        self.assertIn("ref: ${{ steps.firmware-ref.outputs.ref }}", ci)

    def test_ci_audio_jobs_use_build_scripts(self):
        ci = self.read(".github/workflows/ci.yml")
        # mhi/build.sh, ahi/driver/build.sh and ZZTop/build-gcc.sh stage
        # zz9k headers (via tools/stage-zz9k-headers.sh, cloning the SDK
        # at the pinned sdk/SDK_REF when no sibling checkout exists)
        # before re-execing into docker themselves, so CI calls all
        # three host-side.
        self.assertIn("mhi/build.sh", ci)
        self.assertNotIn('-w /src/mhi "$AMIGA_IMAGE" ./build.sh', ci)
        self.assertIn("ahi/driver/build.sh", ci)
        self.assertNotIn('-w /src/ahi/driver "$AMIGA_IMAGE" ./build.sh', ci)
        self.assertIn("ZZTop/build-gcc.sh", ci)
        self.assertIn(
            '-w /src/ahi/duplextest "$AMIGA_IMAGE" ./build.sh', ci
        )

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
            "zz9k.library",
            "mpega.library",
            "zz9k-picture.datatype",
            "amissl_v362.library",
        ):
            self.assertIn(artifact, script)

    def test_shell_scripts_parse(self):
        scripts = (
            "tools/amiga-docker.sh",
            "tools/build-all.sh",
            "tools/package-local.sh",
            "tools/check-release.sh",
            "tools/check-quality.sh",
            "tools/stage-zz9k-headers.sh",
            "rtg/build.sh",
            "net/build.sh",
            "usb-poseidon/build.sh",
            "sd-boot/build.sh",
            "ZZDiag/build.sh",
            "ahi/duplextest/build.sh",
            "sdk/build.sh",
            "amissl/build.sh",
        )
        for relpath in scripts:
            subprocess.run(["sh", "-n", str(ROOT / relpath)], check=True)

    def test_shell_scripts_avoid_ci_shellcheck_patterns(self):
        bad_cdpath = []
        bad_path_export = []
        # Enumerate tracked files rather than walking the tree: CI checks the
        # firmware repo out inside this workspace (check-cfg-keys.sh needs it),
        # and rglob would lint that repo's scripts against this repo's rules.
        tracked = subprocess.check_output(
            ["git", "ls-files", "*.sh"], cwd=ROOT, text=True
        ).splitlines()
        for relpath in tracked:
            path = ROOT / relpath
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
        artifacts = [
            path for path in tracked if path in GENERATED_ARTIFACT_PATHS
        ]
        self.assertEqual([], artifacts)

    def test_release_gate_matches_generated_artifact_policy(self):
        script = self.read("tools/check-release.sh")
        tracked_block = script.split("tracked_artifacts=$(", 1)[1]
        path_block = tracked_block.split("for path in \\", 1)[1]
        path_block = path_block.split("\n    do", 1)[0]
        release_paths = set()

        for line in path_block.splitlines():
            path = line.strip()
            if path.endswith("\\"):
                path = path[:-1].rstrip()
            if path:
                release_paths.add(path)

        self.assertEqual(set(GENERATED_ARTIFACT_PATHS), release_paths)

    def test_installer_string_literals_fit_classic_installer_limit(self):
        script = self.read("installer/ZZ9000Installer/Install ZZ9000")
        violations = []

        for match in re.finditer(r'"(?:\\.|[^"\\])*"', script, re.DOTALL):
            payload = match.group(0)[1:-1].encode("utf-8")
            if len(payload) > 512:
                line = script.count("\n", 0, match.start()) + 1
                violations.append(f"line {line}: {len(payload)} bytes")

        self.assertEqual([], violations)

    def test_locally_packaged_tools_are_ignored(self):
        package_script = self.read("tools/package-local.sh")
        tools = (
            "ZZTop",
            "ZZScanlines",
            "ZZFwUpdate",
            "ZZNetStats",
            "ZZDiag",
            "zz9k-info",
            "zz9k-services",
            "zz9k-view",
            "ZZPlay",
            "ZZPlay.info",
            "zz9k-mp3",
            "zz9k-cryptobench",
            "zz9k-archive",
        )
        placeholder = ROOT / "installer/ZZ9000Installer/Tools/.keep"

        self.assertTrue(placeholder.is_file())
        self.assertEqual(0, placeholder.stat().st_size)
        subprocess.run(
            ["git", "ls-files", "--error-unmatch", str(placeholder.relative_to(ROOT))],
            cwd=ROOT,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        for tool in tools:
            self.assertIn(tool, package_script)
            subprocess.run(
                [
                    "git",
                    "check-ignore",
                    "--quiet",
                    f"installer/ZZ9000Installer/Tools/{tool}",
                ],
                cwd=ROOT,
                check=True,
            )

    def test_audio_stack_uses_shared_ax_header(self):
        header = self.read("include/zz9000_ax.h")
        for token in (
            "ZZ_AX_BYTES_PER_PERIOD",
            "ZZ_AX_AUDIO_BUFSZ",
            "ZZ_AX_INT2_ENV",
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
            # R11 removed ZZ9K_MIX_LEVELS outright: the value-reading
            # helper is gone, and the only permitted literal is the
            # existence probe feeding the one-line ignore warning.
            self.assertNotIn("read_mix_levels_env", source)
            self.assertNotIn("ZZ9K_MIX_LEVELS C040", source)
            self.assertEqual(
                1, source.count('"ENV:ZZ9K_MIX_LEVELS"'),
                "ZZ9K_MIX_LEVELS may appear only as the warning probe",
            )
            self.assertIn("is ignored", source)

    def test_audio_owners_issue_no_dsp_param_stamps(self):
        """R4/U6: DSP stamps are gated on the absent capability.

        The master chain (LPF, mixer volume, EQ) belongs to the firmware
        scene module on control-plane firmware; drivers act as
        control-plane clients gated on ZZ9K_CAP_AUDIO_CONTROL,
        submitting a neutral source trim through the trim opcode and
        re-submitting it at release. The legacy anti-alias LPF stamps
        exist ONLY on the absent-capability (old firmware) branch --
        never on the control-plane path -- and no mixer stamp exists
        on either path.
        """
        ahi = self.read("ahi/driver/zz9000ax-ahi.c")
        mhi = self.read("mhi/mhizz9000.c")

        # AHI: no mixer stamp and no NOLPF bypass anywhere; the LPF
        # stamp exists exactly once, inside the absent-capability
        # branch after the caps gate (never on the control-plane path).
        self.assertNotIn("read_mix_levels_env", ahi)
        self.assertNotIn("ZZ_AX_NOLPF_ENV", ahi)
        self.assertNotIn("ZZ_AX_AP_DSP_SET_VOLUMES", ahi)
        self.assertEqual(1, ahi.count("ZZ_AX_AP_DSP_SET_LOWPASS"))
        alloc = ahi[
            ahi.index("intAHIsub_AllocAudio"):ahi.index("intAHIsub_FreeAudio")
        ]
        caps_gate = alloc.index("caps.capability_bits & ZZ9K_CAP_AUDIO_CONTROL")
        submit = alloc.index("submit_source_trim()")
        absent_gate = alloc.index("if (!audio_control_capped)")
        stamp = alloc.index(
            "write_audio_param(hw_addr, ZZ_AX_AP_DSP_SET_LOWPASS")
        self.assertLess(caps_gate, submit)
        self.assertLess(submit, absent_gate)
        self.assertLess(absent_gate, stamp)

        # MHI: no mixer stamp anywhere, no DSP stamp at allocate or
        # release; the Play-start LPF stamp sits inside the
        # absent-capability branch.
        self.assertNotIn("read_mix_levels_env", mhi)
        self.assertNotIn("ZZ_AX_AP_DSP_SET_VOLUMES", mhi)
        self.assertEqual(1, mhi.count("ZZ_AX_AP_DSP_SET_LOWPASS"))
        mhi_alloc = mhi[
            mhi.index("APTR i_MHIAllocDecoder"):mhi.index("void i_MHIFreeDecoder")
        ]
        self.assertNotIn("setAudioParam", mhi_alloc)
        play = mhi[mhi.index("void i_MHIPlay"):mhi.index("void i_MHIStop")]
        absent_gate = play.index("if(!mp->audio_control_capped)")
        stamp = play.index("setAudioParam(mp, ZZ_AX_AP_DSP_SET_LOWPASS")
        self.assertLess(absent_gate, stamp)
        release = mhi[
            mhi.index("void i_MHIFreeDecoder"):mhi.index("BOOL i_MHIQueueBuffer")
        ]
        self.assertNotIn("setAudioParam", release)

        # Both drivers gate on the capability and submit the trim
        # (allocate and release).
        for source in (ahi, mhi):
            self.assertIn("ZZ9K_CAP_AUDIO_CONTROL", source)
            self.assertIn("(caps.capability_bits & ZZ9K_CAP_AUDIO_CONTROL)",
                          source)
            self.assertIn("submit_source_trim()", source)
            self.assertIn("ZZ9K_OP_AUDIO_TRIM_SUBMIT", source)
            self.assertIn("ZZ9K_AUDIO_BALANCE_NEUTRAL", source)

        # The app mixer API is legacy-only: master-chain SetParam writes
        # stay behind the absent-capability gate and report the
        # documented not-supported status on control-plane firmware.
        setparam = mhi[
            mhi.index("ULONG i_MHISetParam"):mhi.rindex("return MHIF_SUPPORTED;")
        ]
        self.assertIn("mp->audio_control_capped", setparam)
        self.assertIn("MHIF_UNSUPPORTED", setparam)

        # The per-consumer build-script gates fail fast when the staged
        # SDK predates the control-plane ABI.
        self.assertIn("ZZ9K_OP_AUDIO_TRIM_SUBMIT",
                      self.read("ahi/driver/build.sh"))
        self.assertIn("ZZ9K_OP_AUDIO_TRIM_SUBMIT",
                      self.read("mhi/build.sh"))
        self.assertIn("ZZ9K_OP_AUDIO_SCENE_SELECT",
                      self.read("ZZTop/build-gcc.sh"))

    def test_ahi_initializes_period_size_before_enabling_interrupt(self):
        source = self.read("ahi/driver/zz9000ax-ahi.c")
        assign = source.index("AudioCtrl->ahiac_BuffSamples =")
        start = source.index("intAHIsub_Start")
        self.assertLess(assign, start)

    def test_mhi_zero_length_buffer_announces_eof(self):
        source = self.read("mhi/mhizz9000.c")
        body = source[
            source.index("BOOL i_MHIQueueBuffer"):
            source.index("APTR i_MHIGetEmpty")
        ]
        self.assertIn("static BOOL mhi_stream_eof", source)
        self.assertIn("ZZ9K_AUDIO_STREAM_FEED_EOF", source)
        feed_body = source[
            source.index("static BOOL mhi_stream_feed_eof_locked"):
            source.index("static BOOL mhi_stream_eof")
        ]
        self.assertIn("ZZ9K_AUDIO_STREAM_FEED_EOF", feed_body)
        eof_body = source[
            source.index("static BOOL mhi_stream_eof"):
            source.index("static BOOL mhi_stream_service_drain")
        ]
        self.assertIn("old_status == MHIF_OUT_OF_DATA", eof_body)
        self.assertIn("mp->Status = MHIF_PLAYING;", eof_body)
        self.assertLess(eof_body.index("mp->Status = MHIF_PLAYING;"),
                        eof_body.index("mhi_stream_feed_eof_locked(mp)"))
        self.assertIn("if(accepted) mhi_wake_feeder(mp);", eof_body)
        self.assertIn("if(mhi_size == 0)", body)
        self.assertIn("return mhi_stream_eof(mp);", body)
        self.assertIn("!mhi_buffer", body)
        self.assertLess(body.index("if(mhi_size == 0)"),
                        body.index("!mhi_buffer"))
        self.assertLess(body.index("!mhi_buffer"),
                        body.index("AllocVec(sizeof(struct ListNode)"))

    def test_mhi_queue_exhaustion_requests_resumable_drain(self):
        source = self.read("mhi/mhizz9000.c")
        header = self.read("mhi/mhilib.h")
        self.assertIn("#define ZZ_MHI_DRAIN_IDLE_POLLS", source)
        self.assertIn("static BOOL mhi_stream_service_drain", source)
        drain = source[
            source.index("static BOOL mhi_stream_service_drain"):
            source.index("/* ********************* */\n/*  BEGIN feeder process */")
        ]
        self.assertIn("ZZ9K_AUDIO_STREAM_RESULT_NEED_INPUT", drain)
        self.assertIn("mhi_stream_feed_drain_locked(mp)", drain)
        self.assertIn("ZZ9K_AUDIO_STREAM_RESULT_DRAINED", drain)
        self.assertNotIn("mhi_stream_feed_eof_locked(mp)", drain)
        self.assertIn("mp->Status = MHIF_OUT_OF_DATA", drain)
        self.assertIn("mhi_signal_app(mp)", drain)
        feeder = source[
            source.index("static void mhi_feeder(void)"):
            source.index("/* ******************* */\n/*  END feeder process */")
        ]
        self.assertIn("drain_busy = mhi_stream_service_drain(mp);", feeder)
        self.assertIn("mp->play_pending || drain_busy", feeder)
        status = source[
            source.index("UBYTE i_MHIGetStatus"):
            source.index("void i_MHIPlay")
        ]
        self.assertIn("return mp->Status;", status)
        self.assertNotIn("ZZ9KAudioStream", status)
        self.assertIn("UBYTE eof_announced", header)
        self.assertIn("UBYTE drain_requested", header)
        self.assertIn("UBYTE starvation_polls", header)
        retry = source[
            source.index("static BOOL mhi_stream_status_retryable"):
            source.index("static BOOL mhi_stream_service_drain")
        ]
        self.assertIn("ZZ9K_STATUS_BUSY", retry)
        self.assertIn("ZZ9K_STATUS_TIMEOUT", retry)
        self.assertIn("mhi_stream_status_retryable(rc)", drain)
        self.assertIn("mp->Status = MHIF_STOPPED", drain)
        build = self.read("mhi/build.sh")
        self.assertIn("ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_DRAIN", build)

    def test_mhi_buffer_completion_tracks_decoder_consumption(self):
        source = self.read("mhi/mhizz9000.c")
        header = self.read("mhi/mhilib.h")
        self.assertIn("ULONG submitted_bytes", header)
        self.assertRegex(header, r"ULONG\s+StreamEnd")
        self.assertRegex(header, r"UBYTE\s+StreamEndValid")
        self.assertNotIn("HoldUntilDrain", header)
        complete = source[
            source.index("static void mhi_complete_consumed"):
            source.index("static void mhi_feed_pending")
        ]
        self.assertIn("node->StreamEnd", complete)
        self.assertIn("node->StreamEndValid", complete)
        self.assertNotIn("node->StreamEnd == 0", complete)
        self.assertIn("mp->result.bytes_consumed", complete)
        self.assertIn("node->Played = TRUE", complete)
        self.assertIn("mhi_signal_app(mp)", complete)
        feed = source[
            source.index("static void mhi_feed_pending"):
            source.index("static BOOL mhi_stream_open")
        ]
        self.assertIn("it->Index < it->Size", feed)
        self.assertIn("mp->submitted_bytes += chunk", feed)
        self.assertIn("node->StreamEnd = mp->submitted_bytes", feed)
        self.assertIn("node->StreamEndValid = TRUE", feed)
        self.assertIn("mp->drain_requested = FALSE", feed)
        self.assertIn("mhi_complete_consumed(mp", feed)
        self.assertNotIn("Fully handed to the card: the app may reclaim it", feed)
        get_empty = source[
            source.index("APTR i_MHIGetEmpty"):
            source.index("UBYTE i_MHIGetStatus")
        ]
        self.assertNotIn("wake_eof", get_empty)
        self.assertNotIn("mhi_signal_app(mp);", get_empty)
        queue = source[
            source.index("BOOL i_MHIQueueBuffer"):
            source.index("APTR i_MHIGetEmpty")
        ]
        self.assertIn("BufferNode->StreamEndValid = FALSE", queue)

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
        self.assertIn("ahi_data->play_stop = 1;", stop_body)
        self.assertIn("ahi_data->record_stop = 1;", stop_body)
        self.assertIn("update_hw_interrupts(ahi_data);", stop_body)
        self.assertIn("ahi_data->buf_offset = 0;", stop_body)
        self.assertIn("zero_hw_audio_ring(ahi_data);", stop_body)
        self.assertIn("ahi_data->buf_offset = 0;", start_body)
        self.assertIn("zero_hw_audio_ring(ahi_data);", start_body)
        self.assertIn("ahi_data->record_sequence =", start_body)
        self.assertIn("ahi_data->record_stop = 0;", start_body)
        self.assertIn("update_hw_interrupts(ahi_data);", start_body)

    def test_ahi_recording_protocol_and_callback_contract(self):
        shared = self.read("include/zz9000_ax.h")
        header = self.read("ahi/driver/zz9000ax-ahi.h")
        source = self.read("ahi/driver/zz9000ax-ahi.c")

        for token in (
            "ZZ_AX_AUDIO_CONFIG_RECORD",
            "ZZ_AX_AUDIO_CONFIG_TX_STATUS_CAPABLE",
            "ZZ_AX_AUDIO_RX_STATUS_CAPABLE",
            "ZZ_AX_AUDIO_RX_STATUS_PERIOD_MASK",
            "ZZ_AX_AUDIO_RX_STATUS_SEQUENCE_MASK",
            "ZZ_AX_AUDIO_TX_STATUS_CAPABLE",
            "ZZ_AX_AUDIO_TX_STATUS_PERIOD_MASK",
            "ZZ_AX_AUDIO_TX_STATUS_SEQUENCE_MASK",
            "ZZ_AX_RX_BUFFER_DELTA",
        ):
            self.assertIn(token, shared)

        self.assertIn("ZZ_REG_AUDIO_TX_STATUS",
                      self.read("include/zz9000_hw.h"))

        self.assertIn("recording_supported", source)
        self.assertIn("playback_period_ready", source)
        self.assertIn("sequence == ahi_data->play_sequence", source)
        self.assertIn("zz_ax_audio_tx_status_sequence(status)", source)
        self.assertIn("zz_ax_audio_tx_status_period(status)", source)
        self.assertIn("period_offset + ZZ_AX_BYTES_PER_PERIOD", source)
        self.assertIn("uint8_t tx_status_capable;", header)
        self.assertIn("ahi_data->tx_status_capable", source)
        self.assertIn("AHISF_CANRECORD", source)
        self.assertIn("AHIDB_Record", source)
        self.assertIn("AHIDB_FullDuplex", source)
        self.assertIn("AHIDB_MaxRecordSamples", source)
        self.assertIn("AHISF_RECORD", source)
        self.assertIn("AHIST_S16S", source)
        self.assertIn("ahiac_SamplerFunc", source)
        self.assertIn("struct AHIRecordMessage record_message;",
                      header)
        self.assertIn("uint16_t play_sequence;",
                      header)

    def test_ahi_exclusive_owner_is_claimed_before_hardware_mutation(self):
        header = self.read("ahi/driver/zz9000ax-ahi.h")
        source = self.read("ahi/driver/zz9000ax-ahi.c")
        alloc_body = source[
            source.index("intAHIsub_AllocAudio"):
            source.index("static void __attribute__((used)) intAHIsub_FreeAudio")
        ]
        free_body = source[
            source.index("intAHIsub_FreeAudio"):
            source.index("intAHIsub_Stop")
        ]
        destroy_body = source[
            source.index("void destroy_interrupt"):
            source.index("static BOOL mhi_present_locked")
        ]

        self.assertIn("struct z9ax *owner;", header)
        self.assertIn("Z9AXBase->owner = NULL;", source)
        self.assertIn("Z9AXBase->owner || mhi_present_locked()", alloc_body)
        self.assertIn("Z9AXBase->owner = ahi_data;", alloc_body)
        forbid = alloc_body.index("Forbid();")
        owner_check = alloc_body.index(
            "Z9AXBase->owner || mhi_present_locked()"
        )
        owner_publish = alloc_body.index("Z9AXBase->owner = ahi_data;")
        irq_install = alloc_body.index(
            "install_irq_server_locked(ahi_data);", owner_publish
        )
        success_permit = alloc_body.index("Permit();", irq_install)
        self.assertLess(forbid, owner_check)
        self.assertLess(owner_check, owner_publish)
        self.assertLess(owner_publish, irq_install)
        self.assertLess(irq_install, success_permit)
        self.assertLess(
            owner_check,
            alloc_body.index("write_reg(hw_addr, ZZ_REG_AUDIO_CONFIG, 0)")
        )
        self.assertLess(
            owner_check,
            alloc_body.index("zero_hw_audio_ring(ahi_data)")
        )
        self.assertIn("Z9AXBase->owner = NULL;", destroy_body)
        self.assertLess(
            free_body.index("disable_hw_interrupt(ahi_data);"),
            free_body.index("destroy_interrupt(ahi_data);")
        )

    def test_ahi_duplex_tool_uses_one_control_for_both_directions(self):
        source = self.read("ahi/duplextest/ZZAXDuplexTest.c")
        start_body = source[
            source.index("struct TagItem start_tags[]"):
            source.index("control_result = AHI_ControlAudioA")
        ]
        hook_body = source[
            source.index("static ULONG record_hook"):
            source.index("static void put_le16")
        ]

        self.assertEqual(1, source.count("audioctrl = AHI_AllocAudioA("))
        self.assertIn("{ AHIC_Play, TRUE }", start_body)
        self.assertNotIn("capture_only", source)
        self.assertIn("{ AHIC_Record, TRUE }", start_body)
        self.assertIn("{ AHIA_RecordFunc, (ULONG)&hook }", source)
        self.assertIn("message->ahirm_Type != AHIST_S16S", hook_body)
        self.assertLess(
            hook_body.index(
                "if (context->frames >= context->capacity_frames)"
            ),
            hook_body.index("message->ahirm_Type != AHIST_S16S")
        )
        self.assertIn("if (frames > remaining) {\n    frames = remaining;", hook_body)
        self.assertNotIn("overflow_frames", source)
        self.assertNotIn("Write(", hook_body)
        self.assertIn("if (!Close(file))", source)
        self.assertIn("DeleteFile((CONST_STRPTR)path);", source)

    def test_ahi_recording_drains_only_resident_periods(self):
        source = self.read("ahi/driver/zz9000ax-ahi.c")
        body = source[
            source.index("static void process_recording"):
            source.index("void WorkerProcess")
        ]

        self.assertIn("zz_ax_audio_rx_sequence_distance", body)
        self.assertIn("available > ZZ_AX_AUDIO_RX_RESIDENT_PERIODS", body)
        self.assertIn("available = ZZ_AX_AUDIO_RX_RESIDENT_PERIODS", body)
        self.assertIn("newest_period + ZZ_AX_AUDIO_PERIODS", body)
        self.assertIn("period * ZZ_AX_BYTES_PER_PERIOD", body)


if __name__ == "__main__":
    unittest.main()
