package com.snesonline;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Bitmap;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashMap;

import com.caverock.androidsvg.SVG;

public final class OnscreenControlsView extends View {
    private static final float DEADZONE = 0.18f;

    // Semi-transparent in-game controls (backgrounds only).
    private static final int CONTROL_BG_ALPHA = 150;
    // Reduce transparency by ~50% (make the SVG more opaque).
    private static final int DPAD_SVG_ALPHA = 200;

    private final Paint bgPaint = new Paint();
    private final Paint fgPaint = new Paint();
    private final Paint textPaint = new Paint();
    private final Paint abxyTextPaint = new Paint();

    private Drawable dpadDrawable;
    private Drawable roundButtonDrawable;
    private Drawable rectButtonDrawable;
    private Drawable pillButtonDrawable;

    private Bitmap dpadSvgBitmap;
    private final Paint dpadSvgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final RectF dpadRect = new RectF();
    private final RectF aRect = new RectF();
    private final RectF bRect = new RectF();
    private final RectF xRect = new RectF();
    private final RectF yRect = new RectF();
    private final RectF startRect = new RectF();
    private final RectF selectRect = new RectF();
    private final RectF stateRect = new RectF();
    private final RectF lRect = new RectF();
    private final RectF rRect = new RectF();

    private float dpadCx = 0f;
    private float dpadCy = 0f;
    private float dpadRadius = 0f;

    private final HashMap<Integer, TouchBinding> pointerBindings = new HashMap<>();

    private enum BindingType {
        DPAD,
        A,
        B,
        X,
        Y,
        STATE,
        START,
        SELECT,
        L,
        R
    }

    private boolean showStateButton = false;
    private Runnable onStateRequested = null;

    public void setShowSaveButton(boolean show) {
        // Backwards compatible entrypoint (older code called this).
        showStateButton = show;
        invalidate();
    }

    public void setShowStateButton(boolean show) {
        showStateButton = show;
        invalidate();
    }

    public void setOnStateRequested(Runnable r) {
        onStateRequested = r;
    }

    private static final class TouchBinding {
        BindingType type;
        // For dpad we track which directions were applied for this pointer.
        boolean u, d, l, r;

        TouchBinding(BindingType type) {
            this.type = type;
        }
    }

    public OnscreenControlsView(Context context) {
        super(context);

        setFocusable(false);
        setFocusableInTouchMode(false);

        bgPaint.setColor(Color.argb(110, 0, 0, 0));
        bgPaint.setStyle(Paint.Style.FILL);
        bgPaint.setAntiAlias(true);

        fgPaint.setColor(Color.argb(140, 255, 255, 255));
        fgPaint.setStyle(Paint.Style.STROKE);
        fgPaint.setStrokeWidth(3f);
        fgPaint.setAntiAlias(true);

        textPaint.setColor(Color.argb(200, 255, 255, 255));
        textPaint.setTextSize(28f);
        textPaint.setAntiAlias(true);
        textPaint.setTextAlign(Paint.Align.CENTER);

        abxyTextPaint.set(textPaint);
        abxyTextPaint.setTextSize(textPaint.getTextSize() * 1.2f);

        setWillNotDraw(false);

        // Image assets (drawables). If any are missing, we fall back to paint-based rendering.
        try { dpadDrawable = getResources().getDrawable(R.drawable.bg_dpad); } catch (Exception ignored) { dpadDrawable = null; }
        try { roundButtonDrawable = getResources().getDrawable(R.drawable.bg_btn_round); } catch (Exception ignored) { roundButtonDrawable = null; }
        try { rectButtonDrawable = getResources().getDrawable(R.drawable.bg_btn_rect); } catch (Exception ignored) { rectButtonDrawable = null; }
        try { pillButtonDrawable = getResources().getDrawable(R.drawable.bg_btn_pill); } catch (Exception ignored) { pillButtonDrawable = null; }

        // Make in-game controls semi-transparent (backgrounds only).
        if (dpadDrawable != null) dpadDrawable.setAlpha(CONTROL_BG_ALPHA);
        if (roundButtonDrawable != null) roundButtonDrawable.setAlpha(CONTROL_BG_ALPHA);
        if (rectButtonDrawable != null) rectButtonDrawable.setAlpha(CONTROL_BG_ALPHA);
        if (pillButtonDrawable != null) pillButtonDrawable.setAlpha(CONTROL_BG_ALPHA);
        dpadSvgPaint.setAlpha(DPAD_SVG_ALPHA);

        // Classic SNES palette: light controls + dark text.
        textPaint.setColor(UiStyle.TEXT_DETAIL);
        abxyTextPaint.setColor(UiStyle.TEXT_DETAIL);
        fgPaint.setColor(UiStyle.TEXT_DETAIL);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);

        final float pad = Math.max(24f, Math.min(w, h) * 0.04f);
        final float btnBase = Math.max(72f, Math.min(w, h) * 0.13f);
        // Make ABXY a bit smaller (was previously increased).
        final float btn = (btnBase * 1.8f) * 1.2f * 0.9f;
        final float small = btnBase * 0.62f;
        final float topH = Math.max(54f, Math.min(w, h) * 0.08f);

        // L/R along the top (reduce width by ~30%).
        float lrFullW = (w * 0.35f) - pad;
        float lrW = lrFullW * 0.7f;
        lRect.set(pad, pad, pad + lrW, pad + topH);
        rRect.set(w - pad - lrW, pad, w - pad, pad + topH);

        // D-pad bottom-left.
        float dSize = (btnBase * 1.35f) * 1.8f * 1.1f; // +10%
        dpadRect.set(pad, h - pad - dSize, pad + dSize, h - pad);
        dpadCx = dpadRect.centerX();
        dpadCy = dpadRect.centerY();
        dpadRadius = Math.min(dpadRect.width(), dpadRect.height()) * 0.5f;

        // Render SVG once per size change (source-of-truth: res/icons/dpad.svg -> res/raw/dpad.svg).
        renderDpadSvgBitmap(Math.round(Math.min(dpadRect.width(), dpadRect.height())));

        // Buttons bottom-right in a diamond.
        // Keep the entire diamond inside (pad .. w-pad) and (pad .. h-pad)
        final float diamondOffset = 0.58f; // tighter spacing between buttons
        final float diamondExtent = diamondOffset + 0.60f; // offset + button width factor
        float cx = w - pad - btn * diamondExtent;
        float cy = h - pad - btn * diamondExtent;

        aRect.set(cx + btn * diamondOffset, cy, cx + btn * (diamondOffset + 0.60f), cy + btn * 0.60f);
        bRect.set(cx, cy + btn * diamondOffset, cx + btn * 0.60f, cy + btn * (diamondOffset + 0.60f));
        xRect.set(cx, cy - btn * diamondOffset, cx + btn * 0.60f, cy + btn * (0.60f - diamondOffset));
        yRect.set(cx - btn * diamondOffset, cy, cx + btn * (0.60f - diamondOffset), cy + btn * 0.60f);

        // Start/Select/State center bottom.
        float midY = h - pad - small;
        float midW = Math.max(120f, w * 0.16f);
        float gap = Math.max(10f, pad * 0.45f);
        float total = midW * 3f + gap * 2f;
        float left = (w - total) * 0.5f;
        selectRect.set(left, midY, left + midW, midY + small);
        stateRect.set(left + midW + gap, midY, left + midW + gap + midW, midY + small);
        startRect.set(left + (midW + gap) * 2f, midY, left + (midW + gap) * 2f + midW, midY + small);

        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        drawDpad(canvas);
        drawButton(canvas, aRect, "A", roundButtonDrawable);
        drawButton(canvas, bRect, "B", roundButtonDrawable);
        drawButton(canvas, xRect, "X", roundButtonDrawable);
        drawButton(canvas, yRect, "Y", roundButtonDrawable);
        drawButton(canvas, selectRect, "SELECT", rectButtonDrawable);
        if (showStateButton) drawButton(canvas, stateRect, "STATE", rectButtonDrawable);
        drawButton(canvas, startRect, "START", rectButtonDrawable);
        drawButton(canvas, lRect, "L", pillButtonDrawable);
        drawButton(canvas, rRect, "R", pillButtonDrawable);
    }

    private void drawDpad(Canvas c) {
        if (dpadDrawable != null) {
            // Small inset to avoid clipping the 2dp border stroke at the bounds.
            float inset = 2f;
            dpadDrawable.setBounds(
                    (int) (dpadRect.left + inset),
                    (int) (dpadRect.top + inset),
                    (int) (dpadRect.right - inset),
                    (int) (dpadRect.bottom - inset));
            dpadDrawable.draw(c);
        } else {
            c.drawCircle(dpadCx, dpadCy, dpadRadius, bgPaint);
            c.drawCircle(dpadCx, dpadCy, dpadRadius, fgPaint);
        }

        if (dpadSvgBitmap != null && !dpadSvgBitmap.isRecycled()) {
            // Inset SVG so it doesn't paint over the border stroke.
            float svgInset = Math.max(6f, Math.min(dpadRect.width(), dpadRect.height()) * 0.08f);
            Rect src = new Rect(0, 0, dpadSvgBitmap.getWidth(), dpadSvgBitmap.getHeight());
            RectF dst = new RectF(
                    dpadRect.left + svgInset,
                    dpadRect.top + svgInset,
                    dpadRect.right - svgInset,
                    dpadRect.bottom - svgInset);

            // Clip to a circle so any SVG background/rect can't show as a square.
            int save = c.save();
            Path clip = new Path();
            float clipR = Math.max(1f, (Math.min(dst.width(), dst.height()) * 0.5f));
            clip.addCircle(dst.centerX(), dst.centerY(), clipR, Path.Direction.CW);
            c.clipPath(clip);
            c.drawBitmap(dpadSvgBitmap, src, dst, dpadSvgPaint);
            c.restoreToCount(save);
        }
    }

    private void renderDpadSvgBitmap(int sizePx) {
        if (sizePx <= 0) {
            dpadSvgBitmap = null;
            return;
        }

        try {
            SVG svg = SVG.getFromResource(getContext(), R.raw.dpad);
            // Force the SVG to render scaled to our bitmap size.
            svg.setDocumentWidth(sizePx);
            svg.setDocumentHeight(sizePx);
            Bitmap bmp = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bmp);
            svg.renderToCanvas(canvas);
            dpadSvgBitmap = bmp;
        } catch (Exception ignored) {
            dpadSvgBitmap = null;
        }
    }

    private boolean isInDpad(float x, float y) {
        float dx = x - dpadCx;
        float dy = y - dpadCy;
        return (dx * dx + dy * dy) <= (dpadRadius * dpadRadius);
    }

    private void drawButton(Canvas c, RectF r, String label, Drawable bg) {
        if (bg != null) {
            bg.setBounds((int) r.left, (int) r.top, (int) r.right, (int) r.bottom);
            bg.draw(c);
        } else {
            c.drawRoundRect(r, 18f, 18f, bgPaint);
            c.drawRoundRect(r, 18f, 18f, fgPaint);
        }

        Paint p = textPaint;
        if (label != null && label.length() == 1) {
            char ch = label.charAt(0);
            if (ch == 'A' || ch == 'B' || ch == 'X' || ch == 'Y') {
                p = abxyTextPaint;
            }
        }

        // Vertical centering: approximate baseline.
        float y = r.centerY() - (p.descent() + p.ascent()) * 0.5f;
        c.drawText(label, r.centerX(), y, p);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (event == null) return false;

        final int action = event.getActionMasked();
        final int index = event.getActionIndex();

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int pid = event.getPointerId(index);
                float x = event.getX(index);
                float y = event.getY(index);
                TouchBinding b = bindPointer(pid, x, y);
                if (b != null) {
                    pointerBindings.put(pid, b);
                    if (b.type == BindingType.DPAD) {
                        updateDpadForPointer(b, x, y);
                    }
                    invalidate();
                    return true;
                }
                break;
            }

            case MotionEvent.ACTION_MOVE: {
                // Update all active pointers (mainly for DPAD).
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int pid = event.getPointerId(i);
                    TouchBinding b = pointerBindings.get(pid);
                    if (b == null) continue;
                    if (b.type == BindingType.DPAD) {
                        updateDpadForPointer(b, event.getX(i), event.getY(i));
                    }
                }
                invalidate();
                return !pointerBindings.isEmpty();
            }

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int pid = event.getPointerId(index);
                TouchBinding b = pointerBindings.remove(pid);
                if (b != null) {
                    releaseBinding(b);
                    invalidate();
                    return true;
                }
                break;
            }
        }

        return !pointerBindings.isEmpty();
    }

    private TouchBinding bindPointer(int pid, float x, float y) {
        if (isInDpad(x, y)) return new TouchBinding(BindingType.DPAD);
        if (aRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_A); return new TouchBinding(BindingType.A); }
        if (bRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_B); return new TouchBinding(BindingType.B); }
        if (xRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_X); return new TouchBinding(BindingType.X); }
        if (yRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_Y); return new TouchBinding(BindingType.Y); }
        if (startRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_START); return new TouchBinding(BindingType.START); }
        if (showStateButton && stateRect.contains(x, y)) {
            if (onStateRequested != null) onStateRequested.run();
            return new TouchBinding(BindingType.STATE);
        }
        if (selectRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_SELECT); return new TouchBinding(BindingType.SELECT); }
        if (lRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_L1); return new TouchBinding(BindingType.L); }
        if (rRect.contains(x, y)) { pressKey(KeyEvent.KEYCODE_BUTTON_R1); return new TouchBinding(BindingType.R); }
        return null;
    }

    private void releaseBinding(TouchBinding b) {
        switch (b.type) {
            case DPAD:
                applyDpad(false, false, false, false, b);
                return;
            case A: releaseKey(KeyEvent.KEYCODE_BUTTON_A); return;
            case B: releaseKey(KeyEvent.KEYCODE_BUTTON_B); return;
            case X: releaseKey(KeyEvent.KEYCODE_BUTTON_X); return;
            case Y: releaseKey(KeyEvent.KEYCODE_BUTTON_Y); return;
            case STATE: return;
            case START: releaseKey(KeyEvent.KEYCODE_BUTTON_START); return;
            case SELECT: releaseKey(KeyEvent.KEYCODE_BUTTON_SELECT); return;
            case L: releaseKey(KeyEvent.KEYCODE_BUTTON_L1); return;
            case R: releaseKey(KeyEvent.KEYCODE_BUTTON_R1); return;
        }
    }

    private void updateDpadForPointer(TouchBinding b, float x, float y) {
        float cx = dpadRect.centerX();
        float cy = dpadRect.centerY();
        float rx = (x - cx) / (dpadRect.width() * 0.5f);
        float ry = (y - cy) / (dpadRect.height() * 0.5f);

        boolean left = rx < -DEADZONE;
        boolean right = rx > DEADZONE;
        boolean up = ry < -DEADZONE;
        boolean down = ry > DEADZONE;

        applyDpad(up, down, left, right, b);
    }

    private void applyDpad(boolean up, boolean down, boolean left, boolean right, TouchBinding b) {
        // Release any previously-held directions for this pointer.
        if (b.u && !up) releaseKey(KeyEvent.KEYCODE_DPAD_UP);
        if (b.d && !down) releaseKey(KeyEvent.KEYCODE_DPAD_DOWN);
        if (b.l && !left) releaseKey(KeyEvent.KEYCODE_DPAD_LEFT);
        if (b.r && !right) releaseKey(KeyEvent.KEYCODE_DPAD_RIGHT);

        if (!b.u && up) pressKey(KeyEvent.KEYCODE_DPAD_UP);
        if (!b.d && down) pressKey(KeyEvent.KEYCODE_DPAD_DOWN);
        if (!b.l && left) pressKey(KeyEvent.KEYCODE_DPAD_LEFT);
        if (!b.r && right) pressKey(KeyEvent.KEYCODE_DPAD_RIGHT);

        b.u = up;
        b.d = down;
        b.l = left;
        b.r = right;
    }

    private static void pressKey(int keyCode) {
        NativeBridge.nativeOnKey(keyCode, 0);
    }

    private static void releaseKey(int keyCode) {
        NativeBridge.nativeOnKey(keyCode, 1);
    }
}
