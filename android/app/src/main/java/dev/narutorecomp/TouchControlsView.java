package dev.narutorecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Shader;
import android.util.DisplayMetrics;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import org.libsdl.app.VirtualGamepad;

/**
 * An Xbox 360 controller drawn over the game, for landscape play.
 *
 * Layout: left stick bottom left with the D-pad beside it, A/B/X/Y bottom
 * right with the right stick beside them, bumpers and triggers along the top
 * corners, Back/Guide/Start top center, and a small toggle bottom center that
 * hides everything but itself (for a Bluetooth controller).
 *
 * Every touch event recomputes which buttons are held from all fingers not
 * driving a stick, so a finger can slide from one button to the next.
 *
 * Its size and opacity are the launcher's settings. As a preview (in the
 * launcher) it takes no input and draws the full-screen layout shrunk to fit.
 */
public class TouchControlsView extends View {
    private static final int ALPHA_IDLE = 90;
    private static final int ALPHA_PRESSED = 200;

    private static final class Button {
        final int keyCode;
        final String label;
        final int color;
        final boolean round;
        final RectF bounds = new RectF();

        Button(int keyCode, String label, int color, boolean round) {
            this.keyCode = keyCode;
            this.label = label;
            this.color = color;
            this.round = round;
        }
    }

    private static final class Stick {
        final int axisX;
        final int axisY;
        final String label;
        float centerX, centerY, radius;
        int pointerId = -1;
        float knobX, knobY;

        Stick(int axisX, int axisY, String label) {
            this.axisX = axisX;
            this.axisY = axisY;
            this.label = label;
        }
    }

    // Triggers are buttons on screen but axes to the game.
    private static final int TRIGGER_LEFT = -1;
    private static final int TRIGGER_RIGHT = -2;
    private static final int TOGGLE = -3;

    // The Xbox 360 controller's face button colors.
    private static final int GREEN = 0xff3fae2a;
    private static final int RED = 0xffe2231a;
    private static final int BLUE = 0xff1f6fd1;
    private static final int YELLOW = 0xfff5b50c;
    private static final int SILVER = 0xffd9dde2;

    private final float density;
    private final boolean preview;
    private float size = 1.0f;
    private float opacity = 0.7f;
    // The preview's drawing scale from the full-screen layout.
    private float previewScale = 1.0f;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final List<Button> buttons = new ArrayList<>();
    private final Stick leftStick = new Stick(VirtualGamepad.AXIS_LEFT_X, VirtualGamepad.AXIS_LEFT_Y, "L");
    private final Stick rightStick = new Stick(VirtualGamepad.AXIS_RIGHT_X, VirtualGamepad.AXIS_RIGHT_Y, "R");
    private final RectF dpad = new RectF();
    private final Button toggle = new Button(TOGGLE, "◐", Color.WHITE, true);

    private final Set<Integer> held = new HashSet<>();
    private boolean hidden;
    private boolean attached;

    public TouchControlsView(Context context) {
        this(context, false);
    }

    public TouchControlsView(Context context, boolean preview) {
        super(context);
        this.preview = preview;
        density = context.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(2 * density);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);

        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_A, "A", GREEN, true));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_B, "B", RED, true));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_X, "X", BLUE, true));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_Y, "Y", YELLOW, true));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_L1, "LB", SILVER, false));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_R1, "RB", SILVER, false));
        buttons.add(new Button(TRIGGER_LEFT, "LT", SILVER, false));
        buttons.add(new Button(TRIGGER_RIGHT, "RT", SILVER, false));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_SELECT, "◀ BACK", SILVER, false));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_MODE, "X", GREEN, true));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_START, "START ▶", SILVER, false));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_THUMBL, "L3", SILVER, true));
        buttons.add(new Button(KeyEvent.KEYCODE_BUTTON_THUMBR, "R3", SILVER, true));
    }

    /** Size, 0.6 to 1.4 times the default, and opacity, 0.2 to 1. */
    public void setLook(float size, float opacity) {
        this.size = size;
        this.opacity = opacity;
        if (getWidth() > 0) {
            onSizeChanged(getWidth(), getHeight(), getWidth(), getHeight());
        }
        invalidate();
    }

    // Layout sizes grow with the size setting; line widths don't.
    private float dp(float value) {
        return value * density * size;
    }

    // An alpha for the opacity setting; the default, 0.7, draws them as given.
    private int alpha(int value) {
        return Math.min(255, Math.round(value * opacity / 0.7f));
    }

    private Button find(int keyCode) {
        for (Button b : buttons) {
            if (b.keyCode == keyCode) {
                return b;
            }
        }
        throw new IllegalArgumentException("no button " + keyCode);
    }

    private static void circle(RectF r, float x, float y, float radius) {
        r.set(x - radius, y - radius, x + radius, y + radius);
    }

    private static void box(RectF r, float x, float y, float halfW, float halfH) {
        r.set(x - halfW, y - halfH, x + halfW, y + halfH);
    }

    @Override
    protected void onSizeChanged(int viewW, int viewH, int oldW, int oldH) {
        int w = viewW, h = viewH;
        if (preview) {
            // Laid out for the whole (landscape) screen, drawn shrunk.
            DisplayMetrics metrics = getResources().getDisplayMetrics();
            w = Math.max(metrics.widthPixels, metrics.heightPixels);
            h = Math.min(metrics.widthPixels, metrics.heightPixels);
            previewScale = Math.min(viewW / (float) w, viewH / (float) h);
        }
        float face = dp(30);
        float faceGap = dp(58);
        float faceX = w - dp(110);
        float faceY = h - dp(120);
        circle(find(KeyEvent.KEYCODE_BUTTON_A).bounds, faceX, faceY + faceGap, face);
        circle(find(KeyEvent.KEYCODE_BUTTON_B).bounds, faceX + faceGap, faceY, face);
        circle(find(KeyEvent.KEYCODE_BUTTON_X).bounds, faceX - faceGap, faceY, face);
        circle(find(KeyEvent.KEYCODE_BUTTON_Y).bounds, faceX, faceY - faceGap, face);

        leftStick.radius = dp(68);
        leftStick.centerX = dp(130);
        leftStick.centerY = h - dp(115);
        rightStick.radius = dp(52);
        rightStick.centerX = faceX - dp(190);
        rightStick.centerY = h - dp(75);
        resetKnob(leftStick);
        resetKnob(rightStick);

        float dpadHalf = dp(62);
        float dpadX = leftStick.centerX + dp(200);
        float dpadY = h - dp(80);
        dpad.set(dpadX - dpadHalf, dpadY - dpadHalf, dpadX + dpadHalf, dpadY + dpadHalf);

        float shoulderW = dp(52);
        float shoulderH = dp(24);
        box(find(TRIGGER_LEFT).bounds, dp(70), dp(36), shoulderW, shoulderH);
        box(find(KeyEvent.KEYCODE_BUTTON_L1).bounds, dp(70), dp(96), shoulderW, shoulderH);
        box(find(TRIGGER_RIGHT).bounds, w - dp(70), dp(36), shoulderW, shoulderH);
        box(find(KeyEvent.KEYCODE_BUTTON_R1).bounds, w - dp(70), dp(96), shoulderW, shoulderH);

        box(find(KeyEvent.KEYCODE_BUTTON_SELECT).bounds, w / 2f - dp(92), dp(32), dp(44), dp(18));
        circle(find(KeyEvent.KEYCODE_BUTTON_MODE).bounds, w / 2f, dp(32), dp(22));
        box(find(KeyEvent.KEYCODE_BUTTON_START).bounds, w / 2f + dp(92), dp(32), dp(44), dp(18));

        circle(find(KeyEvent.KEYCODE_BUTTON_THUMBL).bounds,
                leftStick.centerX + leftStick.radius + dp(10), leftStick.centerY - leftStick.radius, dp(20));
        circle(find(KeyEvent.KEYCODE_BUTTON_THUMBR).bounds,
                rightStick.centerX - rightStick.radius - dp(6), rightStick.centerY - rightStick.radius, dp(20));

        circle(toggle.bounds, w / 2f, h - 26 * density, 18 * density);
    }

    private static void resetKnob(Stick s) {
        s.knobX = s.centerX;
        s.knobY = s.centerY;
    }

    // --- Input -------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (preview) {
            return false;
        }
        if (!attached) {
            VirtualGamepad.attach();
            attached = true;
        }
        int action = event.getActionMasked();
        int actionIndex = event.getActionIndex();
        int actionId = event.getPointerId(actionIndex);

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = event.getX(actionIndex);
            float y = event.getY(actionIndex);
            if (toggle.bounds.contains(x, y)) {
                setHidden(!hidden);
                return true;
            }
            if (!hidden) {
                // A finger landing near a free stick drives it until lifted.
                for (Stick s : new Stick[] {leftStick, rightStick}) {
                    if (s.pointerId == -1 && Math.hypot(x - s.centerX, y - s.centerY) < s.radius * 1.4f) {
                        s.pointerId = actionId;
                        break;
                    }
                }
            }
        }

        boolean lifting = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP
                || action == MotionEvent.ACTION_CANCEL;
        Set<Integer> nowHeld = new HashSet<>();
        for (int i = 0; i < event.getPointerCount(); i++) {
            int id = event.getPointerId(i);
            if (hidden || (lifting && (i == actionIndex || action == MotionEvent.ACTION_CANCEL))) {
                continue;
            }
            float x = event.getX(i);
            float y = event.getY(i);
            Stick s = id == leftStick.pointerId ? leftStick : id == rightStick.pointerId ? rightStick : null;
            if (s != null) {
                moveStick(s, x, y);
            } else {
                collectHeld(x, y, nowHeld);
            }
        }
        if (lifting) {
            for (Stick s : new Stick[] {leftStick, rightStick}) {
                if (s.pointerId != -1 && (action == MotionEvent.ACTION_CANCEL || s.pointerId == actionId)) {
                    s.pointerId = -1;
                    resetKnob(s);
                    VirtualGamepad.axis(s.axisX, 0);
                    VirtualGamepad.axis(s.axisY, 0);
                }
            }
        }
        applyHeld(nowHeld);
        invalidate();
        return true;
    }

    private void moveStick(Stick s, float x, float y) {
        float dx = x - s.centerX;
        float dy = y - s.centerY;
        float length = (float) Math.hypot(dx, dy);
        if (length > s.radius) {
            dx *= s.radius / length;
            dy *= s.radius / length;
        }
        s.knobX = s.centerX + dx;
        s.knobY = s.centerY + dy;
        VirtualGamepad.axis(s.axisX, dx / s.radius);
        VirtualGamepad.axis(s.axisY, dy / s.radius);
    }

    private void collectHeld(float x, float y, Set<Integer> out) {
        for (Button b : buttons) {
            RectF r = b.bounds;
            // A little slack around each button makes them easier to hit.
            float slack = dp(8);
            if (x >= r.left - slack && x <= r.right + slack && y >= r.top - slack && y <= r.bottom + slack) {
                out.add(b.keyCode);
            }
        }
        if (dpad.contains(x, y)) {
            // Thirds of the pad: the outer ones press a direction, corners
            // press two.
            float third = dpad.width() / 3;
            if (y < dpad.top + third) out.add(KeyEvent.KEYCODE_DPAD_UP);
            if (y > dpad.bottom - third) out.add(KeyEvent.KEYCODE_DPAD_DOWN);
            if (x < dpad.left + third) out.add(KeyEvent.KEYCODE_DPAD_LEFT);
            if (x > dpad.right - third) out.add(KeyEvent.KEYCODE_DPAD_RIGHT);
        }
    }

    private void applyHeld(Set<Integer> nowHeld) {
        boolean pressedAny = false;
        for (int key : nowHeld) {
            if (!held.contains(key)) {
                send(key, true);
                pressedAny = true;
            }
        }
        for (int key : held) {
            if (!nowHeld.contains(key)) {
                send(key, false);
            }
        }
        held.clear();
        held.addAll(nowHeld);
        if (pressedAny) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
    }

    private static void send(int key, boolean down) {
        if (key == TRIGGER_LEFT) {
            VirtualGamepad.axis(VirtualGamepad.AXIS_LEFT_TRIGGER, down ? 1 : -1);
        } else if (key == TRIGGER_RIGHT) {
            VirtualGamepad.axis(VirtualGamepad.AXIS_RIGHT_TRIGGER, down ? 1 : -1);
        } else {
            VirtualGamepad.button(key, down);
        }
    }

    private void setHidden(boolean hide) {
        hidden = hide;
        applyHeld(new HashSet<>());
        for (Stick s : new Stick[] {leftStick, rightStick}) {
            s.pointerId = -1;
            resetKnob(s);
            VirtualGamepad.axis(s.axisX, 0);
            VirtualGamepad.axis(s.axisY, 0);
        }
        invalidate();
    }

    // --- Drawing -----------------------------------------------------------

    @Override
    protected void onDraw(Canvas canvas) {
        if (preview) {
            canvas.save();
            canvas.scale(previewScale, previewScale);
        }
        if (!preview) {
            drawButton(canvas, toggle, false);
        }
        if (!hidden) {
            for (Stick s : new Stick[] {leftStick, rightStick}) {
                drawStick(canvas, s);
            }
            drawDpad(canvas);
            for (Button b : buttons) {
                drawButton(canvas, b, held.contains(b.keyCode));
            }
        }
        if (preview) {
            canvas.restore();
        }
    }

    // A 360 thumbstick: a dark well with a concave grey cap.
    private void drawStick(Canvas canvas, Stick s) {
        boolean active = s.pointerId != -1;
        fill.setShader(null);
        fill.setColor(Color.BLACK);
        fill.setAlpha(alpha(active ? 90 : 60));
        canvas.drawCircle(s.centerX, s.centerY, s.radius, fill);
        stroke.setColor(SILVER);
        stroke.setAlpha(alpha(ALPHA_IDLE));
        canvas.drawCircle(s.centerX, s.centerY, s.radius, stroke);
        float cap = s.radius * 0.48f;
        fill.setShader(new LinearGradient(s.knobX, s.knobY - cap, s.knobX, s.knobY + cap,
                0xff8a9099, 0xff3a3e44, Shader.TileMode.CLAMP));
        fill.setAlpha(alpha(active ? ALPHA_PRESSED : ALPHA_IDLE + 40));
        canvas.drawCircle(s.knobX, s.knobY, cap, fill);
        fill.setShader(null);
        stroke.setAlpha(alpha(ALPHA_PRESSED));
        canvas.drawCircle(s.knobX, s.knobY, cap * 0.72f, stroke);
        drawLabel(canvas, s.label, s.knobX, s.knobY, cap * 0.7f, Color.WHITE, alpha(200));
    }

    private void drawDpad(Canvas canvas) {
        float third = dpad.width() / 3;
        RectF arm = new RectF();
        Map<Integer, RectF> arms = new HashMap<>();
        arms.put(KeyEvent.KEYCODE_DPAD_UP,
                new RectF(dpad.left + third, dpad.top, dpad.right - third, dpad.top + third));
        arms.put(KeyEvent.KEYCODE_DPAD_DOWN,
                new RectF(dpad.left + third, dpad.bottom - third, dpad.right - third, dpad.bottom));
        arms.put(KeyEvent.KEYCODE_DPAD_LEFT,
                new RectF(dpad.left, dpad.top + third, dpad.left + third, dpad.bottom - third));
        arms.put(KeyEvent.KEYCODE_DPAD_RIGHT,
                new RectF(dpad.right - third, dpad.top + third, dpad.right, dpad.bottom - third));
        // The 360's D-pad sits on a round disc.
        fill.setColor(Color.BLACK);
        fill.setAlpha(alpha(60));
        canvas.drawCircle(dpad.centerX(), dpad.centerY(), dpad.width() * 0.56f, fill);
        stroke.setColor(SILVER);
        stroke.setAlpha(alpha(ALPHA_IDLE));
        canvas.drawCircle(dpad.centerX(), dpad.centerY(), dpad.width() * 0.56f, stroke);
        fill.setColor(SILVER);
        fill.setAlpha(alpha(ALPHA_IDLE - 30));
        arm.set(dpad.left + third, dpad.top + third, dpad.right - third, dpad.bottom - third);
        canvas.drawRect(arm, fill);
        for (Map.Entry<Integer, RectF> e : arms.entrySet()) {
            fill.setAlpha(alpha(held.contains(e.getKey()) ? ALPHA_PRESSED : ALPHA_IDLE));
            canvas.drawRoundRect(e.getValue(), dp(6), dp(6), fill);
        }
    }

    private void drawButton(Canvas canvas, Button b, boolean pressed) {
        RectF r = b.bounds;
        boolean face = b.color != SILVER && b.keyCode != TOGGLE && b.keyCode != KeyEvent.KEYCODE_BUTTON_MODE;
        boolean guide = b.keyCode == KeyEvent.KEYCODE_BUTTON_MODE;
        int a = b.keyCode == TOGGLE ? ALPHA_IDLE : alpha(pressed ? ALPHA_PRESSED : ALPHA_IDLE);
        // Face buttons are dark with a colored letter, as on the 360; the
        // rest are silver.
        fill.setColor(face ? 0xff15171a : guide ? SILVER : b.color == Color.WHITE ? Color.WHITE : 0xff2a2d31);
        fill.setAlpha(pressed && face ? a : face ? alpha(150) : a);
        if (pressed && face) {
            fill.setColor(b.color);
            fill.setAlpha(alpha(ALPHA_PRESSED));
        }
        stroke.setColor(b.color);
        stroke.setAlpha(b.keyCode == TOGGLE ? ALPHA_PRESSED : alpha(ALPHA_PRESSED));
        if (b.round) {
            canvas.drawOval(r, fill);
            canvas.drawOval(r, stroke);
        } else {
            canvas.drawRoundRect(r, dp(10), dp(10), fill);
            canvas.drawRoundRect(r, dp(10), dp(10), stroke);
        }
        if (guide) {
            // The guide button's green ring of light.
            stroke.setColor(GREEN);
            stroke.setAlpha(alpha(230));
            canvas.drawCircle(r.centerX(), r.centerY(), r.width() * 0.42f, stroke);
        }
        float textSize = Math.min(r.height() * 0.55f, r.width() / Math.max(2, b.label.length()) * 1.5f);
        int textColor = face && !pressed ? b.color : guide ? GREEN : Color.WHITE;
        drawLabel(canvas, b.label, r.centerX(), r.centerY(), textSize, textColor,
                b.keyCode == TOGGLE ? 230 : alpha(240));
    }

    private void drawLabel(Canvas canvas, String label, float x, float y, float size, int color, int alpha) {
        text.setColor(color);
        text.setAlpha(alpha);
        text.setTextSize(size);
        canvas.drawText(label, x, y - (text.ascent() + text.descent()) / 2, text);
    }
}
