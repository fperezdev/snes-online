package com.snesonline;

import java.nio.ByteBuffer;

public final class NativeBridge {
    static {
        System.loadLibrary("snesonline_jni");
    }

    private NativeBridge() {}

    public static native boolean nativeInitialize(
            String corePath,
            String romPath,
            boolean enableNetplay,
            String remoteHost,
            int remotePort,
            int localPort,
            int localPlayerNum,
            String roomServerUrl,
            String roomCode);
    public static native void nativeShutdown();

    public static native void nativeStartLoop();
    public static native void nativeStopLoop();

    public static native void nativeOnAxis(float axisX, float axisY);
    public static native void nativeOnKey(int keyCode, int action);

    // Video
    public static native int nativeGetVideoWidth();
    public static native int nativeGetVideoHeight();
    public static native ByteBuffer nativeGetVideoBufferRGBA();

    // Netplay status
    // 0=off, 1=connecting (no peer yet), 2=waiting (peer but missing inputs), 3=ok
    public static native int nativeGetNetplayStatus();

    // Networking helpers
    // Returns the best-effort public mapped UDP port for a socket bound to localPort (0 on failure).
    public static native int nativeStunPublicUdpPort(int localPort);

    // Returns best-effort public mapped UDP address as "ip:port" for a socket bound to localPort ("" on failure).
    public static native String nativeStunMappedAddress(int localPort);

    // Audio
    // Returns frames written into dst (dst length must be framesWanted*2)
    public static native int nativeGetAudioSampleRateHz();
    public static native int nativePopAudio(short[] dstInterleavedStereo, int framesWanted);
}
