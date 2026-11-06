package dev.narutorecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** The launcher's drawn pieces, in the desktop launcher's style. */
final class LauncherViews {
    static final int ORANGE = 0xffff7a1a;
    static final int ORANGE_DEEP = 0xffe0480e;
    static final int INK = 0xff17110e;
    static final int CARD = 0xe0151214;
    static final int MUTED = 0xffa79e98;

    private LauncherViews() {}

    /** A rounded rectangle; with a focus ring for controllers when focusable. */
    static StateListDrawable background(Context context, int color, int pressed, float radiusDp) {
        float density = context.getResources().getDisplayMetrics().density;
        StateListDrawable states = new StateListDrawable();
        GradientDrawable focused = rounded(color, radiusDp * density);
        focused.setStroke(Math.round(2 * density), ORANGE);
        states.addState(new int[] {android.R.attr.state_focused}, focused);
        states.addState(new int[] {android.R.attr.state_pressed}, rounded(pressed, radiusDp * density));
        states.addState(new int[] {}, rounded(color, radiusDp * density));
        return states;
    }

    static GradientDrawable rounded(int color, float radius) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(color);
        shape.setCornerRadius(radius);
        return shape;
    }

    /**
     * The game's key art, Naruto framed on the left behind the title, with a
     * shade toward the settings on the right; a drawn swirl until the game is
     * found.
     */
    static final class ArtBackground extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private Bitmap art;

        ArtBackground(Context context) {
            super(context);
        }

        void setArt(Bitmap art) {
            this.art = art;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            int w = getWidth(), h = getHeight();
            canvas.drawColor(INK);
            if (art != null) {
                // Cover the view, zoomed a little, with Naruto's face (27%
                // across, 20% down the art) a third of the way in.
                float scale = 1.12f * Math.max(w / (float) art.getWidth(), h / (float) art.getHeight());
                float drawnW = art.getWidth() * scale, drawnH = art.getHeight() * scale;
                float left = Math.min(0, Math.max(w - drawnW, w * 0.30f - drawnW * 0.47f));
                float top = Math.min(0, Math.max(h - drawnH, h * 0.25f - drawnH * 0.20f));
                canvas.drawBitmap(art, null, new RectF(left, top, left + drawnW, top + drawnH), paint);
            } else {
                paint.setShader(new RadialGradient(w * 0.28f, h * 0.45f, h * 0.9f,
                        0xff5a2a10, INK, Shader.TileMode.CLAMP));
                canvas.drawRect(0, 0, w, h, paint);
                paint.setShader(null);
                paint.setStyle(Paint.Style.STROKE);
                paint.setColor(ORANGE);
                paint.setAlpha(60);
                paint.setStrokeWidth(h * 0.012f);
                for (int i = 1; i <= 4; i++) {
                    canvas.drawCircle(w * 0.28f, h * 0.45f, h * 0.12f * i, paint);
                }
                paint.setStyle(Paint.Style.FILL);
            }
            // Darken the bottom left (the title) and the right (the card).
            paint.setShader(new LinearGradient(0, 0, w, 0,
                    new int[] {0x99000000, 0x22000000, 0xcc0b0809}, new float[] {0, 0.45f, 0.85f},
                    Shader.TileMode.CLAMP));
            canvas.drawRect(0, 0, w, h, paint);
            paint.setShader(new LinearGradient(0, h * 0.4f, 0, h,
                    0x00000000, 0xdd0b0809, Shader.TileMode.CLAMP));
            canvas.drawRect(0, 0, w, h, paint);
            paint.setShader(null);
            // An orange slash under the title, as on the desktop launcher.
            Path slash = new Path();
            slash.moveTo(0, h * 0.93f);
            slash.lineTo(w * 0.42f, h * 0.86f);
            slash.lineTo(w * 0.40f, h * 0.885f);
            slash.lineTo(0, h * 0.965f);
            slash.close();
            paint.setColor(ORANGE_DEEP);
            paint.setAlpha(220);
            canvas.drawPath(slash, paint);
            paint.setAlpha(255);
        }
    }

    /** The slanted orange PLAY button with a swirl. */
    static final class PlayButton extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float density;

        PlayButton(Context context) {
            super(context);
            density = context.getResources().getDisplayMetrics().density;
            setFocusable(true);
            setClickable(true);
            setContentDescription("Play");
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            setMeasuredDimension(Math.round(230 * density), Math.round(64 * density));
        }

        @Override
        protected void drawableStateChanged() {
            super.drawableStateChanged();
            invalidate();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float w = getWidth(), h = getHeight(), slant = h * 0.32f;
            Path shape = new Path();
            shape.moveTo(slant, 0);
            shape.lineTo(w, 0);
            shape.lineTo(w - slant, h);
            shape.lineTo(0, h);
            shape.close();
            boolean lit = isPressed() || isFocused();
            paint.setStyle(Paint.Style.FILL);
            if (isEnabled()) {
                paint.setShader(new LinearGradient(0, 0, w, h,
                        lit ? 0xffffc070 : 0xffffb347, lit ? 0xffff5a1a : ORANGE_DEEP,
                        Shader.TileMode.CLAMP));
            } else {
                paint.setShader(null);
                paint.setColor(0xff4a4440);
            }
            canvas.drawPath(shape, paint);
            paint.setShader(null);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth((lit ? 3 : 2) * density);
            paint.setColor(lit ? Color.WHITE : 0xff5fb4ff);
            canvas.drawPath(shape, paint);
            // The swirl.
            float cx = slant + h * 0.42f, cy = h / 2, r = h * 0.28f;
            paint.setColor(INK);
            paint.setStrokeWidth(h * 0.07f);
            paint.setStrokeCap(Paint.Cap.ROUND);
            Path swirl = new Path();
            for (int i = 0; i <= 60; i++) {
                double t = i / 60.0 * Math.PI * 4.2;
                float radius = (float) (r * i / 60.0);
                float x = cx + radius * (float) Math.cos(t), y = cy + radius * (float) Math.sin(t);
                if (i == 0) swirl.moveTo(x, y); else swirl.lineTo(x, y);
            }
            canvas.drawPath(swirl, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(isEnabled() ? INK : 0xff8a827c);
            paint.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
            paint.setTextSize(h * 0.48f);
            paint.setLetterSpacing(0.25f);
            paint.setTextAlign(Paint.Align.CENTER);
            float textX = (cx + r + w - slant) / 2 + h * 0.1f;
            canvas.drawText("PLAY", textX, cy - (paint.ascent() + paint.descent()) / 2, paint);
        }
    }

    /** A row of exclusive choices. */
    static final class Segmented extends LinearLayout {
        interface Listener {
            void onChanged(String key);
        }

        private final List<TextView> options = new ArrayList<>();
        private final List<String> keys = new ArrayList<>();

        Segmented(Context context, String[][] choices, String value, Listener listener) {
            super(context);
            setOrientation(HORIZONTAL);
            float density = context.getResources().getDisplayMetrics().density;
            for (String[] choice : choices) {
                TextView option = new TextView(context);
                option.setText(choice[1]);
                option.setTextSize(15);
                option.setTypeface(Typeface.DEFAULT_BOLD);
                option.setGravity(Gravity.CENTER);
                option.setPadding(Math.round(18 * density), Math.round(9 * density),
                        Math.round(18 * density), Math.round(9 * density));
                option.setFocusable(true);
                option.setClickable(true);
                String key = choice[0];
                option.setOnClickListener(v -> {
                    select(key);
                    listener.onChanged(key);
                });
                LayoutParams params = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
                params.rightMargin = Math.round(8 * density);
                addView(option, params);
                options.add(option);
                keys.add(key);
            }
            select(value);
        }

        void select(String value) {
            for (int i = 0; i < options.size(); i++) {
                boolean on = keys.get(i).equals(value);
                TextView option = options.get(i);
                option.setBackground(background(getContext(), on ? ORANGE : 0xff2a2426,
                        on ? 0xffff9a4a : 0xff3a3234, 10));
                option.setTextColor(on ? INK : 0xfff2ebe6);
            }
        }
    }
}
