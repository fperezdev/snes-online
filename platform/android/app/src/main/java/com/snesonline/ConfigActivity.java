package com.snesonline;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Switch;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;

public class ConfigActivity extends Activity {
    private static final int REQ_PICK_ROM = 1002;

    private static final String DEFAULT_ROOM_SERVER_URL = "https://snes-online-1hgm.onrender.com";

    private static final String PREFS = "snesonline";
    private static final String PREF_ROM_PATH = "romPath";
    private static final String PREF_NETPLAY_ENABLED = "netplayEnabled";
    private static final String PREF_LOCAL_PORT = "localPort";
    private static final String PREF_SHOW_ONSCREEN_CONTROLS = "showOnscreenControls";

    private static final String PREF_ROOM_SERVER_URL = "roomServerUrl";
    private static final String PREF_ROOM_CODE = "roomCode";
    private static final String PREF_ROOM_PASSWORD = "roomPassword";

    // Default bundled/downloadable core (arm64-v8a)
    private static final String DEFAULT_CORE_ASSET_PATH = "cores/snes9x_libretro_android.so";
    private static final String DEFAULT_CORE_FILENAME = "snes9x_libretro_android.so";

    private EditText editRomPath;
    private Switch switchNetplay;
    private Switch switchOnscreenControls;
    private EditText editLocalPort;

    private EditText editRoomServerUrl;
    private EditText editRoomPassword;
    private EditText editRoomCode;
    private TextView txtStatus;

    private String corePath = "";
    private String romPath = "";

    private volatile boolean coreReady = false;

    private boolean netplayEnabled = false;
    private boolean showOnscreenControls = true;
    private int localPort = 7000;

    private String roomServerUrl = "";
    private String roomCode = "";
    private String roomPassword = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_config);

        editRomPath = findViewById(R.id.editRomPath);
        switchNetplay = findViewById(R.id.switchNetplay);
        switchOnscreenControls = findViewById(R.id.switchOnscreenControls);
        editLocalPort = findViewById(R.id.editLocalPort);

        editRoomServerUrl = findViewById(R.id.editRoomServerUrl);
        editRoomPassword = findViewById(R.id.editRoomPassword);
        editRoomCode = findViewById(R.id.editRoomCode);
        txtStatus = findViewById(R.id.txtStatus);

        Button btnPickRom = findViewById(R.id.btnPickRom);
        Button btnStart = findViewById(R.id.btnStart);

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        romPath = prefs.getString(PREF_ROM_PATH, "");
        if (!romPath.isEmpty()) editRomPath.setText(romPath);

        netplayEnabled = prefs.getBoolean(PREF_NETPLAY_ENABLED, false);
        showOnscreenControls = prefs.getBoolean(PREF_SHOW_ONSCREEN_CONTROLS, true);
        localPort = prefs.getInt(PREF_LOCAL_PORT, 7000);

        roomServerUrl = prefs.getString(PREF_ROOM_SERVER_URL, "");
        roomCode = prefs.getString(PREF_ROOM_CODE, "");
        roomPassword = prefs.getString(PREF_ROOM_PASSWORD, "");

        if (roomServerUrl == null || roomServerUrl.trim().isEmpty()) {
            roomServerUrl = DEFAULT_ROOM_SERVER_URL;
        }

        switchNetplay.setChecked(netplayEnabled);
        switchOnscreenControls.setChecked(showOnscreenControls);
        editLocalPort.setText(String.valueOf(localPort));

        if (roomServerUrl != null && !roomServerUrl.isEmpty()) editRoomServerUrl.setText(roomServerUrl);
        if (!roomCode.isEmpty()) editRoomCode.setText(roomCode);
        if (roomPassword != null && !roomPassword.isEmpty()) editRoomPassword.setText(roomPassword);

        btnPickRom.setOnClickListener(v -> pickFile(REQ_PICK_ROM));

        // Always use the bundled Snes9x core. Extract it to internal storage on first run.
        btnStart.setEnabled(false);
        setStatus("Preparing bundled Snes9x core...");
        new Thread(() -> {
            try {
                File dst = new File(getFilesDir(), "cores/" + DEFAULT_CORE_FILENAME);
                if (!dst.exists() || dst.length() == 0) {
                    extractAssetToFile(DEFAULT_CORE_ASSET_PATH, dst);
                }
                corePath = dst.getAbsolutePath();
                coreReady = true;
                runOnUiThread(() -> {
                    btnStart.setEnabled(true);
                    setStatus("Core ready: Snes9x (bundled)");
                });
            } catch (Exception e) {
                coreReady = false;
                runOnUiThread(() -> {
                    btnStart.setEnabled(false);
                    setStatus("Bundled core failed: " + e.getMessage());
                });
            }
        }, "BundledCorePrepare").start();

        btnStart.setOnClickListener(v -> {
            romPath = editRomPath.getText().toString().trim();

            netplayEnabled = switchNetplay.isChecked();
            showOnscreenControls = switchOnscreenControls.isChecked();
            localPort = parseIntOr(editLocalPort.getText().toString().trim(), 7000);

            roomServerUrl = editRoomServerUrl.getText().toString().trim();
            roomCode = editRoomCode.getText().toString().trim();
            roomPassword = editRoomPassword.getText().toString();

            if (!coreReady || corePath.isEmpty()) {
                setStatus("Core not ready yet");
                return;
            }

            if (romPath.isEmpty()) {
                setStatus("Pick a ROM");
                return;
            }

            if (netplayEnabled) {
                if (!isValidPort(localPort)) {
                    setStatus("Netplay enabled: local UDP port must be 1..65535");
                    return;
                }
                if (roomServerUrl == null || roomServerUrl.trim().isEmpty()) {
                    setStatus("Netplay enabled: room server URL is required");
                    return;
                }
                if (!isValidRoomServerUrl(roomServerUrl)) {
                    setStatus("Room server URL must start with http:// or https://");
                    return;
                }
                if (roomCode == null || normalizeRoomCode(roomCode).isEmpty()) {
                    setStatus("Netplay enabled: room code is required");
                    return;
                }
                if (roomPassword == null || roomPassword.trim().isEmpty()) {
                    setStatus("Netplay enabled: room password is required");
                    return;
                }
            }

            prefs.edit()
                    .putString(PREF_ROM_PATH, romPath)
                    .putBoolean(PREF_NETPLAY_ENABLED, netplayEnabled)
                    .putBoolean(PREF_SHOW_ONSCREEN_CONTROLS, showOnscreenControls)
                    .putString(PREF_ROOM_SERVER_URL, roomServerUrl)
                    .putString(PREF_ROOM_CODE, roomCode)
                    .putInt(PREF_LOCAL_PORT, localPort)
                    .putString(PREF_ROOM_PASSWORD, roomPassword)
                    .apply();

            Intent i = new Intent(this, GameActivity.class);
            i.putExtra("romPath", romPath);
            i.putExtra("enableNetplay", netplayEnabled);
            i.putExtra("showOnscreenControls", showOnscreenControls);
            i.putExtra("localPort", localPort);
            i.putExtra("roomServerUrl", netplayEnabled ? roomServerUrl : "");
            i.putExtra("roomCode", netplayEnabled ? normalizeRoomCode(roomCode) : "");
            i.putExtra("roomPassword", netplayEnabled ? roomPassword : "");
            startActivity(i);
        });
    }

    private static boolean isValidRoomServerUrl(String s) {
        if (s == null) return false;
        String t = s.trim().toLowerCase();
        return t.startsWith("http://") || t.startsWith("https://");
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

    private static String httpJson(String method, String urlStr, String apiKey, String bodyJson) throws Exception {
        URL url = new URL(urlStr);
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        conn.setConnectTimeout(3000);
        conn.setReadTimeout(5000);
        conn.setRequestMethod(method);
        conn.setRequestProperty("Accept", "application/json");
        conn.setRequestProperty("Content-Type", "application/json; charset=utf-8");
        if (apiKey != null && !apiKey.isEmpty()) {
            conn.setRequestProperty("X-API-Key", apiKey);
        }

        if (bodyJson != null && !bodyJson.isEmpty() && (method.equals("POST") || method.equals("PUT"))) {
            conn.setDoOutput(true);
            byte[] bytes = bodyJson.getBytes(StandardCharsets.UTF_8);
            try (OutputStream os = conn.getOutputStream()) {
                os.write(bytes);
            }
        }

        int code = conn.getResponseCode();
        InputStream in = (code >= 200 && code < 300) ? conn.getInputStream() : conn.getErrorStream();
        if (in == null) throw new Exception("HTTP " + code);
        byte[] data;
        try (InputStream is = in) {
            data = readAllBytes(is);
        }
        String resp = new String(data, StandardCharsets.UTF_8);
        if (code < 200 || code >= 300) {
            throw new Exception("HTTP " + code + ": " + resp);
        }
        return resp;
    }

    private static byte[] readAllBytes(InputStream in) throws Exception {
        byte[] buf = new byte[16 * 1024];
        int n;
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }

    private static final class RoomLookup {
        String code;
        String ip;
        int port;
        boolean created;
    }

    private static RoomLookup parseRoomResponse(String json) {
        if (json == null) return null;
        // Very small JSON extractor to avoid extra deps.
        // Expected: {"ok":true,"room":{"code":"...","ip":"...","port":7000,...}}
        RoomLookup out = new RoomLookup();
        out.code = extractJsonString(json, "\"code\"");
        out.ip = extractJsonString(json, "\"ip\"");
        String p = extractJsonNumber(json, "\"port\"");
        try { out.port = Integer.parseInt(p); } catch (Exception ignored) { out.port = 7000; }
        out.created = extractJsonBool(json, "\"created\"");
        return out;
    }

    private static boolean extractJsonBool(String json, String keyQuoted) {
        int k = json.indexOf(keyQuoted);
        if (k < 0) return false;
        int c = json.indexOf(':', k);
        if (c < 0) return false;
        int i = c + 1;
        while (i < json.length() && (json.charAt(i) == ' ' || json.charAt(i) == '\n' || json.charAt(i) == '\r' || json.charAt(i) == '\t')) i++;
        return json.startsWith("true", i);
    }

    private static String extractJsonString(String json, String keyQuoted) {
        int k = json.indexOf(keyQuoted);
        if (k < 0) return "";
        int c = json.indexOf(':', k);
        if (c < 0) return "";
        int q1 = json.indexOf('"', c + 1);
        if (q1 < 0) return "";
        int q2 = json.indexOf('"', q1 + 1);
        if (q2 < 0) return "";
        return json.substring(q1 + 1, q2);
    }

    private static String extractJsonNumber(String json, String keyQuoted) {
        int k = json.indexOf(keyQuoted);
        if (k < 0) return "";
        int c = json.indexOf(':', k);
        if (c < 0) return "";
        int i = c + 1;
        while (i < json.length() && (json.charAt(i) == ' ' || json.charAt(i) == '\n' || json.charAt(i) == '\r' || json.charAt(i) == '\t')) i++;
        int j = i;
        while (j < json.length() && (json.charAt(j) >= '0' && json.charAt(j) <= '9')) j++;
        if (j <= i) return "";
        return json.substring(i, j);
    }

    private static Inet4Address resolveIpv4BestEffort(String host) {
        try {
            InetAddress[] addrs = InetAddress.getAllByName(host);
            for (InetAddress a : addrs) {
                if (a instanceof Inet4Address) return (Inet4Address) a;
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    private boolean sendUdpPingBestEffort(String host, int port) {
        try {
            Inet4Address ipv4 = resolveIpv4BestEffort(host);
            if (ipv4 == null) return false;
            byte[] data = "snesonline_ping".getBytes();

            try (DatagramSocket sock = new DatagramSocket()) {
                sock.setSoTimeout(500);
                DatagramPacket p = new DatagramPacket(data, data.length, ipv4, port);
                sock.send(p);
            }
            setStatus("UDP preflight sent to " + ipv4.getHostAddress() + ":" + port + " (check host rx)");
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }

    private static int parseIntOr(String s, int fallback) {
        try {
            return Integer.parseInt(s);
        } catch (Exception ignored) {
            return fallback;
        }
    }

    private static boolean isValidPort(int p) {
        return p >= 1 && p <= 65535;
    }

    private void pickFile(int requestCode) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, requestCode);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;

        try {
            if (requestCode == REQ_PICK_ROM) {
                File dst = new File(getFilesDir(), "roms/rom.sfc");
                String out = copyUriToPrivateFile(uri, dst);
                editRomPath.setText(out);
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_ROM_PATH, out).apply();
                setStatus("ROM copied to: " + out);
            }
        } catch (Exception e) {
            setStatus("Failed to import file: " + e.getMessage());
        }
    }

    private void extractAssetToFile(String assetPath, File dst) throws Exception {
        File parent = dst.getParentFile();
        if (parent != null) parent.mkdirs();

        try (InputStream in = getAssets().open(assetPath);
             FileOutputStream out = new FileOutputStream(dst, false)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }

        if (!dst.exists() || dst.length() == 0) {
            throw new Exception("Extracted file is empty");
        }
    }

    private String copyUriToPrivateFile(Uri uri, File dst) throws Exception {
        File parent = dst.getParentFile();
        if (parent != null) parent.mkdirs();

        ContentResolver cr = getContentResolver();
        try (InputStream in = cr.openInputStream(uri);
             FileOutputStream out = new FileOutputStream(dst, false)) {
            if (in == null) throw new Exception("openInputStream returned null");
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
        return dst.getAbsolutePath();
    }

    private void setStatus(String s) {
        txtStatus.setText(s);
    }
}
