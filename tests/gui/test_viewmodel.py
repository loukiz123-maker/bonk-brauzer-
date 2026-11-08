"""UI logic tests - these run without a display server or Qt."""

from __future__ import annotations

import pytest

from lynxdroid.backend.contract import (
    BackendCapabilities,
    BackendState,
    Feature,
    Readiness,
    Snapshot,
    Support,
)
from lynxdroid.core.diagnostics import Check, Report, Status
from lynxdroid.display.geometry import Size, ViewportGeometry
from lynxdroid.gui import viewmodel as vm
from lynxdroid.gui.theme import DARK, LIGHT, contrast_ratio, palette_for, stylesheet
from lynxdroid.input.channels import Capabilities as ChannelCapabilities
from lynxdroid.input.channels import ChannelKind
from lynxdroid.profiles.model import Element, ElementType, Profile, Target
from lynxdroid.settings import Settings


def caps(**features) -> BackendCapabilities:
    items = tuple(
        Feature(
            key,
            key.replace("-", " ").title(),
            Support.SUPPORTED if value else Support.UNSUPPORTED,
            "" if value else f"{key} is not available",
        )
        for key, value in features.items()
    )
    return BackendCapabilities("waydroid", "1.5.0", items, max_touches=1)


def snapshot(state: BackendState, **readiness) -> Snapshot:
    return Snapshot(state=state, readiness=dict(readiness.items()), detail="test", error=None)


class TestStatusPanel:
    @pytest.mark.parametrize(
        "state,tone",
        [
            (BackendState.RUNNING, vm.Tone.POSITIVE),
            (BackendState.STARTING, vm.Tone.INFO),
            (BackendState.ERROR, vm.Tone.DANGER),
            (BackendState.STOPPED, vm.Tone.NEUTRAL),
            (BackendState.NOT_CONFIGURED, vm.Tone.WARNING),
        ],
    )
    def test_tone_matches_state(self, state: BackendState, tone: vm.Tone) -> None:
        panel = vm.status_panel(snapshot(state), caps(session=True))
        assert panel.tone is tone
        assert panel.headline

    def test_layers_are_listed_with_human_text(self) -> None:
        panel = vm.status_panel(
            snapshot(BackendState.RUNNING, container=Readiness.READY, android=Readiness.NOT_READY),
            caps(session=True),
        )
        labels = {label: text for label, _readiness, text in panel.layers}
        assert labels["Container service"] == "ready"
        assert labels["Android system"] == "not ready"
        assert labels["Graphics path"] == "not checked"

    def test_error_detail_is_surfaced(self) -> None:
        snap = Snapshot(state=BackendState.ERROR, error="binder missing", detail="")
        assert "binder missing" in vm.status_panel(snap, caps(session=True)).detail


class TestControls:
    def test_start_enabled_when_stopped(self) -> None:
        assert vm.start_control(snapshot(BackendState.STOPPED), caps(session=True)).enabled

    def test_start_disabled_when_running(self) -> None:
        state = vm.start_control(snapshot(BackendState.RUNNING), caps(session=True))
        assert not state.enabled
        assert "already running" in state.reason

    def test_start_disabled_without_backend_support(self) -> None:
        state = vm.start_control(snapshot(BackendState.STOPPED), caps(session=False))
        assert not state.enabled
        assert "not available" in state.reason

    def test_stop_during_start_is_accepted_with_explanation(self) -> None:
        state = vm.stop_control(snapshot(BackendState.STARTING), caps(session=True))
        assert state.enabled
        assert "as soon as the start finishes" in state.reason

    def test_stop_disabled_when_stopped(self) -> None:
        assert not vm.stop_control(snapshot(BackendState.STOPPED), caps(session=True)).enabled

    def test_overlay_requires_multitouch_channel(self) -> None:
        profile = Profile.new("P", Target(package="com.a.b"))
        single = ChannelCapabilities(ChannelKind.ADB_SHELL, True, 1, False, False)
        multi = ChannelCapabilities(ChannelKind.AGENT, True, 10, True, True)
        assert not vm.overlay_control(caps(session=True), single, profile).enabled
        assert vm.overlay_control(caps(session=True), multi, profile).enabled

    def test_overlay_explains_missing_profile(self) -> None:
        multi = ChannelCapabilities(ChannelKind.AGENT, True, 10, True, True)
        state = vm.overlay_control(caps(session=True), multi, None)
        assert not state.enabled
        assert "profile" in state.reason

    def test_display_control_propagates_reason(self) -> None:
        state = vm.display_control(caps(**{"display-props": False}))
        assert not state.enabled
        assert "not available" in state.reason


class TestBanners:
    def test_no_banners_for_a_clean_state(self) -> None:
        found = vm.banners(
            snapshot=snapshot(BackendState.RUNNING),
            caps=caps(session=True),
            report=None,
            settings=Settings(),
            channel=None,
        )
        assert found == []

    def test_error_banner_is_danger(self) -> None:
        snap = Snapshot(state=BackendState.ERROR, error="binder missing")
        found = vm.banners(
            snapshot=snap, caps=caps(session=True), report=None, settings=Settings(), channel=None
        )
        assert any(b.tone is vm.Tone.DANGER and "binder missing" in b.detail for b in found)

    def test_multi_instance_is_flagged_as_unverified(self) -> None:
        base = caps(session=True)
        with_multi = BackendCapabilities(
            base.name,
            base.version,
            (
                *base.features,
                Feature(
                    "multi-instance",
                    "Instances",
                    Support.UNTESTED,
                    "waydroid does not document isolated instances",
                ),
            ),
            notes=base.notes,
        )
        found = vm.banners(
            snapshot=None, caps=with_multi, report=None, settings=Settings(), channel=None
        )
        assert any("not verified" in b.text for b in found)

    def test_arm_warning_respects_settings(self) -> None:
        base = caps(session=True)
        with_arm = BackendCapabilities(
            base.name,
            base.version,
            (*base.features, Feature("arm-translation", "ARM", Support.UNTESTED, "nope")),
            notes=(),
        )
        shown = vm.banners(
            snapshot=None, caps=with_arm, report=None, settings=Settings(), channel=None
        )
        assert any("ARM-only" in b.text for b in shown)
        hidden = vm.banners(
            snapshot=None,
            caps=with_arm,
            report=None,
            settings=Settings(arm_translation_warning=False),
            channel=None,
        )
        assert not any("ARM-only" in b.text for b in hidden)

    def test_blocking_checks_create_a_danger_banner(self) -> None:
        report = Report(area="host")
        report.add(Check("kernel.binder", "Binder", Status.ERROR, "no binder"))
        found = vm.banners(
            snapshot=None, caps=None, report=report, settings=Settings(), channel=None
        )
        assert any(b.tone is vm.Tone.DANGER for b in found)

    def test_single_touch_channel_warns(self) -> None:
        channel = ChannelCapabilities(
            ChannelKind.ADB_SHELL, True, 1, False, False, reason="taps only"
        )
        found = vm.banners(
            snapshot=None, caps=None, report=None, settings=Settings(), channel=channel
        )
        assert any("Single-touch" in b.text for b in found)


class TestDisplayForm:
    def test_valid_form_has_no_issues(self) -> None:
        form = vm.DisplayForm("1920", "1080", "320").validated()
        assert form.is_valid
        assert form.preset_name == "1080p landscape"
        assert form.message() == ""

    def test_invalid_form_reports_field(self) -> None:
        form = vm.DisplayForm("0", "1080", "320").validated()
        assert not form.is_valid
        assert "width" in form.message()

    def test_orientation_is_derived(self) -> None:
        assert vm.DisplayForm("1080", "1920", "320").validated().orientation is not None

    def test_from_settings_round_trips(self) -> None:
        form = vm.DisplayForm.from_settings(Settings(buffer_width=1280, buffer_height=720, dpi=240))
        assert (form.width, form.height, form.dpi) == ("1280", "720", "240")

    def test_preset_rows_cover_every_preset(self) -> None:
        rows = vm.preset_rows()
        assert len(rows) >= 4
        assert all(len(row) == 3 for row in rows)


class TestProfileRows:
    def test_aspect_mismatch_is_flagged(self) -> None:
        profile = Profile.new("Old", Target(package="com.a.b"))
        rows = vm.profile_rows([profile], live_buffer=(1080, 1920))
        assert rows[0].warning
        assert rows[0].tone is vm.Tone.WARNING

    def test_matching_buffer_has_no_warning(self) -> None:
        profile = Profile.new("Same", Target(package="com.a.b"))
        assert vm.profile_rows([profile], live_buffer=(1920, 1080))[0].warning == ""

    def test_key_conflict_is_surfaced(self) -> None:
        elements = (
            Element(id="a", type=ElementType.TAP, label="A", x=0.2, y=0.2, keys=("j",)),
            Element(id="b", type=ElementType.TAP, label="B", x=0.8, y=0.8, keys=("j",)),
        )
        # The model refuses duplicates at validate time, so build a raw profile
        # object the way a hand-edited file could look before validation.
        profile = Profile.__new__(Profile)
        object.__setattr__(profile, "id", "x")
        object.__setattr__(profile, "name", "Conflict")
        object.__setattr__(profile, "target", Target(package="com.a.b"))
        object.__setattr__(profile, "elements", elements)
        object.__setattr__(
            profile,
            "reference",
            profile.__class__.__dataclass_fields__
            and __import__("lynxdroid.profiles.model", fromlist=["x"]).ReferenceResolution(
                1920, 1080, 320
            ),
        )
        rows = vm.profile_rows([profile])
        assert "key conflict" in rows[0].warning


class TestCheckRows:
    def test_every_check_becomes_a_row(self) -> None:
        report = Report(area="x")
        report.add(Check("a", "A", Status.OK, "fine"))
        report.add(Check("b", "B", Status.ERROR, "broken", next_step="install it"))
        rows = vm.check_rows(report)
        assert len(rows) == 2
        assert rows[1].tone is vm.Tone.DANGER
        assert rows[1].next_step == "install it"

    def test_hide_ok_filters_rows(self) -> None:
        report = Report(area="x")
        report.add(Check("a", "A", Status.OK, "fine"))
        report.add(Check("b", "B", Status.WARNING, "meh"))
        assert len(vm.check_rows(report, include_ok=False)) == 1

    def test_summary_tones(self) -> None:
        report = Report(area="x")
        report.add(Check("a", "A", Status.OK, "fine"))
        assert vm.report_summary(report)[1] is vm.Tone.POSITIVE
        report.add(Check("b", "B", Status.WARNING, "meh"))
        assert vm.report_summary(report)[1] is vm.Tone.WARNING
        report.add(Check("c", "C", Status.ERROR, "bad"))
        assert vm.report_summary(report)[1] is vm.Tone.DANGER


class TestMisc:
    def test_geometry_caption_mentions_bars(self) -> None:
        geometry = ViewportGeometry(buffer=Size(1920, 1080), surface=Size(1000, 400))
        caption = vm.geometry_caption(geometry)
        assert "1920x1080" in caption
        assert "black bars" in caption

    def test_geometry_caption_without_surface(self) -> None:
        assert "no Android surface" in vm.geometry_caption(None)

    def test_element_summary_per_type(self) -> None:
        assert "tap" in vm.element_summary(
            Element(id="a", type=ElementType.TAP, label="A", x=0.1, y=0.1, keys=("j",))
        )
        assert "stick" in vm.element_summary(
            Element(
                id="d",
                type=ElementType.DPAD,
                label="D",
                x=0.5,
                y=0.5,
                dpad_keys=("w", "d", "s", "a"),
            )
        )
        assert "swipe" in vm.element_summary(
            Element(
                id="s",
                type=ElementType.SWIPE,
                label="S",
                x=0.2,
                y=0.2,
                keys=("z",),
                swipe_to=(0.8, 0.8),
            )
        )

    def test_editor_hint_exists_for_every_type(self) -> None:
        for element_type in ElementType:
            assert vm.editor_hint(element_type)

    def test_apply_changes_mentions_restart(self) -> None:
        message, tone = vm.apply_changes_message(
            ["persist.waydroid.width: 1280 -> 1920", "buffer size takes effect on restart"]
        )
        assert "restart" in message
        assert tone is vm.Tone.INFO

    def test_empty_state_texts(self) -> None:
        assert "Waydroid is not installed" in vm.empty_state_text(False, False)
        assert "diagnostic" in vm.empty_state_text(True, False)
        assert "Ready" in vm.empty_state_text(True, True)

    def test_consent_text_is_explicit(self) -> None:
        text = vm.consent_text("Change density", "runs wm density")
        assert "Change density" in text
        assert "wm density" in text
        assert "persists" in text


class TestTheme:
    @pytest.mark.parametrize("palette", [DARK, LIGHT])
    def test_body_text_has_aa_contrast(self, palette) -> None:
        assert contrast_ratio(palette.text, palette.window) >= 4.5
        assert contrast_ratio(palette.text, palette.surface) >= 4.5

    @pytest.mark.parametrize("palette", [DARK, LIGHT])
    def test_muted_text_is_still_readable(self, palette) -> None:
        # Muted text carries hints and next steps: it must stay legible.
        assert contrast_ratio(palette.text_muted, palette.surface) >= 4.5

    @pytest.mark.parametrize("palette", [DARK, LIGHT])
    def test_primary_button_text_is_readable(self, palette) -> None:
        assert contrast_ratio(palette.accent_text, palette.accent) >= 4.5

    def test_unknown_palette_falls_back_to_dark(self) -> None:
        assert palette_for("neon").name == "dark"

    def test_stylesheet_contains_every_palette_value(self) -> None:
        sheet = stylesheet(DARK)
        for value in (DARK.window, DARK.accent, DARK.danger, DARK.text):
            assert value in sheet

    def test_tone_colours_are_distinct(self) -> None:
        colours = {DARK.tone_colour(tone) for tone in vm.Tone}
        assert len(colours) == len(vm.Tone)

    def test_contrast_ratio_rejects_bad_hex(self) -> None:
        with pytest.raises(ValueError):
            contrast_ratio("red", "#000000")
