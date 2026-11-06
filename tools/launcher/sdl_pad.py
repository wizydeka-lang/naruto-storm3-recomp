"""Game controllers for the launcher, read through the system's SDL 3.

Only SDL's gamepad subsystem is used (no window), so it runs beside Qt. Every
function is safe to call when SDL 3 is missing: there are just no pads.
"""

import ctypes
import ctypes.util

# SDL_GamepadButton, in the positions SDL reports every controller by.
SOUTH, EAST, WEST, NORTH, BACK, GUIDE, START, LEFT_STICK, RIGHT_STICK, \
    LEFT_SHOULDER, RIGHT_SHOULDER, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT = range(15)
LEFT_TRIGGER_AXIS, RIGHT_TRIGGER_AXIS = 4, 5
LEFTY_AXIS = 1

# The game's names for buttons (src/input_remap.h), by SDL button, and the
# triggers by axis.
GAME_NAMES = {SOUTH: "A", EAST: "B", WEST: "X", NORTH: "Y", BACK: "Back", START: "Start",
              LEFT_STICK: "LS", RIGHT_STICK: "RS", LEFT_SHOULDER: "LB",
              RIGHT_SHOULDER: "RB", DPAD_UP: "Up", DPAD_DOWN: "Down", DPAD_LEFT: "Left",
              DPAD_RIGHT: "Right"}
TRIGGER_NAMES = {LEFT_TRIGGER_AXIS: "LT", RIGHT_TRIGGER_AXIS: "RT"}

_SDL_INIT_GAMEPAD = 0x2000


def kind_of_vendor(vendor):
    """The game's controller kind for a USB vendor (as input_remap.cpp)."""
    return {0x045E: "xbox", 0x054C: "playstation", 0x057E: "nintendo"}.get(vendor, "other")


class Pads:
    def __init__(self):
        self._sdl = None
        self._open = {}  # SDL_JoystickID: SDL_Gamepad*
        name = ctypes.util.find_library("SDL3") or "libSDL3.so.0"
        try:
            sdl = ctypes.CDLL(name)
        except OSError:
            return
        sdl.SDL_SetHint.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        sdl.SDL_Init.argtypes = [ctypes.c_uint32]
        sdl.SDL_Init.restype = ctypes.c_bool
        sdl.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
        sdl.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_uint32)
        sdl.SDL_free.argtypes = [ctypes.c_void_p]
        sdl.SDL_OpenGamepad.argtypes = [ctypes.c_uint32]
        sdl.SDL_OpenGamepad.restype = ctypes.c_void_p
        sdl.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
        sdl.SDL_GetGamepadName.argtypes = [ctypes.c_void_p]
        sdl.SDL_GetGamepadName.restype = ctypes.c_char_p
        sdl.SDL_GetGamepadVendor.argtypes = [ctypes.c_void_p]
        sdl.SDL_GetGamepadVendor.restype = ctypes.c_uint16
        sdl.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
        sdl.SDL_GetGamepadButton.restype = ctypes.c_bool
        sdl.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
        sdl.SDL_GetGamepadAxis.restype = ctypes.c_int16
        # No SDL window ever has focus here, and Qt handles the signals.
        sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
        sdl.SDL_SetHint(b"SDL_NO_SIGNAL_HANDLERS", b"1")
        if sdl.SDL_Init(_SDL_INIT_GAMEPAD):
            self._sdl = sdl

    @property
    def available(self):
        return self._sdl is not None

    def poll(self):
        """Updates the pads; returns [(name, kind, buttons)] for each, where
        buttons is the set of the game's names for those held."""
        if not self._sdl:
            return []
        sdl = self._sdl
        sdl.SDL_PumpEvents()
        count = ctypes.c_int(0)
        ids = sdl.SDL_GetGamepads(ctypes.byref(count))
        present = [ids[i] for i in range(count.value)] if ids else []
        if ids:
            sdl.SDL_free(ids)
        for gone in set(self._open) - set(present):
            sdl.SDL_CloseGamepad(self._open.pop(gone))
        for new in set(present) - set(self._open):
            pad = sdl.SDL_OpenGamepad(new)
            if pad:
                self._open[new] = pad
        sdl.SDL_UpdateGamepads()
        pads = []
        for pad in self._open.values():
            held = {name for button, name in GAME_NAMES.items()
                    if sdl.SDL_GetGamepadButton(pad, button)}
            held |= {name for axis, name in TRIGGER_NAMES.items()
                     if sdl.SDL_GetGamepadAxis(pad, axis) > 16000}
            stick = sdl.SDL_GetGamepadAxis(pad, LEFTY_AXIS)
            if stick < -20000:
                held.add("StickUp")
            elif stick > 20000:
                held.add("StickDown")
            name = (sdl.SDL_GetGamepadName(pad) or b"Controller").decode(errors="replace")
            pads.append((name, kind_of_vendor(sdl.SDL_GetGamepadVendor(pad)), held))
        return pads
