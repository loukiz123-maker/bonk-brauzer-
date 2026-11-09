"""Input engine tests: the multitouch guarantees from stages 8 and 10."""

from __future__ import annotations

import pytest

from lynxdroid.display.geometry import Rotation, Size, ViewportGeometry
from lynxdroid.input.engine import InputEngine, keys_of
from lynxdroid.input.events import Action, TouchEvent, TouchSlotExhausted
from lynxdroid.profiles.model import Element, ElementType, Profile


class Clock:
    """Deterministic millisecond clock."""

    def __init__(self, start: int = 1_000) -> None:
        self.now = start

    def __call__(self) -> int:
        return self.now

    def advance(self, ms: int) -> None:
        self.now += ms


@pytest.fixture()
def clock() -> Clock:
    return Clock()


@pytest.fixture()
def geo() -> ViewportGeometry:
    return ViewportGeometry(buffer=Size(1000, 500), surface=Size(1000, 500))


def build(profile: Profile, geo: ViewportGeometry, clock: Clock, capacity: int = 10) -> InputEngine:
    return InputEngine(profile, geo, clock=clock, capacity=capacity)


def actions(events: list[TouchEvent]) -> list[tuple[str, int]]:
    return [(event.action.value, event.pointer) for event in events]


class TestTap:
    def test_tap_emits_exactly_one_down_up_pair(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        events = engine.key_down("j")
        assert actions(events) == [("down", 1), ("up", 1)]
        assert engine.key_up("j") == []
        assert events[0].x == pytest.approx(750.0)
        assert events[0].y == pytest.approx(250.0)
        assert events[1].x == events[0].x and events[1].y == events[0].y

    def test_auto_repeat_does_not_produce_extra_taps(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("j")
        engine.key_up("j")
        for _ in range(5):
            assert engine.key_down("j", repeat=True) == []
        assert len(engine.active_pointers) == 0

    def test_unbound_key_is_ignored(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        assert engine.key_down("q") == []
        assert engine.key_up("q") == []

    def test_pointer_is_released_after_tap(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("j")
        engine.key_up("j")
        assert engine.active_pointers == {}

    def test_tap_at_corner_is_clamped_inside_buffer(self, target, geo, clock) -> None:
        corner = Element(id="corner", type=ElementType.TAP, label="C", x=1.0, y=1.0, keys=("c",))
        profile = Profile.new("Corner", target, elements=(corner,))
        engine = build(profile, geo, clock)
        down = engine.key_down("c")[0]
        assert (down.x, down.y) == (999.0, 499.0)


class TestHold:
    def test_hold_lives_until_key_release(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("k")
        clock.advance(2_500)
        assert len(engine.active_pointers) == 1
        released = engine.key_up("k")
        assert actions(released) == [("up", 1)]
        assert engine.active_pointers == {}

    def test_duplicate_key_down_does_not_open_a_second_contact(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("k")
        assert engine.key_down("k") == []
        assert len(engine.active_pointers) == 1


class TestDpad:
    def test_first_direction_presses_down_at_centre_then_moves(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        events = engine.key_down("d")
        assert actions(events) == [("down", 1), ("move", 1)]
        centre = (200.0, 375.0)
        assert (events[0].x, events[0].y) == pytest.approx(centre)
        assert events[1].x > centre[0]
        assert events[1].y == pytest.approx(centre[1])

    def test_diagonal_is_normalised_to_the_radius(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("w")
        events = engine.key_down("d")
        move = events[-1]
        dx = move.x - 200.0
        dy = move.y - 375.0
        radius = 0.08 * 500  # element radius x shorter edge
        assert (dx**2 + dy**2) ** 0.5 == pytest.approx(radius, rel=1e-6)
        assert abs(dx) == pytest.approx(abs(dy), rel=1e-6)  # 45 degrees

    def test_w_plus_d_stays_inside_radius(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("w")
        engine.key_down("d")
        contact = next(iter(engine.active_pointers.values()))
        distance = ((contact.x - 200.0) ** 2 + (contact.y - 375.0) ** 2) ** 0.5
        assert distance <= 0.08 * 500 + 1e-6

    def test_releasing_one_key_keeps_the_other_direction(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("w")
        engine.key_down("d")
        events = engine.key_up("w")
        assert actions(events) == [("move", 1)]
        contact = next(iter(engine.active_pointers.values()))
        assert contact.x > 200.0
        assert contact.y == pytest.approx(375.0)  # vertical component removed

    def test_releasing_every_key_closes_the_contact(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("w")
        engine.key_down("d")
        engine.key_up("w")
        events = engine.key_up("d")
        assert actions(events) == [("up", 1)]
        assert engine.active_pointers == {}

    def test_opposite_keys_cancel_to_centre_without_crashing(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("w")
        events = engine.key_down("s")
        contact = next(iter(engine.active_pointers.values()))
        assert actions(events) == [("move", 1)]
        assert (contact.x, contact.y) == pytest.approx((200.0, 375.0))
        engine.key_up("s")
        contact = next(iter(engine.active_pointers.values()))
        assert contact.y == pytest.approx(375.0 - 0.08 * 500)  # clean "up"

    def test_rapid_direction_switching_keeps_one_contact(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        for key in ("w", "d", "s", "a", "w"):
            engine.key_down(key)
            engine.key_up(key)
        assert len(engine.active_pointers) == 0
        assert len(engine._queue.drain()) > 0  # events were produced


class TestSimultaneous:
    def test_move_and_action_use_independent_pointers(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("d")  # dpad -> pointer 1
        engine.key_down("k")  # hold -> pointer 2
        down_a = engine.key_down("j")  # tap -> pointer 3, released immediately
        pointers = set(engine.active_pointers)
        assert len(pointers) == 2
        assert len(pointers) == 2
        taps = [e for e in down_a + engine.key_up("j") if e.source == "tap-a"]
        assert taps[0].pointer not in pointers  # tap slot already returned

    def test_eight_simultaneous_contacts(self, target, geo, clock) -> None:
        elements = tuple(
            Element(
                id=f"hold-{i}",
                type=ElementType.HOLD,
                label=f"H{i}",
                x=0.1 * i,
                y=0.5,
                keys=(f"{i}",),
            )
            for i in range(8)
        )
        profile = Profile.new("Octopus", target, elements=elements)
        engine = build(profile, geo, clock)
        for i in range(8):
            engine.key_down(f"{i}")
        assert len(engine.active_pointers) == 8
        assert len(set(engine.active_pointers)) == 8
        for i in range(8):
            engine.key_up(f"{i}")
        assert engine.active_pointers == {}

    def test_slot_exhaustion_raises_and_leaves_no_zombie(self, target, geo, clock) -> None:
        elements = tuple(
            Element(
                id=f"hold-{i}",
                type=ElementType.HOLD,
                label=f"H{i}",
                x=0.1 * i,
                y=0.5,
                keys=(f"{i}",),
            )
            for i in range(3)
        )
        profile = Profile.new("Three", target, elements=elements)
        engine = build(profile, geo, clock, capacity=2)
        engine.key_down("0")
        engine.key_down("1")
        with pytest.raises(TouchSlotExhausted):
            engine.key_down("2")
        assert len(engine.active_pointers) == 2
        errors = engine.drain_errors()
        assert len(errors) == 1
        assert errors[0].code == "touch-slots-exhausted"
        # The refused key is not remembered as held...
        engine.key_up("2")  # no-op: no contact, no events
        # ...and releasing one slot lets the next key through.
        engine.key_up("0")
        engine.key_down("2")
        assert len(engine.active_pointers) == 2

    def test_pointer_ids_are_never_reused_while_open(self, target, geo, clock) -> None:
        elements = tuple(
            Element(
                id=f"hold-{i}",
                type=ElementType.HOLD,
                label=f"H{i}",
                x=0.1 * (i + 1),
                y=0.5,
                keys=(f"{i}",),
            )
            for i in range(4)
        )
        profile = Profile.new("Four", target, elements=elements)
        engine = build(profile, geo, clock)
        engine.key_down("0")
        engine.key_down("1")
        pointers = [c.pointer for c in engine.active_pointers.values()]
        assert len(pointers) == len(set(pointers))  # never two contacts, one id
        freed = next(c.pointer for c in engine.active_pointers.values() if c.element_id == "hold-0")
        engine.key_up("0")
        assert freed not in engine.active_pointers  # freed exactly with its contact
        engine.key_down("2")
        # The freed id is the only one that may be recycled.
        assert len(engine.active_pointers) == 2
        assert set(engine.active_pointers) <= {freed, *pointers}


class TestFocusLoss:
    def test_focus_loss_cancels_everything(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("d")
        engine.key_down("k")
        events = engine.focus_lost()
        assert all(event.action is Action.CANCEL for event in events)
        assert {event.pointer for event in events} == {1, 2}
        assert engine.active_pointers == {}

    def test_held_keys_are_forgotten_after_focus_loss(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("k")
        engine.focus_lost()
        # A late key-up for a key we no longer hold must not emit anything.
        assert engine.key_up("k") == []
        # And a fresh key-down must still work.
        assert actions(engine.key_down("k")) == [("down", 1)]

    def test_dpad_state_is_reset_after_focus_loss(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("w")
        engine.key_down("d")
        engine.focus_lost()
        events = engine.key_down("w")
        assert actions(events) == [("down", 1), ("move", 1)]
        move = events[-1]
        assert move.x == pytest.approx(200.0, abs=1e-6)  # straight up, no leftover "d"


class TestGeometryAndProfileChanges:
    def test_geometry_change_releases_contacts_first(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("d")
        released = engine.set_geometry(
            ViewportGeometry(buffer=Size(500, 1000), surface=Size(500, 1000))
        )
        assert [e.action for e in released] == [Action.CANCEL]
        assert engine.active_pointers == {}

    def test_geometry_change_moves_future_touches(self, target, geo, clock) -> None:
        element = Element(id="t", type=ElementType.TAP, label="T", x=0.5, y=0.5, keys=("t",))
        profile = Profile.new("Center", target, elements=(element,))
        engine = build(profile, geo, clock)
        assert engine.key_down("t")[0].x == pytest.approx(500.0)
        engine.set_geometry(ViewportGeometry(buffer=Size(2000, 400), surface=Size(500, 100)))
        assert engine.key_down("t")[0].x == pytest.approx(1000.0)

    def test_profile_switch_releases_and_rebuilds(self, profile, geo, clock, target) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("d")
        other = Profile.new(
            "Other",
            target,
            elements=(Element(id="x", type=ElementType.TAP, label="X", x=0.5, y=0.5, keys=("x",)),),
        )
        released = engine.set_profile(other)
        assert released and all(e.action is Action.CANCEL for e in released)
        assert engine.key_down("w") == []  # old binding gone
        assert actions(engine.key_down("x")) == [("down", 1), ("up", 1)]

    def test_rotation_does_not_move_profile_anchors(self, target, clock) -> None:
        element = Element(id="t", type=ElementType.TAP, label="T", x=0.25, y=0.75, keys=("t",))
        profile = Profile.new("Anchored", target, elements=(element,))
        upright = build(
            profile, ViewportGeometry(buffer=Size(1000, 500), surface=Size(1000, 500)), clock
        )
        rotated = build(
            profile,
            ViewportGeometry(
                buffer=Size(1000, 500), surface=Size(500, 1000), rotation=Rotation.CW90
            ),
            clock,
        )
        a = upright.key_down("t")[0]
        b = rotated.key_down("t")[0]
        assert (a.x, a.y) == (b.x, b.y)


class TestSwipe:
    def test_swipe_progresses_and_completes(self, target, geo, clock) -> None:
        element = Element(
            id="sw",
            type=ElementType.SWIPE,
            label="SW",
            x=0.2,
            y=0.5,
            keys=("z",),
            swipe_to=(0.8, 0.5),
            swipe_duration_ms=200,
        )
        profile = Profile.new("Swipe", target, elements=(element,))
        engine = build(profile, geo, clock)
        down = engine.key_down("z")
        assert actions(down) == [("down", 1)]
        clock.advance(100)
        mid = engine.tick()
        assert [e.action for e in mid] == [Action.MOVE]
        assert 200 < mid[0].x < 800
        clock.advance(150)
        done = engine.tick()
        assert [e.action for e in done] == [Action.MOVE, Action.UP]
        assert engine.active_pointers == {}

    def test_swipe_is_cancelled_by_focus_loss(self, target, geo, clock) -> None:
        element = Element(
            id="sw",
            type=ElementType.SWIPE,
            label="SW",
            x=0.2,
            y=0.5,
            keys=("z",),
            swipe_to=(0.8, 0.5),
            swipe_duration_ms=500,
        )
        profile = Profile.new("Swipe", target, elements=(element,))
        engine = build(profile, geo, clock)
        engine.key_down("z")
        engine.focus_lost()
        clock.advance(1000)
        assert engine.tick() == []

    def test_no_deadline_when_idle(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        assert engine.next_deadline_ms() is None


class TestStats:
    def test_stats_are_jsonable(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("k")
        stats = engine.stats()
        assert stats["active_pointers"] == 1
        assert stats["elements"] == len(profile.elements)

    def test_event_sequence_is_monotonic(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("d")
        engine.key_down("k")
        engine.key_up("d")
        events = engine.pending()
        assert [e.seq for e in events] == sorted(e.seq for e in events)
        assert len({e.seq for e in events}) == len(events)


def test_keys_of_collects_every_binding(profile) -> None:
    assert keys_of(profile) == {"j", "w", "d", "s", "a", "k"}


def test_unknown_element_type_in_engine_is_safe(profile, geo, clock) -> None:
    engine = build(profile, geo, clock)
    engine.analog_dpad("no-such-element", 0.5, 0.5)
    engine.mouse_delta(10, 10)
    assert engine.active_pointers == {}


class TestMouseAimMode:
    """The engine half of aim mode; the pointer capture itself needs Qt."""

    @pytest.fixture()
    def aim_profile(self, target):
        mouse = Element(
            id="aim",
            type=ElementType.MOUSE,
            label="Aim",
            x=0.5,
            y=0.5,
            keys=("k",),
            sensitivity=2.0,
            exit_key="Escape",
        )
        return Profile.new("Aim", target, elements=(mouse,))

    def test_pressing_the_key_starts_a_contact_and_reports_aim(
        self, aim_profile, geo, clock
    ) -> None:
        engine = build(aim_profile, geo, clock)
        assert engine.aim_active is False
        events = engine.key_down("k")
        # DOWN opens the contact; the MOVE that follows puts it where the aim
        # cursor rests. Android needs the DOWN first or it never sees a touch.
        assert actions(events) == [("down", 1), ("move", 1)]
        # The window asks this before it may capture the pointer.
        assert engine.aim_active is True

    def test_motion_moves_the_contact_that_is_already_down(self, aim_profile, geo, clock) -> None:
        engine = build(aim_profile, geo, clock)
        engine.key_down("k")
        # active_pointers hands out the live contact, so read the numbers now:
        # holding the object would make "before" change with "after".
        before = (engine.active_pointers[1].x, engine.active_pointers[1].y)
        assert engine.mouse_delta(10.0, 0.0)  # returns the MOVE it produced
        after = engine.active_pointers[1]
        # sensitivity 2.0: 10 px of mouse travel is 20 px of aim.
        assert after.x == pytest.approx(before[0] + 20.0)
        assert after.y == pytest.approx(before[1])

    def test_motion_before_the_key_does_nothing(self, aim_profile, geo, clock) -> None:
        engine = build(aim_profile, geo, clock)
        assert engine.mouse_delta(50.0, 50.0) == []
        assert engine.aim_active is False

    def test_the_exit_key_releases_the_contact_and_the_capture(
        self, aim_profile, geo, clock
    ) -> None:
        engine = build(aim_profile, geo, clock)
        engine.key_down("k")
        events = engine.key_down("Escape")
        assert actions(events) == [("up", 1)]
        assert engine.aim_active is False

    def test_losing_focus_ends_aim_mode(self, aim_profile, geo, clock) -> None:
        """A captured pointer must never outlive the window's focus."""
        engine = build(aim_profile, geo, clock)
        engine.key_down("k")
        engine.focus_lost("window lost focus")
        assert engine.aim_active is False
        assert engine.active_pointers == {}

    def test_switching_profile_ends_aim_mode(self, aim_profile, geo, clock, target) -> None:
        engine = build(aim_profile, geo, clock)
        engine.key_down("k")
        engine.set_profile(Profile.new("Other", target))
        assert engine.aim_active is False

    def test_capture_flag_is_remembered_not_assumed(self, aim_profile, geo, clock) -> None:
        engine = build(aim_profile, geo, clock)
        assert engine.realtime_capture is False
        engine.set_realtime_capture(True)
        assert engine.realtime_capture is True
        engine.set_realtime_capture(False)
        assert engine.realtime_capture is False
        assert engine.aim_active is False


class TestPressState:
    """Which elements report a held key, for the overlay highlight."""

    def test_a_hold_reports_itself_while_the_key_is_down(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        assert engine.press_state() == {}
        engine.key_down("k")  # the HOLD element in the shared fixture profile
        assert engine.press_state() == {"hold-fire": {"k"}}
        engine.key_up("k")
        assert engine.press_state() == {}

    def test_a_tap_is_not_reported_as_held(self, profile, geo, clock) -> None:
        """A tap is over the moment it is pressed; nothing stays held."""
        engine = build(profile, geo, clock)
        engine.key_down("j")
        assert engine.press_state() == {}
        engine.key_up("j")

    def test_every_reported_id_exists_in_the_profile(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        engine.key_down("k")
        ids = {element.id for element in profile.elements}
        assert set(engine.press_state()) <= ids


class TestOpposingMovement:
    @pytest.mark.parametrize("first,opposite", [("w", "s"), ("s", "w"), ("a", "d"), ("d", "a")])
    def test_opposites_return_to_centre_then_resume(
        self, first, opposite, profile, geo, clock
    ) -> None:
        engine = build(profile, geo, clock)
        down = engine.key_down(first)
        centre = (down[0].x, down[0].y)
        neutral = engine.key_down(opposite)
        assert len(neutral) == 1 and neutral[0].action is Action.MOVE
        assert (neutral[0].x, neutral[0].y) == pytest.approx(centre)
        resumed = engine.key_up(opposite)
        assert len(resumed) == 1 and resumed[0].action is Action.MOVE
        assert (resumed[0].x, resumed[0].y) != pytest.approx(centre)
        assert engine.key_up(first)[0].action is Action.UP
        assert not engine.active_pointers

    def test_opposites_cancel_one_axis_while_other_remains(self, profile, geo, clock) -> None:
        engine = build(profile, geo, clock)
        centre = engine.key_down("w")[0]
        engine.key_down("d")
        move = engine.key_down("s")[-1]
        assert move.x > centre.x
        assert move.y == pytest.approx(centre.y)
