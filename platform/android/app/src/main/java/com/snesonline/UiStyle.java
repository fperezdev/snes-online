package com.snesonline;

public final class UiStyle {
    // Classic SNES palette (Android-only).
    // Primary Base, Console Light Grey, #CEC9CC
    // Secondary Base, Console Dark Grey, #908A99
    // Accent 1, Light Lavender (Buttons), #B5B6E4
    // Accent 2, Dark Purple (Buttons), #4F43AE
    // Text/Detail, Deep Charcoal, #211A21
    public static final int PRIMARY_BASE = 0xFFCEC9CC;
    public static final int SECONDARY_BASE = 0xFF908A99;
    public static final int ACCENT_1 = 0xFFB5B6E4;
    public static final int ACCENT_2 = 0xFF4F43AE;
    public static final int TEXT_DETAIL = 0xFF211A21;

    private UiStyle() {}

    public static int configBackground() { return 0xFFFFFFFF; }
    public static int configText() { return TEXT_DETAIL; }

    // Use slightly translucent dark overlay for in-game dialogs.
    public static int stateDialogBackground() { return 0xF2211A21; }
    public static int saveHeader() { return ACCENT_1; }
    public static int loadHeader() { return ACCENT_2; }
    public static int saveListBackground() { return 0xE6908A99; }
    public static int loadListBackground() { return 0xE6908A99; }
    public static int dividerColor() { return ACCENT_2; }
}
