from test_support.offline_keyboard import keyboard, fake
import unittest
normal = unittest.TestSuite(fake.TestKeyboard(n) for n in ('test_play_nodelay', 'test_play_stash'))
assert unittest.TextTestRunner().run(normal).wasSuccessful()
case = fake.TestKeyboard(); case.setUp(); case.do(fake.d_ctrl)
marker = RuntimeError('synthetic iterator stopped')
def events():
    yield fake.d_a[0]
    raise marker
try:
    keyboard.play(events(), 0)
except RuntimeError as exc:
    assert exc is marker
print('iterator trace:', [(e.event_type, e.scan_code) for e in fake.output_events])
case.tearDown(); case.setUp()
def fail(code):
    raise RuntimeError('synthetic backend stopped before effect')
keyboard._os_keyboard.press = fail
try:
    keyboard.play(fake.d_a, 0)
except RuntimeError:
    pass
print('backend failure is_replaying:', keyboard._listener.is_replaying)
case.tearDown()
