# -*- coding: utf-8 -*-
"""
Offline tests for the exception cleanup path of `keyboard.play`/`keyboard.replay`.

Uses the fake event facilities from `keyboard._keyboard_tests`, loaded through
`test_support.offline_keyboard` so no real OS backend is imported. Assertions
are made on the order of emitted events and on final state; no real hook or
sleep is involved.
"""
import unittest

from test_support.offline_keyboard import keyboard, fake
from keyboard import KEY_DOWN, KEY_UP

make_event = fake.make_event


def trace():
    """Emitted events so far, as (event_type, scan_code) pairs."""
    return [(e.event_type, e.scan_code) for e in fake.output_events]


class CountingIterator(object):
    """Yields the given events, then raises `error`. Counts `next` calls."""
    def __init__(self, events, error):
        self.remaining = list(events)
        self.error = error
        self.calls = 0
    def __iter__(self):
        return self
    def __next__(self):
        self.calls += 1
        if self.remaining:
            return self.remaining.pop(0)
        raise self.error
    next = __next__


class TestPlayCleanup(unittest.TestCase):
    def setUp(self):
        del fake.input_events[:]
        del fake.output_events[:]
        keyboard._recording = None
        keyboard._pressed_events.clear()
        keyboard._physically_pressed_keys.clear()
        keyboard._logically_pressed_keys.clear()
        keyboard._listener.init()
        keyboard._listener.start_if_necessary()
        self.addCleanup(keyboard.unhook_all)
        self.addCleanup(setattr, keyboard._listener, 'is_replaying', False)

    def pump(self, events):
        """Feeds physical (user) events through the listener."""
        for event in events:
            if keyboard._listener.direct_callback(event):
                fake.output_events.append(event)
        del fake.output_events[:]
        keyboard._listener.queue.join()

    def patch_os(self, name, replacement):
        original = getattr(keyboard._os_keyboard, name)
        setattr(keyboard._os_keyboard, name, replacement)
        self.addCleanup(setattr, keyboard._os_keyboard, name, original)
        return original

    def patch_sleep(self, replacement):
        original = keyboard._time.sleep
        keyboard._time.sleep = replacement
        self.addCleanup(setattr, keyboard._time, 'sleep', original)

    # Success paths.

    def test_normal_replay(self):
        keyboard.play(fake.d_a + fake.u_a, 0)
        self.assertEqual(trace(), [(KEY_DOWN, 1), (KEY_UP, 1)])
        self.assertFalse(keyboard._listener.is_replaying)

    def test_replay_alias(self):
        keyboard.replay(fake.du_b, 0)
        self.assertEqual(trace(), [(KEY_DOWN, 2), (KEY_UP, 2)])
        self.assertFalse(keyboard._listener.is_replaying)

    def test_normal_replay_stash_and_restore_modifiers(self):
        self.pump(fake.d_ctrl)
        keyboard.play(fake.d_a + fake.u_a, 0)
        self.assertEqual(trace(), [(KEY_UP, 7), (KEY_DOWN, 1), (KEY_UP, 1), (KEY_DOWN, 7)])
        self.assertFalse(keyboard._listener.is_replaying)

    def test_speed_factor_scales_sleeps(self):
        sleeps = []
        self.patch_sleep(sleeps.append)
        events = [
            make_event(KEY_DOWN, 'a', time=0),
            make_event(KEY_UP, 'a', time=1),
            make_event(KEY_DOWN, 'b', time=3),
            make_event(KEY_UP, 'b', time=4),
        ]
        keyboard.play(events, 2)
        self.assertEqual(sleeps, [0.5, 1.0, 0.5])
        self.assertEqual(trace(), [(KEY_DOWN, 1), (KEY_UP, 1), (KEY_DOWN, 2), (KEY_UP, 2)])

    def test_event_source_consumed_lazily(self):
        log = []
        events = fake.d_a + fake.u_a + fake.d_b
        def next_():
            log.append('next')
            if events:
                return events.pop(0)
            raise StopIteration
        class Source(object):
            def __iter__(self):
                return self
            def __next__(self):
                return next_()
            next = __next__
        real_press = keyboard._os_keyboard.press
        real_release = keyboard._os_keyboard.release
        self.patch_os('press', lambda code: (log.append(('press', code)), real_press(code)))
        self.patch_os('release', lambda code: (log.append(('release', code)), real_release(code)))
        keyboard.play(Source(), 0)
        self.assertEqual(log, [
            'next', ('press', 1),
            'next', ('release', 1),
            'next', ('press', 2),
            'next',
        ])

    # Iterator failure.

    def test_iterator_interruption_cleans_up(self):
        marker = RuntimeError('synthetic iterator stopped')
        self.pump(fake.d_ctrl)
        events = CountingIterator(fake.d_ctrl + fake.d_a + fake.d_b, marker)
        with self.assertRaises(RuntimeError) as cm:
            keyboard.play(events, 0)
        self.assertIs(cm.exception, marker)
        # Stopped consuming: one `next` per yielded event plus the failing one.
        self.assertEqual(events.calls, 4)
        # Held keys released in reverse press order, starting ctrl restored once.
        self.assertEqual(trace(), [
            (KEY_UP, 7),               # stash_state releases starting ctrl
            (KEY_DOWN, 7),             # replay presses ctrl
            (KEY_DOWN, 1),             # replay presses a
            (KEY_DOWN, 2),             # replay presses b
            (KEY_UP, 2),               # cleanup releases b
            (KEY_UP, 1),               # cleanup releases a
            (KEY_UP, 7),               # cleanup releases ctrl
            (KEY_DOWN, 7),             # starting modifier restored, exactly once
        ])
        self.assertEqual(trace().count((KEY_DOWN, 7)), 2)
        self.assertFalse(keyboard._listener.is_replaying)

    # Wait (interval sleep) failure.

    def test_wait_interruption_cleans_up(self):
        marker = KeyboardInterrupt('synthetic wait interrupted')
        def failing_sleep(duration):
            raise marker
        self.patch_sleep(failing_sleep)
        events = [
            make_event(KEY_DOWN, 'a', time=0),
            make_event(KEY_DOWN, 'b', time=2),
        ]
        with self.assertRaises(KeyboardInterrupt) as cm:
            keyboard.play(iter(events), 1)
        self.assertIs(cm.exception, marker)
        # `a` was pressed before the wait; `b` never happened.
        self.assertEqual(trace(), [(KEY_DOWN, 1), (KEY_UP, 1)])
        self.assertFalse(keyboard._listener.is_replaying)

    # Underlying send failure.

    def test_send_failure_cleans_up(self):
        marker = RuntimeError('synthetic backend stopped before effect')
        real_press = keyboard._os_keyboard.press
        def failing_press(scan_code):
            if scan_code == 2:
                raise marker
            real_press(scan_code)
        self.patch_os('press', failing_press)
        events = fake.d_a + fake.d_b + fake.u_b + fake.u_a
        with self.assertRaises(RuntimeError) as cm:
            keyboard.play(iter(events), 0)
        self.assertIs(cm.exception, marker)
        # The failed send produced no event and no cleanup for `b`.
        self.assertEqual(trace(), [(KEY_DOWN, 1), (KEY_UP, 1)])
        self.assertFalse(keyboard._listener.is_replaying)

    def test_send_failure_restores_starting_modifiers(self):
        marker = RuntimeError('synthetic backend stopped before effect')
        real_press = keyboard._os_keyboard.press
        def failing_press(scan_code):
            if scan_code == 1:
                raise marker
            real_press(scan_code)
        self.patch_os('press', failing_press)
        self.pump(fake.d_ctrl)
        with self.assertRaises(RuntimeError) as cm:
            keyboard.play(iter(fake.d_a), 0)
        self.assertIs(cm.exception, marker)
        self.assertEqual(trace(), [(KEY_UP, 7), (KEY_DOWN, 7)])
        self.assertFalse(keyboard._listener.is_replaying)

    # Tracking of successful presses.

    def test_repeated_down_released_once(self):
        marker = RuntimeError('stop')
        events = CountingIterator(fake.d_a + fake.d_a, marker)
        with self.assertRaises(RuntimeError):
            keyboard.play(events, 0)
        self.assertEqual(trace(), [(KEY_DOWN, 1), (KEY_DOWN, 1), (KEY_UP, 1)])

    def test_paired_down_up_not_released_again(self):
        marker = RuntimeError('stop')
        events = CountingIterator(fake.d_a + fake.u_a + fake.d_b, marker)
        with self.assertRaises(RuntimeError):
            keyboard.play(events, 0)
        self.assertEqual(trace(), [
            (KEY_DOWN, 1), (KEY_UP, 1),
            (KEY_DOWN, 2),
            (KEY_UP, 2),               # only `b` is still held
        ])

    def test_cleanup_releases_in_reverse_press_order(self):
        marker = RuntimeError('stop')
        events = CountingIterator(fake.d_a + fake.d_b + fake.d_c, marker)
        with self.assertRaises(RuntimeError):
            keyboard.play(events, 0)
        self.assertEqual(trace(), [
            (KEY_DOWN, 1), (KEY_DOWN, 2), (KEY_DOWN, 3),
            (KEY_UP, 3), (KEY_UP, 2), (KEY_UP, 1),
        ])

    # Name fallback for scan_code == 0.

    def test_scan_code_zero_resolved_by_name(self):
        down = fake.KeyboardEvent(event_type=KEY_DOWN, scan_code=0, name='a')
        up = fake.KeyboardEvent(event_type=KEY_UP, scan_code=0, name='a')
        keyboard.play([down, up], 0)
        self.assertEqual(trace(), [(KEY_DOWN, 1), (KEY_UP, 1)])

    def test_scan_code_zero_released_by_name_on_cleanup(self):
        marker = RuntimeError('stop')
        down = fake.KeyboardEvent(event_type=KEY_DOWN, scan_code=0, name='a')
        events = CountingIterator([down], marker)
        with self.assertRaises(RuntimeError):
            keyboard.play(events, 0)
        self.assertEqual(trace(), [(KEY_DOWN, 1), (KEY_UP, 1)])

    # is_replaying restoration.

    def test_nested_is_replaying_preserved(self):
        keyboard._listener.is_replaying = True
        try:
            keyboard.press('a')
            self.assertTrue(keyboard._listener.is_replaying)
            keyboard.release('a')
            self.assertTrue(keyboard._listener.is_replaying)
            keyboard.restore_state([1, 5])
            self.assertTrue(keyboard._listener.is_replaying)
            keyboard.play(fake.du_a, 0)
            self.assertTrue(keyboard._listener.is_replaying)
            marker = RuntimeError('stop')
            with self.assertRaises(RuntimeError):
                keyboard.play(CountingIterator(fake.d_a, marker), 0)
            self.assertTrue(keyboard._listener.is_replaying)
        finally:
            keyboard._listener.is_replaying = False

    def test_failed_send_restores_is_replaying(self):
        marker = RuntimeError('backend stopped')
        self.patch_os('press', lambda code: (_ for _ in ()).throw(marker))
        with self.assertRaises(RuntimeError) as cm:
            keyboard.press('a')
        self.assertIs(cm.exception, marker)
        self.assertFalse(keyboard._listener.is_replaying)

    def test_failed_restore_state_restores_is_replaying(self):
        marker = RuntimeError('backend stopped')
        self.patch_os('press', lambda code: (_ for _ in ()).throw(marker))
        with self.assertRaises(RuntimeError) as cm:
            keyboard.restore_state([1])
        self.assertIs(cm.exception, marker)
        self.assertFalse(keyboard._listener.is_replaying)


if __name__ == '__main__':
    unittest.main()
