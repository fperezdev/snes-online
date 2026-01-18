package com.snesonline;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;
import android.os.Process;
import android.view.InputDevice;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;
import android.widget.TextView;
import android.widget.FrameLayout;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;

import org.json.JSONObject;

public class GameActivity extends Activity {
    private SurfaceView surfaceView;
    private SurfaceHolder holder;

    private FrameLayout gameRoot;
    private OnscreenControlsView onscreenControls;
    private boolean showOnscreenControls = true;

    private TextView waitingView;

    private volatile boolean running = false;
    private Thread audioThread;

    private AudioTrack audioTrack;
    private int audioFramesPerWrite = 1024;

    private static final int MAX_W = 512;
    private static final int MAX_H = 512;
    private ByteBuffer video;
    private IntBuffer videoInts;
    private int[] videoArray;
    private Bitmap bitmap;

    private final Paint blitPaint = new Paint();

    private boolean gameplayStarted = false;

    // Some controllers (incl. DualShock 4) report L2/R2 as axes, not keys.
    private boolean l2Down = false;
    private boolean r2Down = false;

    private final Handler ui = new Handler(Looper.getMainLooper());

    private static final String DEFAULT_CORE_ASSET_PATH = "cores/snes9x_libretro_android.so";
    private static final String DEFAULT_CORE_FILENAME = "snes9x_libretro_android.so";
    private static final String DEFAULT_ROOM_SERVER_URL = "https://snes-online-1hgm.onrender.com";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        surfaceView = new SurfaceView(this);
        surfaceView.setFocusable(true);
        surfaceView.setFocusableInTouchMode(true);
        holder = surfaceView.getHolder();
        waitingView = new TextView(this);
        waitingView.setTextColor(Color.WHITE);
        waitingView.setBackgroundColor(Color.BLACK);
        waitingView.setTextSize(20.0f);
        waitingView.setPadding(32, 32, 32, 32);
        waitingView.setText("Starting...");

        String corePath = ensureBundledCoreReady();
        String romPath = getIntent().getStringExtra("romPath");

        boolean enableNetplay = getIntent().getBooleanExtra("enableNetplay", false);
        showOnscreenControls = getIntent().getBooleanExtra("showOnscreenControls", true);
        String remoteHost = getIntent().getStringExtra("remoteHost");
        int remotePort = getIntent().getIntExtra("remotePort", 7000);
        int localPort = getIntent().getIntExtra("localPort", 7000);
        String roomServerUrl = getIntent().getStringExtra("roomServerUrl");
        String roomCode = getIntent().getStringExtra("roomCode");
        String roomPassword = getIntent().getStringExtra("roomPassword");

        if (romPath == null) romPath = "";
        if (remoteHost == null) remoteHost = "";
        if (roomServerUrl == null) roomServerUrl = "";
        if (roomCode == null) roomCode = "";
        if (roomPassword == null) roomPassword = "";

        final String finalRomPath = romPath;
        final String finalCorePath = corePath;
        final int finalLocalPort = localPort;

        if (!enableNetplay) {
            // Non-netplay starts immediately.
            boolean ok = NativeBridge.nativeInitialize(
                    finalCorePath,
                    finalRomPath,
                    false,
                    remoteHost,
                    remotePort,
                    finalLocalPort,
                    1,
                    "",
                    "");
            if (!ok) {
                Toast.makeText(this, "Failed to start.", Toast.LENGTH_LONG).show();
                finish();
                return;
            }
            initVideo();
            NativeBridge.nativeStartLoop();
            startGameplay();
            return;
        }

        // Netplay: connect to room at game start. Session lifetime is tied to the game.
        waitingView.setText("CONNECTING TO ROOM...\n\nPlease wait.");
        setContentView(waitingView);

        final String baseUrl = (roomServerUrl == null || roomServerUrl.trim().isEmpty()) ? DEFAULT_ROOM_SERVER_URL : roomServerUrl.trim();
        final String codeNorm = normalizeRoomCode(roomCode);
        final String pw = roomPassword;

        new Thread(() -> {
            try {
                RoomConnectResult r = connectRoomAtStart(baseUrl, codeNorm, pw, finalLocalPort);
                if (!r.ok) throw new Exception(r.error);

                final String resolvedRemoteHost = (r.role == 2) ? r.hostIp : "";
                final int resolvedRemotePort = (r.role == 2) ? r.hostPort : 7000;
                final int localPlayerNum = r.role;

                boolean ok = NativeBridge.nativeInitialize(
                        finalCorePath,
                        finalRomPath,
                        true,
                        resolvedRemoteHost,
                        resolvedRemotePort,
                        finalLocalPort,
                        localPlayerNum,
                        "",
                        "");
                if (!ok) throw new Exception("Native init failed");

                initVideo();
                NativeBridge.nativeStartLoop();

                ui.post(this::startWaitingLoop);
            } catch (Exception e) {
                ui.post(() -> {
                    Toast.makeText(this, "Room connect failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    finish();
                });
            }
        }, "RoomConnectAtStart").start();
    }

    private void initVideo() {
        video = NativeBridge.nativeGetVideoBufferRGBA();
        if (video != null) {
            video.order(ByteOrder.nativeOrder());
            videoInts = video.asIntBuffer();
        }
        bitmap = Bitmap.createBitmap(MAX_W, MAX_H, Bitmap.Config.ARGB_8888);
        videoArray = new int[MAX_W * MAX_H];

        blitPaint.setFilterBitmap(false);
        blitPaint.setDither(false);
    }

    private static final class RoomConnectResult {
        boolean ok;
        String error;
        int role; // 1 or 2
        String hostIp;
        int hostPort;
    }

    private static RoomConnectResult connectRoomAtStart(String baseUrl, String code, String password, int localPort) {
        RoomConnectResult out = new RoomConnectResult();
        try {
            if (code == null || code.isEmpty()) {
                out.ok = false;
                out.error = "Room code is required";
                return out;
            }
            if (password == null || password.trim().isEmpty()) {
                out.ok = false;
                out.error = "Room password is required";
                return out;
            }

            // First call: determine role (server assigns Player 1 to the first connector).
            JSONObject r0 = postRoomConnect(baseUrl, code, password, 0);
            int role = r0.optInt("role", 0);
            String creatorToken = r0.optString("creatorToken", "");
            boolean waiting = r0.optBoolean("waiting", false);
            JSONObject room = r0.optJSONObject("room");
            int port = (room != null) ? room.optInt("port", 0) : 0;
            String ip;
            if (room != null) {
                String localIp = room.optString("localIp", "");
                ip = (localIp != null && !localIp.isEmpty()) ? localIp : room.optString("ip", "");
            } else {
                ip = "";
            }

            if (role == 1) {
                // Player 1 must provide its public (NAT-mapped) UDP port (via STUN) to finalize the room.
                if (port == 0) {
                    int publicPort = NativeBridge.nativeStunPublicUdpPort(localPort);
                    if (publicPort < 1 || publicPort > 65535) {
                        throw new Exception("STUN failed (cannot discover public UDP port)");
                    }
                    JSONObject r1 = postRoomConnect(baseUrl, code, password, publicPort, creatorToken);
                    role = r1.optInt("role", 1);
                    waiting = r1.optBoolean("waiting", false);
                    room = r1.optJSONObject("room");
                    port = (room != null) ? room.optInt("port", publicPort) : publicPort;
                    if (room != null) {
                        String localIp = room.optString("localIp", "");
                        ip = (localIp != null && !localIp.isEmpty()) ? localIp : room.optString("ip", "");
                    } else {
                        ip = "";
                    }
                }

                out.ok = true;
                out.role = 1;
                out.hostIp = ip;
                out.hostPort = port;
                return out;
            }

            if (role == 2) {
                // Player 2 polls until host has finalized port.
                long deadlineMs = System.currentTimeMillis() + 15_000;
                while (waiting || port == 0) {
                    if (System.currentTimeMillis() > deadlineMs) {
                        out.ok = false;
                        out.error = "Timed out waiting for host";
                        return out;
                    }
                    try { Thread.sleep(250); } catch (InterruptedException ignored) {}
                    JSONObject r = postRoomConnect(baseUrl, code, password, 0);
                    waiting = r.optBoolean("waiting", false);
                    room = r.optJSONObject("room");
                    port = (room != null) ? room.optInt("port", 0) : 0;
                    if (room != null) {
                        String localIp = room.optString("localIp", "");
                        ip = (localIp != null && !localIp.isEmpty()) ? localIp : room.optString("ip", "");
                    } else {
                        ip = "";
                    }
                }

                if (ip == null || ip.isEmpty() || port < 1 || port > 65535) {
                    out.ok = false;
                    out.error = "Room server returned invalid host endpoint";
                    return out;
                }

                out.ok = true;
                out.role = 2;
                out.hostIp = ip;
                out.hostPort = port;
                return out;
            }

            out.ok = false;
            out.error = "Room server did not assign a role";
            return out;
        } catch (Exception e) {
            out.ok = false;
            out.error = e.getMessage();
            return out;
        }
    }

    private static JSONObject postRoomConnect(String baseUrl, String code, String password, int port) throws Exception {
        return postRoomConnect(baseUrl, code, password, port, "");
    }

    private static JSONObject postRoomConnect(String baseUrl, String code, String password, int port, String creatorToken) throws Exception {
        String base = trimTrailingSlash(baseUrl);
        URL u = new URL(base + "/rooms/connect");
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) u.openConnection();
        c.setConnectTimeout(4000);
        c.setReadTimeout(6000);
        c.setRequestMethod("POST");
        c.setDoOutput(true);
        c.setRequestProperty("Content-Type", "application/json; charset=utf-8");

        JSONObject body = new JSONObject();
        body.put("code", code);
        body.put("password", password);
        if (port > 0) body.put("port", port);
        if (creatorToken != null && !creatorToken.isEmpty()) body.put("creatorToken", creatorToken);
        String localIp = localLanIpv4BestEffort();
        if (localIp != null && !localIp.isEmpty()) body.put("localIp", localIp);

        byte[] data = body.toString().getBytes(StandardCharsets.UTF_8);
        try (java.io.OutputStream os = c.getOutputStream()) {
            os.write(data);
        }

        int rc = c.getResponseCode();
        java.io.InputStream in = (rc >= 200 && rc < 300) ? c.getInputStream() : c.getErrorStream();
        String resp = readAllUtf8(in);
        JSONObject json = new JSONObject(resp);
        if (!json.optBoolean("ok", false)) {
            throw new Exception(json.optString("error", "http_" + rc));
        }
        return json;
    }

    private static String readAllUtf8(java.io.InputStream in) throws Exception {
        if (in == null) return "";
        byte[] buf = new byte[32 * 1024];
        int n;
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toString("UTF-8");
    }

    private static String trimTrailingSlash(String s) {
        if (s == null) return "";
        String t = s.trim();
        while (t.endsWith("/")) t = t.substring(0, t.length() - 1);
        return t;
    }

    private static String normalizeRoomCode(String s) {
        if (s == null) return "";
        String up = s.trim().toUpperCase();
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < up.length(); i++) {
            char c = up.charAt(i);
            boolean ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (ok) out.append(c);
        }
        return out.toString();
    }

    private static String localLanIpv4BestEffort() {
        try {
            java.util.Enumeration<java.net.NetworkInterface> en = java.net.NetworkInterface.getNetworkInterfaces();
            if (en == null) return "";
            while (en.hasMoreElements()) {
                java.net.NetworkInterface nif = en.nextElement();
                if (nif == null) continue;
                if (!nif.isUp() || nif.isLoopback()) continue;
                java.util.Enumeration<java.net.InetAddress> addrs = nif.getInetAddresses();
                while (addrs.hasMoreElements()) {
                    java.net.InetAddress a = addrs.nextElement();
                    if (!(a instanceof java.net.Inet4Address)) continue;
                    if (a.isLoopbackAddress()) continue;
                    if (!a.isSiteLocalAddress()) continue; // RFC1918
                    String s = a.getHostAddress();
                    if (s != null && !s.isEmpty()) return s;
                }
            }
        } catch (Exception ignored) {}
        return "";
    }

    private void startWaitingLoop() {
        ui.post(new Runnable() {
            @Override
            public void run() {
                if (isFinishing() || gameplayStarted) return;

                final int st = NativeBridge.nativeGetNetplayStatus();
                if (st == 3) {
                    startGameplay();
                    return;
                }

                final String msg = (st == 1)
                        ? "WAITING FOR PEER...\n\nStart the other device to connect."
                        : "WAITING FOR INPUTS...\n\nConnecting...";
                waitingView.setText(msg);
                ui.postDelayed(this, 100);
            }
        });
    }

    private void startGameplay() {
        if (gameplayStarted) return;
        gameplayStarted = true;

        if (gameRoot == null) {
            gameRoot = new FrameLayout(this);
            gameRoot.addView(surfaceView, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT));

            if (showOnscreenControls) {
                onscreenControls = new OnscreenControlsView(this);
                onscreenControls.setClickable(true);
                onscreenControls.setFocusable(false);
                gameRoot.addView(onscreenControls, new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));
            }
        }

        setContentView(gameRoot);
        surfaceView.requestFocus();
        setupAudio();

        running = true;
        startAudioThread();
        startRenderLoop();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event != null && isControllerSource(event.getSource())) {
            final int action = event.getAction();
            if (action == KeyEvent.ACTION_DOWN || action == KeyEvent.ACTION_UP) {
                // native expects 0=down, 1=up
                NativeBridge.nativeOnKey(event.getKeyCode(), action == KeyEvent.ACTION_DOWN ? 0 : 1);
            }

            if (shouldConsumeControllerKey(event, event.getKeyCode())) {
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (event != null && isControllerSource(event.getSource())
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            if (handleControllerMotion(event)) {
                return true;
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    private String ensureBundledCoreReady() {
        try {
            File dst = new File(getFilesDir(), "cores/" + DEFAULT_CORE_FILENAME);
            File parent = dst.getParentFile();
            if (parent != null) parent.mkdirs();

            if (!dst.exists() || dst.length() == 0) {
                try (InputStream in = getAssets().open(DEFAULT_CORE_ASSET_PATH);
                     FileOutputStream out = new FileOutputStream(dst, false)) {
                    byte[] buf = new byte[64 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                    }
                }
            }

            if (!dst.exists() || dst.length() == 0) return "";
            return dst.getAbsolutePath();
        } catch (Exception ignored) {
            return "";
        }
    }

    private void setupAudio() {
        // Match the libretro core's sample rate to avoid pitch/speed issues.
        int sampleRate = NativeBridge.nativeGetAudioSampleRateHz();
        if (sampleRate < 8000 || sampleRate > 192000) sampleRate = 48000;
        final int channelMask = AudioFormat.CHANNEL_OUT_STEREO;
        final int format = AudioFormat.ENCODING_PCM_16BIT;

        // Stereo S16: 2 channels * 2 bytes
        final int frameBytes = 4;

        int minBytes = AudioTrack.getMinBufferSize(sampleRate, channelMask, format);
        if (minBytes <= 0) minBytes = 4096;

        // Keep light headroom to reduce underruns with low latency.
        int minFrames = (minBytes + frameBytes - 1) / frameBytes;
        audioFramesPerWrite = 512;
        if (audioFramesPerWrite < 256) audioFramesPerWrite = 256;

        int targetFrames = Math.max(minFrames, audioFramesPerWrite * 3);
        int bufferBytes = targetFrames * frameBytes;

        audioTrack = new AudioTrack(
                new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build(),
                new AudioFormat.Builder()
                        .setSampleRate(sampleRate)
                        .setEncoding(format)
                        .setChannelMask(channelMask)
                        .build(),
                bufferBytes,
                AudioTrack.MODE_STREAM,
                AudioManager.AUDIO_SESSION_ID_GENERATE);

        audioTrack.play();
    }

    private void startAudioThread() {
        audioThread = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);

            final int framesWanted = Math.max(audioFramesPerWrite, 256);
            // framesWanted * 2 samples
            short[] tmp = new short[framesWanted * 2];
            while (running) {
                int frames = NativeBridge.nativePopAudio(tmp, framesWanted);
                if (frames < 0) frames = 0;

                // Always write a full chunk. If we didn't get enough audio from native
                // (common during timing hiccups/netplay stalls), zero-pad the remainder.
                int samplesToWrite = frames * 2;
                final int targetSamples = framesWanted * 2;
                if (samplesToWrite < targetSamples) {
                    for (int i = samplesToWrite; i < targetSamples; i++) tmp[i] = 0;
                    samplesToWrite = targetSamples;
                }

                int off = 0;
                int remaining = samplesToWrite;
                while (remaining > 0 && running) {
                    int n;
                    if (Build.VERSION.SDK_INT >= 23) {
                        n = audioTrack.write(tmp, off, remaining, AudioTrack.WRITE_BLOCKING);
                    } else {
                        n = audioTrack.write(tmp, off, remaining);
                    }
                    if (n <= 0) {
                        try { Thread.sleep(2); } catch (InterruptedException ignored) {}
                        break;
                    }
                    off += n;
                    remaining -= n;
                }
            }
        }, "AudioThread");
        audioThread.start();
    }

    private void startRenderLoop() {
        ui.post(new Runnable() {
            @Override
            public void run() {
                if (!running) return;
                renderOnce();
                ui.postDelayed(this, 16);
            }
        });
    }

    private void renderOnce() {
        if (holder == null || video == null || bitmap == null) return;

        final int w = NativeBridge.nativeGetVideoWidth();
        final int h = NativeBridge.nativeGetVideoHeight();
        if (w <= 0 || h <= 0) return;

        // Copy full backing buffer (512x512) then crop on draw.
        if (videoInts == null) return;
        videoInts.position(0);
        videoInts.get(videoArray, 0, MAX_W * MAX_H);
        bitmap.setPixels(videoArray, 0, MAX_W, 0, 0, MAX_W, MAX_H);

        Canvas c = holder.lockCanvas();
        if (c == null) return;
        try {
            c.drawColor(Color.BLACK);
            Rect src = new Rect(0, 0, Math.min(w, MAX_W), Math.min(h, MAX_H));
            Rect dst = new Rect(0, 0, c.getWidth(), c.getHeight());
            c.drawBitmap(bitmap, src, dst, blitPaint);
        } finally {
            holder.unlockCanvasAndPost(c);
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        NativeBridge.nativeOnKey(keyCode, 0);
        return shouldConsumeControllerKey(event, keyCode) || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        NativeBridge.nativeOnKey(keyCode, 1);
        return shouldConsumeControllerKey(event, keyCode) || super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (event != null && isControllerSource(event.getSource())
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            if (handleControllerMotion(event)) return true;
        }
        return super.onGenericMotionEvent(event);
    }

    private static boolean isControllerSource(int source) {
        return ((source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
                || ((source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                || ((source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD);
    }

    private boolean handleControllerMotion(MotionEvent event) {
        final InputDevice device = event.getDevice();

        // Prefer hat/dpad axes if present.
        float hatX = getCenteredAxis(event, device, MotionEvent.AXIS_HAT_X);
        float hatY = getCenteredAxis(event, device, MotionEvent.AXIS_HAT_Y);

        // Left stick (fallback to RX/RY for some mappings).
        float x = getCenteredAxis(event, device, MotionEvent.AXIS_X);
        float y = getCenteredAxis(event, device, MotionEvent.AXIS_Y);
        if (Math.abs(x) <= 0.1f && Math.abs(y) <= 0.1f) {
            float rx = getCenteredAxis(event, device, MotionEvent.AXIS_RX);
            float ry = getCenteredAxis(event, device, MotionEvent.AXIS_RY);
            if (Math.abs(rx) > 0.1f || Math.abs(ry) > 0.1f) {
                x = rx;
                y = ry;
            }
        }

        final float outX = (Math.abs(hatX) > 0.1f || Math.abs(hatY) > 0.1f) ? hatX : x;
        final float outY = (Math.abs(hatX) > 0.1f || Math.abs(hatY) > 0.1f) ? hatY : y;
        NativeBridge.nativeOnAxis(outX, outY);

        // L2/R2 as axes on some controllers (e.g., DS4).
        float l2 = event.getAxisValue(MotionEvent.AXIS_LTRIGGER);
        float r2 = event.getAxisValue(MotionEvent.AXIS_RTRIGGER);
        // Some mappings use Z/RZ for triggers.
        l2 = Math.max(l2, event.getAxisValue(MotionEvent.AXIS_Z));
        r2 = Math.max(r2, event.getAxisValue(MotionEvent.AXIS_RZ));

        final boolean l2Now = l2 > 0.5f;
        final boolean r2Now = r2 > 0.5f;
        if (l2Now != l2Down) {
            l2Down = l2Now;
            NativeBridge.nativeOnKey(KeyEvent.KEYCODE_BUTTON_L2, l2Now ? 0 : 1);
        }
        if (r2Now != r2Down) {
            r2Down = r2Now;
            NativeBridge.nativeOnKey(KeyEvent.KEYCODE_BUTTON_R2, r2Now ? 0 : 1);
        }

        return true;
    }

    private static float getCenteredAxis(MotionEvent event, InputDevice device, int axis) {
        if (device == null) return 0.0f;
        final InputDevice.MotionRange range = device.getMotionRange(axis, event.getSource());
        if (range == null) return 0.0f;

        final float value = event.getAxisValue(axis);
        final float flat = range.getFlat();
        if (Math.abs(value) <= flat) return 0.0f;
        return value;
    }

    private static boolean shouldConsumeControllerKey(KeyEvent event, int keyCode) {
        if (event == null) return false;
        final int source = event.getSource();
        final boolean isController = ((source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
                || ((source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                || ((source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD);
        if (!isController) return false;

        switch (keyCode) {
            // D-pad
            case KeyEvent.KEYCODE_DPAD_UP:
            case KeyEvent.KEYCODE_DPAD_DOWN:
            case KeyEvent.KEYCODE_DPAD_LEFT:
            case KeyEvent.KEYCODE_DPAD_RIGHT:
            // Common gamepad buttons
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_BUTTON_X:
            case KeyEvent.KEYCODE_BUTTON_Y:
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_BUTTON_L2:
            case KeyEvent.KEYCODE_BUTTON_R2:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                return true;
            default:
                return false;
        }
    }

    @Override
    protected void onDestroy() {
        running = false;
        NativeBridge.nativeStopLoop();
        NativeBridge.nativeShutdown();

        if (audioThread != null) {
            try { audioThread.join(500); } catch (InterruptedException ignored) {}
        }
        if (audioTrack != null) {
            audioTrack.stop();
            audioTrack.release();
        }

        super.onDestroy();
    }
}
