import pathlib
import subprocess
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
        # mhi/build.sh stages zz9k headers (cloning the SDK at the
        # pinned sdk/SDK_REF when no sibling checkout exists) before
        # re-execing into docker itself, so CI calls it host-side.
        self.assertIn("mhi/build.sh", ci)
        self.assertNotIn('-w /src/mhi "$AMIGA_IMAGE" ./build.sh', ci)
        self.assertIn('-w /src/ahi/driver "$AMIGA_IMAGE" ./build.sh', ci)
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
