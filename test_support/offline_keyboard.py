"""Load the upstream fake event tests without importing a real OS backend."""
import sys
import types
for name in ('_darwinkeyboard', '_winkeyboard', '_nixkeyboard'):
    sys.modules['keyboard.' + name] = types.ModuleType('keyboard.' + name)
from keyboard import _keyboard_tests as fake
import keyboard
