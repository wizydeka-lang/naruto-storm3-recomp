package org.libsdl.app;

/**
 * An on-screen gamepad registered with SDL like a real Android controller.
 *
 * Lives in SDL's package because SDLControllerManager's joystick entry points
 * are package-private. SDL builds the gamepad mapping from the button and axis
 * masks: buttons are Android gamepad key codes, and axes 0-5 are left X/Y,
 * right X/Y, left trigger and right trigger.
 */
public final class VirtualGamepad {
    // Real devices get small positive ids from InputDevice; stay clear of them.
    private static final int DEVICE_ID = 0x7ffe0001;

    public static final int AXIS_LEFT_X = 0;
    public static final int AXIS_LEFT_Y = 1;
    public static final int AXIS_RIGHT_X = 2;
    public static final int AXIS_RIGHT_Y = 3;
    public static final int AXIS_LEFT_TRIGGER = 4;
    public static final int AXIS_RIGHT_TRIGGER = 5;

    private static boolean sAttached;

    private VirtualGamepad() {}

    /** Connects the gamepad; the game sees it like a controller plugged in. */
    public static synchronized void attach() {
        if (sAttached) {
            return;
        }
        // SDL_GAMEPAD_BUTTON_SOUTH through DPAD_RIGHT (bits 0-14), and the six
        // axes LEFTX through RIGHT_TRIGGER (bits 0-5).
        int buttonMask = 0x7fff;
        int axisMask = 0x003f;
        SDLControllerManager.nativeAddJoystick(DEVICE_ID, "Touch Controller", "naruto-touch-gamepad",
                0, 0, buttonMask, 6, axisMask, 0, false, false);
        sAttached = true;
        // Unmapped trigger axes read as half pressed; -1 is released.
        axis(AXIS_LEFT_TRIGGER, -1.0f);
        axis(AXIS_RIGHT_TRIGGER, -1.0f);
    }

    public static synchronized void detach() {
        if (!sAttached) {
            return;
        }
        SDLControllerManager.nativeRemoveJoystick(DEVICE_ID);
        sAttached = false;
    }

    /** keyCode is a KeyEvent.KEYCODE_BUTTON_* or KEYCODE_DPAD_* constant. */
    public static void button(int keyCode, boolean down) {
        if (!sAttached) {
            return;
        }
        if (down) {
            SDLControllerManager.onNativePadDown(DEVICE_ID, keyCode);
        } else {
            SDLControllerManager.onNativePadUp(DEVICE_ID, keyCode);
        }
    }

    /** Sticks take -1..1 (down and right positive); triggers -1 (released) to 1. */
    public static void axis(int axis, float value) {
        if (!sAttached) {
            return;
        }
        SDLControllerManager.onNativeJoy(DEVICE_ID, axis, value);
    }
}
