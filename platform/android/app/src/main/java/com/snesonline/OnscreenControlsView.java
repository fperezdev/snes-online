package com.snesonline;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashMap;

public final class OnscreenControlsView extends View {
    private static final float DEADZONE = 0.18f;

    private final Paint bgPaint = new Paint();
    private final Paint fgPaint = new Paint();
    private final Paint textPaint = new Paint();

    private final RectF dpadRect = new RectF();
    private final RectF aRect = new RectF();
    private final RectF bRect = new RectF();
    private final RectF xRect = new RectF();
    private final RectF yRect = new RectF();
    private final RectF startRect = new RectF();
    private final RectF selectRect = new RectF();
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
        START,
        SELECT,
        L,
        R
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

        setWillNotDraw(false);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);

        final float pad = Math.max(24f, Math.min(w, h) * 0.04f);
        final float btnBase = Math.max(72f, Math.min(w, h) * 0.13f);
        final float btn = btnBase * 1.8f; // ABXY size (+20%)
        final float small = btnBase * 0.62f;
        final float topH = Math.max(54f, Math.min(w, h) * 0.08f);

        // L/R along the top.
        lRect.set(pad, pad, w * 0.35f, pad + topH);
        rRect.set(w * 0.65f, pad, w - pad, pad + topH);

        // D-pad bottom-left.
        float dSize = (btnBase * 1.35f) * 1.8f; // +20%
        dpadRect.set(pad, h - pad - dSize, pad + dSize, h - pad);
        dpadCx = dpadRect.centerX();
        dpadCy = dpadRect.centerY();
        dpadRadius = Math.min(dpadRect.width(), dpadRect.height()) * 0.5f;

        // Buttons bottom-right in a diamond.
        // Keep the entire diamond inside (pad .. w-pad) and (pad .. h-pad)
        final float diamondOffset = 0.65f; // more separation between buttons
        final float diamondExtent = diamondOffset + 0.60f; // offset + button width factor
        float cx = w - pad - btn * diamondExtent;
        float cy = h - pad - btn * diamondExtent;

        aRect.set(cx + btn * diamondOffset, cy, cx + btn * (diamondOffset + 0.60f), cy + btn * 0.60f);
        bRect.set(cx, cy + btn * diamondOffset, cx + btn * 0.60f, cy + btn * (diamondOffset + 0.60f));
        xRect.set(cx, cy - btn * diamondOffset, cx + btn * 0.60f, cy + btn * (0.60f - diamondOffset));
        yRect.set(cx - btn * diamondOffset, cy, cx + btn * (0.60f - diamondOffset), cy + btn * 0.60f);

        // Start/Select center bottom.
        float midY = h - pad - small;
        float midW = Math.max(140f, w * 0.18f);
        selectRect.set(w * 0.5f - midW - pad, midY, w * 0.5f - pad, midY + small);
        startRect.set(w * 0.5f + pad, midY, w * 0.5f + midW + pad, midY + small);

        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        drawDpad(canvas);
        drawButton(canvas, aRect, "A");
        drawButton(canvas, bRect, "B");
        drawButton(canvas, xRect, "X");
        drawButton(canvas, yRect, "Y");
        drawButton(canvas, selectRect, "SELECT");
        drawButton(canvas, startRect, "START");
        drawButton(canvas, lRect, "L");
        drawButton(canvas, rRect, "R");
    }

    private void drawDpad(Canvas c) {
        c.drawCircle(dpadCx, dpadCy, dpadRadius, bgPaint);
        c.drawCircle(dpadCx, dpadCy, dpadRadius, fgPaint);
        float y = dpadCy - (textPaint.descent() + textPaint.ascent()) * 0.5f;
        c.drawText("D-PAD", dpadCx, y, textPaint);
    }

    private boolean isInDpad(float x, float y) {
        float dx = x - dpadCx;
        float dy = y - dpadCy;
        return (dx * dx + dy * dy) <= (dpadRadius * dpadRadius);
    }

    private void drawButton(Canvas c, RectF r, String label) {
        c.drawRoundRect(r, 18f, 18f, bgPaint);
        c.drawRoundRect(r, 18f, 18f, fgPaint);
        // Vertical centering: approximate baseline.
        float y = r.centerY() - (textPaint.descent() + textPaint.ascent()) * 0.5f;
        c.drawText(label, r.centerX(), y, textPaint);
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
