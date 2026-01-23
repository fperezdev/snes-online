package com.snesonline;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;
import android.os.Process;
import android.util.Log;
import android.util.TypedValue;
import android.view.InputDevice;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Toast;
import android.widget.TextView;
import android.widget.FrameLayout;
import android.widget.Button;
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
    private static final String TAG = "SnesOnline";
    private SurfaceView surfaceView;
    private SurfaceHolder holder;

    private FrameLayout gameRoot;
    private OnscreenControlsView onscreenControls;
    private boolean showOnscreenControls = true;

    private TextView waitingView;

    private int directNetplayRole = 0; // 1=host, 2=join
    private String directSelfEndpoint = ""; // best-effort public mapped ip:port
    private String directTargetEndpoint = ""; // remote ip:port (from connection code)

    private volatile boolean running = false;
    private Thread audioThread;

    private AudioTrack audioTrack;
    // Basic audio configuration (no presets)
    private int audioSampleRateHz = 48000;
    // Write in device-friendly chunks. A local jitter buffer smooths bursty core output.
    private int audioFramesPerWrite = 1024;
    private int audioBufferWriteMultiplier = 4;
    private boolean audioWriteBlocking = true;

    // Local jitter buffer (Java-side) to smooth bursty audio production.
    private short[] audioJitterPcm = null;
    private int audioJitterCapacityFrames = 0;
    private int audioJitterR = 0; // frame index
    private int audioJitterW = 0; // frame index

    private static final int MAX_W = 512;
    private static final int MAX_H = 512;
    private ByteBuffer video;
    private IntBuffer videoInts;
    private int[] videoArray;
    private Bitmap bitmap;

    private final Paint blitPaint = new Paint();

    private boolean gameplayStarted = false;

    private boolean showingSlotDialog = false;
    private boolean pendingNetplayUnpause = false;

    private boolean enableNetplayIntent = false;
    private int localPlayerNumIntent = 1;
    private String statePathIntent = "";

    private String romPathIntent = "";
    private boolean showSaveButtonIntent = false;

    // Some controllers (incl. DualShock 4) report L2/R2 as axes, not keys.
    private boolean l2Down = false;
    private boolean r2Down = false;

    private final Handler ui = new Handler(Looper.getMainLooper());

    private static final String DEFAULT_CORE_ASSET_PATH = "cores/snes9x_libretro_android.so";
    private static final String DEFAULT_CORE_FILENAME = "snes9x_libretro_android.so";
    private static final String DEFAULT_ROOM_SERVER_URL = "https://snes-online-1hgm.onrender.com";

    private void applyImmersiveFullscreen() {
        // Best-effort: hide title bar + status/navigation bars.
        try {
            if (getActionBar() != null) getActionBar().hide();
        } catch (Exception ignored) {}

        try {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        } catch (Exception ignored) {}

        try {
            final View decor = getWindow().getDecorView();
            decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);
        } catch (Exception ignored) {}
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Request no title before view creation.
        try { requestWindowFeature(Window.FEATURE_NO_TITLE); } catch (Exception ignored) {}
        super.onCreate(savedInstanceState);

        applyImmersiveFullscreen();

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
        String statePath = getIntent().getStringExtra("statePath");
        String savePath = getIntent().getStringExtra("savePath");

        boolean enableNetplay = getIntent().getBooleanExtra("enableNetplay", false);
        showOnscreenControls = getIntent().getBooleanExtra("showOnscreenControls", true);
        String remoteHost = getIntent().getStringExtra("remoteHost");
        int remotePort = getIntent().getIntExtra("remotePort", 0);
        int localPort = getIntent().getIntExtra("localPort", 7000);
        int localPlayerNum = getIntent().getIntExtra("localPlayerNum", 0);
        String sharedSecret = getIntent().getStringExtra("sharedSecret");
        String roomServerUrl = getIntent().getStringExtra("roomServerUrl");
        String roomCode = getIntent().getStringExtra("roomCode");
        String roomPassword = getIntent().getStringExtra("roomPassword");

        if (romPath == null) romPath = "";
        if (statePath == null) statePath = "";
        if (savePath == null) savePath = "";
        if (remoteHost == null) remoteHost = "";
        if (sharedSecret == null) sharedSecret = "";
        if (roomServerUrl == null) roomServerUrl = "";
        if (roomCode == null) roomCode = "";
        if (roomPassword == null) roomPassword = "";

        enableNetplayIntent = enableNetplay;
        localPlayerNumIntent = (localPlayerNum == 2) ? 2 : 1;
        statePathIntent = statePath;
        romPathIntent = romPath;
        showSaveButtonIntent = getIntent().getBooleanExtra("showSaveButton", false);

        final String finalRomPath = romPath;
        final String finalCorePath = corePath;
        final String finalStatePath = statePath;
        final String finalSavePath = savePath;
        final int finalLocalPort = localPort;

        applyImmersiveFullscreen();

        if (!enableNetplay) {
            // Non-netplay starts immediately.
            boolean ok = NativeBridge.nativeInitialize(
                    finalCorePath,
                    finalRomPath,
                    finalStatePath,
                    finalSavePath,
                    false,
                    remoteHost,
                    remotePort,
                    finalLocalPort,
                    1,
                    "",
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

        // Netplay: direct-connect (STUN) mode.
        // - Player 1 starts with remoteHost="" and waits for the first packet.
        // - Player 2 starts with remoteHost/remotePort from the connection code.
        final int directRole = (localPlayerNum == 1 || localPlayerNum == 2) ? localPlayerNum : 0;
        final boolean hasDirectTarget = (remoteHost != null && !remoteHost.isEmpty() && remotePort > 0 && remotePort <= 65535);
        if (directRole != 0 && (directRole == 1 || hasDirectTarget)) {
            directNetplayRole = directRole;
            directTargetEndpoint = (hasDirectTarget ? (remoteHost + ":" + remotePort) : "");
            directSelfEndpoint = "";
            if (directRole == 1) {
                // Show the public mapped endpoint the other device should send to.
                try {
                    String mapped = NativeBridge.nativeStunMappedAddress(finalLocalPort);
                    if (mapped != null && mapped.contains(":")) {
                        directSelfEndpoint = mapped.trim();
                    }
                } catch (Exception ignored) {
                }
            }

            final String initialMsg;
            if (directRole == 1) {
                String ep = (directSelfEndpoint != null && !directSelfEndpoint.isEmpty()) ? directSelfEndpoint : (":" + finalLocalPort);
                initialMsg = "Waiting for peer to connect to your port " + ep;
            } else {
                String ep = (directTargetEndpoint != null && !directTargetEndpoint.isEmpty()) ? directTargetEndpoint : (remoteHost + ":" + remotePort);
                initialMsg = "Waiting for peer to connect at " + ep;
            }
            waitingView.setText(initialMsg);
            setContentView(waitingView);

            try {
                boolean ok = NativeBridge.nativeInitialize(
                        finalCorePath,
                        finalRomPath,
                        finalStatePath,
                    finalSavePath,
                        true,
                        (directRole == 2) ? remoteHost : "",
                        (directRole == 2) ? remotePort : 0,
                        finalLocalPort,
                        directRole,
                        "",
                    "",
                    sharedSecret);
                if (!ok) throw new Exception("Native init failed");

                initVideo();
                NativeBridge.nativeStartLoop();
                ui.post(this::startWaitingLoop);
            } catch (Exception e) {
                Toast.makeText(this, "Netplay start failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
                finish();
            }
            return;
        }

        // Netplay: legacy room-server rendezvous mode. Session lifetime is tied to the game.
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
                final int resolvedLocalPlayerNum = r.role;

                boolean ok = NativeBridge.nativeInitialize(
                        finalCorePath,
                        finalRomPath,
                        finalStatePath,
                    finalSavePath,
                        true,
                        resolvedRemoteHost,
                        resolvedRemotePort,
                        finalLocalPort,
                        resolvedLocalPlayerNum,
                        "",
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

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyImmersiveFullscreen();
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

                final String msg;
                if (st == 4) {
                    msg = "SYNCING SAVE STATE...\n\nPlease wait.";
                } else if (st == 1) {
                    if (directNetplayRole == 1) {
                        String ep = (directSelfEndpoint != null && !directSelfEndpoint.isEmpty()) ? directSelfEndpoint : "";
                        msg = "Waiting for peer to connect to your port " + (ep.isEmpty() ? "" : ep);
                    } else if (directNetplayRole == 2) {
                        String ep = (directTargetEndpoint != null && !directTargetEndpoint.isEmpty()) ? directTargetEndpoint : "";
                        msg = "Waiting for peer to connect at " + (ep.isEmpty() ? "" : ep);
                    } else {
                        msg = "Waiting for peer...";
                    }
                } else {
                    msg = "WAITING FOR INPUTS...\n\nConnecting...";
                }
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

                // Host-only rule (netplay): only host can save/load.
                final boolean canUseSaveLoad = showSaveButtonIntent && (!enableNetplayIntent || localPlayerNumIntent == 1);
                // Keep only the top-center State pill; do not show the in-overlay STATE button.
                onscreenControls.setShowStateButton(false);
                onscreenControls.setOnStateRequested(null);
                gameRoot.addView(onscreenControls, new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));

                // Tiny top-center pills (Back + State).
                addTopPillButtons(gameRoot, canUseSaveLoad);
            } else {
                // Still show Back/State pills even when on-screen controls are off.
                final boolean canUseSaveLoad = showSaveButtonIntent && (!enableNetplayIntent || localPlayerNumIntent == 1);
                addTopPillButtons(gameRoot, canUseSaveLoad);
            }
        }

        setContentView(gameRoot);
        surfaceView.requestFocus();
        setupAudio();

        running = true;
        startAudioThread();
        startRenderLoop();
    }

    private void addTopPillButtons(FrameLayout root, boolean showState) {
        final LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER);

        final Button back = new Button(this);
        styleTopPill(back);
        back.setText("Back");
        back.setOnClickListener(v -> finish());

        final Button state = new Button(this);
        styleTopPill(state);
        state.setText("State");
        state.setOnClickListener(v -> ui.post(this::showStateDialog));
        state.setVisibility(showState ? View.VISIBLE : View.GONE);

        bar.addView(back, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout.LayoutParams stLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        stLp.leftMargin = dp(8);
        bar.addView(state, stLp);

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
        lp.topMargin = dp(8);
        root.addView(bar, lp);
    }

    private void styleTopPill(Button b) {
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setPadding(dp(8), dp(3), dp(8), dp(3));
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setMinWidth(0);
        b.setMinimumWidth(0);

        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xC0000000); // black with alpha
        bg.setCornerRadius(dp(999));
        b.setBackground(bg);
    }

    private int dp(int dp) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP,
                dp,
                getResources().getDisplayMetrics());
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
        int nativeHzRaw = 0;
        int tries = 0;
        // Some cores only report sample rate after the first frame.
        // Wait a short amount of time to avoid configuring AudioTrack at the wrong rate.
        for (; tries < 60; ++tries) { // ~600ms worst-case
            nativeHzRaw = NativeBridge.nativeGetAudioSampleRateHz();
            if (nativeHzRaw >= 8000 && nativeHzRaw <= 192000) break;
            try { android.os.SystemClock.sleep(10); } catch (Exception ignored) {}
        }

        audioSampleRateHz = nativeHzRaw;
        if (audioSampleRateHz < 8000 || audioSampleRateHz > 192000) {
            audioSampleRateHz = 48000;
        }

        final int channelMask = AudioFormat.CHANNEL_OUT_STEREO;
        final int format = AudioFormat.ENCODING_PCM_16BIT;

        // Stereo S16: 2 channels * 2 bytes
        final int frameBytes = 4;

        int minBytes = AudioTrack.getMinBufferSize(audioSampleRateHz, channelMask, format);
        if (minBytes <= 0) minBytes = 4096;

        // Keep light headroom to reduce underruns with low latency.
        int minFrames = (minBytes + frameBytes - 1) / frameBytes;

        if (audioFramesPerWrite < 256) audioFramesPerWrite = 256;

        int mult = audioBufferWriteMultiplier;
        if (mult < 2) mult = 2;
        int targetFrames = Math.max(minFrames, audioFramesPerWrite * mult);
        int bufferBytes = targetFrames * frameBytes;

        AudioAttributes attrs = new AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_GAME)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build();
        AudioFormat fmt = new AudioFormat.Builder()
            .setSampleRate(audioSampleRateHz)
            .setEncoding(format)
            .setChannelMask(channelMask)
            .build();

        if (Build.VERSION.SDK_INT >= 21) {
            AudioTrack.Builder b = new AudioTrack.Builder()
                .setAudioAttributes(attrs)
                .setAudioFormat(fmt)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(bufferBytes);
            audioTrack = b.build();
        } else {
            audioTrack = new AudioTrack(
                attrs,
                fmt,
                bufferBytes,
                AudioTrack.MODE_STREAM,
                AudioManager.AUDIO_SESSION_ID_GENERATE);
        }

        audioTrack.play();
    }

    private void startAudioThread() {
        audioThread = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);

            final int framesWanted = Math.max(audioFramesPerWrite, 256);
            short[] tmp = new short[framesWanted * 2];
            final int pumpChunkFrames = Math.max(512, Math.min(4096, framesWanted * 2));
            short[] pumpChunk = new short[pumpChunkFrames * 2];

            // Allocate ~0.5s jitter buffer (at least 4 write-chunks).
            audioJitterCapacityFrames = Math.max(framesWanted * 4, Math.max(4096, audioSampleRateHz / 2));
            audioJitterPcm = new short[audioJitterCapacityFrames * 2];
            audioJitterR = 0;
            audioJitterW = 0;
            while (running) {
                // 1) Pump as much as we can from native into the jitter buffer (non-blocking).
                // Native typically produces audio in bursts; this buffer smooths that.
                for (int pumpIters = 0; pumpIters < 32 && running; pumpIters++) {
                    int buffered = audioJitterW - audioJitterR;
                    if (buffered < 0) buffered += audioJitterCapacityFrames;
                    int free = audioJitterCapacityFrames - buffered;
                    if (free < 64) break;

                    int want = Math.min(pumpChunkFrames, free);
                    int got = NativeBridge.nativePopAudio(pumpChunk, want);
                    if (got < 0) got = 0;
                    if (got == 0) break;

                    int w = audioJitterW;
                    for (int i = 0; i < got; i++) {
                        int idx = (w + i);
                        if (idx >= audioJitterCapacityFrames) idx -= audioJitterCapacityFrames;
                        audioJitterPcm[idx * 2] = pumpChunk[i * 2];
                        audioJitterPcm[idx * 2 + 1] = pumpChunk[i * 2 + 1];
                    }
                    w += got;
                    if (w >= audioJitterCapacityFrames) w -= audioJitterCapacityFrames;
                    audioJitterW = w;
                }

                // 2) Produce exactly framesWanted for AudioTrack.
                int available = audioJitterW - audioJitterR;
                if (available < 0) available += audioJitterCapacityFrames;
                int take = Math.min(framesWanted, available);

                int r = audioJitterR;
                for (int i = 0; i < take; i++) {
                    int idx = r + i;
                    if (idx >= audioJitterCapacityFrames) idx -= audioJitterCapacityFrames;
                    tmp[i * 2] = audioJitterPcm[idx * 2];
                    tmp[i * 2 + 1] = audioJitterPcm[idx * 2 + 1];
                }
                r += take;
                if (r >= audioJitterCapacityFrames) r -= audioJitterCapacityFrames;
                audioJitterR = r;

                if (take < framesWanted) {
                    int missing = framesWanted - take;
                    int start = take * 2;
                    int end = framesWanted * 2;
                    for (int i = start; i < end; i++) tmp[i] = 0;
                }

                int off = 0;
                int remaining = framesWanted * 2;
                while (remaining > 0 && running) {
                    int n;
                    if (Build.VERSION.SDK_INT >= 23) {
                        n = audioTrack.write(
                                tmp,
                                off,
                                remaining,
                                audioWriteBlocking ? AudioTrack.WRITE_BLOCKING : AudioTrack.WRITE_NON_BLOCKING);
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
            int cw = c.getWidth();
            int ch = c.getHeight();
            if (cw <= 0 || ch <= 0) return;

            // Fit the emulated frame inside the screen while preserving aspect ratio.
            float sx = cw / (float) w;
            float sy = ch / (float) h;
            float s = Math.min(sx, sy);

            int dw = Math.max(1, Math.round(w * s));
            int dh = Math.max(1, Math.round(h * s));
            int left = (cw - dw) / 2;
            int top = (ch - dh) / 2;
            Rect dst = new Rect(left, top, left + dw, top + dh);
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

    private static String baseNameNoExt(String path) {
        if (path == null) return "";
        try {
            String name = new File(path).getName();
            int dot = name.lastIndexOf('.');
            if (dot > 0) name = name.substring(0, dot);
            name = name.trim();
            return name;
        } catch (Exception ignored) {
            return "";
        }
    }

    private static String sanitizeSimpleBase(String base) {
        if (base == null) return "";
        String s = base.trim();
        if (s.isEmpty()) return "";
        s = s.replace("/", "_").replace("\\\\", "_");
        if (s.length() > 60) s = s.substring(0, 60);
        return s;
    }

    private File slotFileForRom(int slotIndex1Based) {
        String base = sanitizeSimpleBase(baseNameNoExt(romPathIntent));
        if (base.isEmpty()) base = "game";
        File dir = new File(getFilesDir(), "states");
        dir.mkdirs();
        return new File(dir, base + "_slot" + slotIndex1Based + ".state");
    }

    private void showStateDialog() {
        if (showingSlotDialog) return;

        // Netplay rule: only host can create/load the canonical savestate.
        if (enableNetplayIntent && localPlayerNumIntent != 1) {
            Toast.makeText(this, "Only the host can save/load.", Toast.LENGTH_SHORT).show();
            return;
        }

        showingSlotDialog = true;
        pendingNetplayUnpause = false;
        NativeBridge.nativeSetPaused(true);

        final File[] slots = new File[5];
        for (int i = 0; i < 5; i++) slots[i] = slotFileForRom(i + 1);

        final String[] saveLabels = new String[5];
        final String[] loadLabels = new String[5];
        final Runnable refreshLabels = () -> {
            for (int i = 0; i < 5; i++) {
                final int slot = i + 1;
                File f = slots[i];
                if (f != null && f.exists() && f.length() > 0) {
                    String ts = String.valueOf(android.text.format.DateFormat.format("yyyy-MM-dd HH:mm", f.lastModified()));
                    saveLabels[i] = "Slot " + slot + "  (" + ts + ")";
                    loadLabels[i] = "Slot " + slot + "  (" + ts + ")";
                } else {
                    saveLabels[i] = "Slot " + slot;
                    loadLabels[i] = "Slot " + slot + "  (empty)";
                }
            }
        };
        refreshLabels.run();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        int pad = (int) (getResources().getDisplayMetrics().density * 16);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(UiStyle.stateDialogBackground());

        final int listHeight = (int) (getResources().getDisplayMetrics().density * 260);

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);

        TextView saveTitle = new TextView(this);
        saveTitle.setText("SAVE");
        saveTitle.setTextColor(UiStyle.saveHeader());
        saveTitle.setTypeface(Typeface.DEFAULT_BOLD);
        saveTitle.setTextSize(18f);
        saveTitle.setPadding(0, 0, 0, pad / 2);
        saveTitle.setGravity(android.view.Gravity.CENTER_HORIZONTAL);

        TextView loadTitle = new TextView(this);
        loadTitle.setText("LOAD");
        loadTitle.setTextColor(UiStyle.loadHeader());
        loadTitle.setTypeface(Typeface.DEFAULT_BOLD);
        loadTitle.setTextSize(18f);
        loadTitle.setPadding(0, 0, 0, pad / 2);
        loadTitle.setGravity(android.view.Gravity.CENTER_HORIZONTAL);

        ListView saveList = new ListView(this);
        ListView loadList = new ListView(this);

        saveList.setDividerHeight(1);
        saveList.setDivider(new android.graphics.drawable.ColorDrawable(Color.argb(140, 255, 255, 255)));
        loadList.setDividerHeight(1);
        loadList.setDivider(new android.graphics.drawable.ColorDrawable(Color.argb(140, 255, 255, 255)));

        // Keep backgrounds opaque so gameplay behind doesn't reduce readability.
        saveList.setBackgroundColor(UiStyle.saveListBackground());
        loadList.setBackgroundColor(UiStyle.loadListBackground());

        saveList.setSelector(new android.graphics.drawable.ColorDrawable(Color.argb(140, 255, 255, 255)));
        loadList.setSelector(new android.graphics.drawable.ColorDrawable(Color.argb(140, 255, 255, 255)));

        ArrayAdapter<String> saveAdapter = new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, saveLabels) {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent) {
                View v = super.getView(position, convertView, parent);
                if (v instanceof TextView) {
                    TextView tv = (TextView) v;
                    tv.setTextColor(Color.WHITE);
                    tv.setTextSize(16f);
                    tv.setPadding(pad / 2, pad / 3, pad / 2, pad / 3);
                    tv.setBackgroundColor(Color.TRANSPARENT);
                }
                return v;
            }
        };
        ArrayAdapter<String> loadAdapter = new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, loadLabels) {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent) {
                View v = super.getView(position, convertView, parent);
                if (v instanceof TextView) {
                    TextView tv = (TextView) v;
                    tv.setTextColor(Color.WHITE);
                    tv.setTextSize(16f);
                    tv.setPadding(pad / 2, pad / 3, pad / 2, pad / 3);
                    tv.setBackgroundColor(Color.TRANSPARENT);
                }
                return v;
            }
        };
        saveList.setAdapter(saveAdapter);
        loadList.setAdapter(loadAdapter);

        saveList.setOnItemClickListener((parent, view, position, id) -> {
            if (position < 0 || position >= slots.length) return;
            File f = slots[position];
            if (f == null) return;
            boolean ok = NativeBridge.nativeSaveStateToFile(f.getAbsolutePath());
            Toast.makeText(this, ok ? ("Saved: slot " + (position + 1)) : "Save failed", Toast.LENGTH_SHORT).show();
            refreshLabels.run();
            saveAdapter.notifyDataSetChanged();
            loadAdapter.notifyDataSetChanged();
        });

        final AlertDialog[] dlgRef = new AlertDialog[1];

        loadList.setOnItemClickListener((parent, view, position, id) -> {
            if (position < 0 || position >= slots.length) return;
            File f = slots[position];
            if (f == null || !f.exists() || f.length() == 0) {
                Toast.makeText(this, "Slot is empty", Toast.LENGTH_SHORT).show();
                return;
            }

            boolean ok = NativeBridge.nativeLoadStateFromFile(f.getAbsolutePath());
            if (!ok) {
                Toast.makeText(this, "Load failed", Toast.LENGTH_SHORT).show();
                return;
            }

            if (enableNetplayIntent) {
                pendingNetplayUnpause = true;
                Toast.makeText(this, "Syncing state...", Toast.LENGTH_SHORT).show();
                if (dlgRef[0] != null) dlgRef[0].dismiss();
                waitForNetplayReadyThenUnpause();
            } else {
                Toast.makeText(this, "Loaded: slot " + (position + 1), Toast.LENGTH_SHORT).show();
                NativeBridge.nativeSetPaused(false);
                if (dlgRef[0] != null) dlgRef[0].dismiss();
            }
        });

        left.addView(saveTitle);
        left.addView(saveList, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, listHeight));
        right.addView(loadTitle);
        right.addView(loadList, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, listHeight));

        root.addView(left, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        View divider = new View(this);
        LinearLayout.LayoutParams divLp = new LinearLayout.LayoutParams((int) (getResources().getDisplayMetrics().density * 2), LinearLayout.LayoutParams.MATCH_PARENT);
        int divPad = (int) (getResources().getDisplayMetrics().density * 10);
        divLp.setMargins(divPad, 0, divPad, 0);
        divider.setLayoutParams(divLp);
        divider.setBackgroundColor(UiStyle.dividerColor());
        root.addView(divider);

        root.addView(right, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle("STATE")
                .setView(root)
                .setNegativeButton("Close", (d, w) -> {
                    // handled by onDismiss
                })
                .setOnDismissListener(d -> {
                    showingSlotDialog = false;
                    if (!pendingNetplayUnpause) {
                        NativeBridge.nativeSetPaused(false);
                    }
                })
                .create();
        dlgRef[0] = dlg;
        dlg.show();
    }

    private void waitForNetplayReadyThenUnpause() {
        ui.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (isFinishing()) {
                    NativeBridge.nativeSetPaused(false);
                    pendingNetplayUnpause = false;
                    return;
                }
                int st = NativeBridge.nativeGetNetplayStatus();
                if (st == 3) {
                    NativeBridge.nativeSetPaused(false);
                    pendingNetplayUnpause = false;
                    Toast.makeText(GameActivity.this, "State synced", Toast.LENGTH_SHORT).show();
                } else {
                    ui.postDelayed(this, 100);
                }
            }
        }, 100);
    }
}
