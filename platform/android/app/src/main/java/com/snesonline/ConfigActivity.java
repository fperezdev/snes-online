package com.snesonline;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.view.View;
import android.widget.Toast;
import android.widget.Switch;
import android.widget.Button;
import android.widget.ImageButton;
import android.widget.EditText;
import android.widget.TextView;
import android.app.AlertDialog;
import android.graphics.Color;

import android.text.Editable;
import android.text.TextWatcher;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.net.NetworkInterface;
import java.util.Enumeration;

public class ConfigActivity extends Activity {
    private static final int REQ_PICK_ROM = 1002;

    private static final String PREFS = "snesonline";
    private static final String PREF_ROM_PATH = "romPath";
    private static final String PREF_NETPLAY_ENABLED = "netplayEnabled";
    private static final String PREF_LOCAL_PORT = "localPort";
    private static final String PREF_SHOW_ONSCREEN_CONTROLS = "showOnscreenControls";
    private static final String PREF_SHOW_SAVE_BUTTON = "showSaveButton";

    // 1 = host (Player 1), 2 = join (Player 2)
    private static final String PREF_NETPLAY_ROLE = "netplayRole";
    private static final String PREF_REMOTE_HOST = "remoteHost";
    private static final String PREF_REMOTE_PORT = "remotePort";

    // New format: ip:port:secret
    private static final String PREF_CONNECTION_STRING = "connectionString";
    private static final String PREF_SHARED_SECRET = "sharedSecret";

    // Back-compat (older builds stored the base64 connection code here).
    private static final String PREF_CONNECTION_CODE = "connectionCode";

    // Default bundled/downloadable core (arm64-v8a)
    private static final String DEFAULT_CORE_ASSET_PATH = "cores/snes9x_libretro_android.so";
    private static final String DEFAULT_CORE_FILENAME = "snes9x_libretro_android.so";

    private EditText editRomPath;
    private Switch switchNetplay;
    private Switch switchOnscreenControls;
    private Switch switchShowSaveButton;
    private View configRoot;
    private EditText editLocalPort;
    private EditText editSecret;
    private EditText editConnectionCode;
    private TextView txtStatus;
    private Button btnStart;
    private Button btnStartConnection;
    private Button btnJoinConnection;
    private Button btnPickRom;


    private TextView txtOr;
    private TextView txtHostWaiting;
    private View hostWaitingRow;
    private ImageButton btnCopyConnectionIcon;
    private TextView txtJoinTarget;
    private Button btnCancelConnection;

    private String corePath = "";
    private String romPath = "";

    private volatile boolean coreReady = false;

    private boolean netplayEnabled = false;
    private boolean showOnscreenControls = true;
    private boolean showSaveButton = false;
    private int localPort = 7000;


    private int netplayRole = 1;
    private String resolvedRemoteHost = "";
    private int resolvedRemotePort = 0;

    private String connectionString = "";
    private String sharedSecret = "";

    private enum ConnectionUiState {
        IDLE,
        HOST_READY,
        JOIN_INPUT,
        JOIN_READY
    }

    private ConnectionUiState connectionUiState = ConnectionUiState.IDLE;

    private String lastHostConnectionString = ""; // ip:port:secret

    private static final class HostPort {
        String host = "";
        int port = 0;
    }

    // Supports:
    // - ipv4:port
    // - hostname:port
    // - [ipv6]:port
    // - (best-effort) raw ipv6 with port appended (split on last ':')
    private static HostPort parseHostPort(String s) {
        HostPort out = new HostPort();
        if (s == null) return out;
        String t = s.trim();
        if (t.isEmpty()) return out;

        try {
            if (t.startsWith("[")) {
                int r = t.indexOf(']');
                if (r > 1 && r + 2 <= t.length() && t.charAt(r + 1) == ':') {
                    out.host = t.substring(1, r);
                    out.port = parseIntOr(t.substring(r + 2), 0);
                    return out;
                }
                return out;
            }

            int lastColon = t.lastIndexOf(':');
            if (lastColon <= 0 || lastColon + 1 >= t.length()) return out;
            out.host = t.substring(0, lastColon);
            out.port = parseIntOr(t.substring(lastColon + 1), 0);
            return out;
        } catch (Exception ignored) {
            return out;
        }
    }

    private static final class ParsedConnectionString {
        String host = "";
        int port = 0;
        String secret = "";
    }

    private static ParsedConnectionString parseConnectionString(String s) throws Exception {
        if (s == null) throw new Exception("Empty connection string");
        String t = s.trim();
        if (t.isEmpty()) throw new Exception("Empty connection string");

        int lastColon = t.lastIndexOf(':');
        if (lastColon <= 0 || lastColon + 1 >= t.length()) throw new Exception("Expected ip:port:secret");

        String secret = t.substring(lastColon + 1).trim();
        String hostPort = t.substring(0, lastColon).trim();
        if (secret.isEmpty()) throw new Exception("Secret is required");
        if (secret.contains(":")) throw new Exception("Secret cannot contain ':'");

        HostPort hp = parseHostPort(hostPort);
        if (hp.host == null) hp.host = "";
        if (hp.host.isEmpty() || hp.port < 1 || hp.port > 65535) throw new Exception("Invalid host/port");

        ParsedConnectionString out = new ParsedConnectionString();
        out.host = hp.host;
        out.port = hp.port;
        out.secret = secret;
        return out;
    }

    private static String canonicalizeHostPortForShare(String host, int port) {
        if (host == null) host = "";
        String h = host.trim();
        if (h.contains(":") && !h.startsWith("[")) h = "[" + h + "]";
        return h + ":" + port;
    }

    private static void copyToClipboard(Activity a, String label, String text) {
        ClipboardManager cm = (ClipboardManager) a.getSystemService(CLIPBOARD_SERVICE);
        if (cm == null) return;
        cm.setPrimaryClip(ClipData.newPlainText(label, text));
    }

    private void toastCopied() {
        Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show();
    }

    private static String localLanIpv4BestEffort() {
        try {
            Enumeration<NetworkInterface> nifs = NetworkInterface.getNetworkInterfaces();
            if (nifs == null) return "";
            while (nifs.hasMoreElements()) {
                NetworkInterface nif = nifs.nextElement();
                if (nif == null) continue;
                if (!nif.isUp() || nif.isLoopback()) continue;
                Enumeration<java.net.InetAddress> addrs = nif.getInetAddresses();
                while (addrs.hasMoreElements()) {
                    java.net.InetAddress a = addrs.nextElement();
                    if (!(a instanceof java.net.Inet4Address)) continue;
                    String ip = a.getHostAddress();
                    if (ip == null || ip.isEmpty()) continue;
                    if (ip.startsWith("127.")) continue;
                    if (ip.startsWith("10.")) return ip;
                    if (ip.startsWith("192.168.")) return ip;
                    if (ip.startsWith("169.254.")) return ip;
                    if (ip.startsWith("172.")) {
                        // 172.16.0.0 - 172.31.255.255
                        String[] parts = ip.split("\\.");
                        if (parts.length >= 2) {
                            int b = parseIntOr(parts[1], -1);
                            if (b >= 16 && b <= 31) return ip;
                        }
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return "";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_config);

        setTitle("SNES ONLINE");

        configRoot = findViewById(R.id.configRoot);

        editRomPath = findViewById(R.id.editRomPath);
        switchNetplay = findViewById(R.id.switchNetplay);
        switchOnscreenControls = findViewById(R.id.switchOnscreenControls);
        switchShowSaveButton = findViewById(R.id.switchShowSaveButton);
        editLocalPort = findViewById(R.id.editLocalPort);
        editSecret = findViewById(R.id.editSecret);
        editConnectionCode = findViewById(R.id.editConnectionCode);
        txtStatus = findViewById(R.id.txtStatus);

        btnPickRom = findViewById(R.id.btnPickRom);
        btnStart = findViewById(R.id.btnStart);
        btnStartConnection = findViewById(R.id.btnStartConnection);
        btnJoinConnection = findViewById(R.id.btnJoinConnection);

        txtOr = findViewById(R.id.txtOr);
        txtHostWaiting = findViewById(R.id.txtHostWaiting);
        hostWaitingRow = findViewById(R.id.hostWaitingRow);
        btnCopyConnectionIcon = findViewById(R.id.btnCopyConnectionIcon);
        txtJoinTarget = findViewById(R.id.txtJoinTarget);
        btnCancelConnection = findViewById(R.id.btnCancelConnection);

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        romPath = prefs.getString(PREF_ROM_PATH, "");
        if (!romPath.isEmpty()) editRomPath.setText(romPath);

        netplayEnabled = prefs.getBoolean(PREF_NETPLAY_ENABLED, false);
        showOnscreenControls = prefs.getBoolean(PREF_SHOW_ONSCREEN_CONTROLS, true);
        showSaveButton = prefs.getBoolean(PREF_SHOW_SAVE_BUTTON, true);
        localPort = prefs.getInt(PREF_LOCAL_PORT, 7000);

        netplayRole = prefs.getInt(PREF_NETPLAY_ROLE, 1);
        if (netplayRole != 1 && netplayRole != 2) netplayRole = 1;
        resolvedRemoteHost = prefs.getString(PREF_REMOTE_HOST, "");
        resolvedRemotePort = prefs.getInt(PREF_REMOTE_PORT, 0);

        sharedSecret = prefs.getString(PREF_SHARED_SECRET, "");
        connectionString = prefs.getString(PREF_CONNECTION_STRING, "");
        if (connectionString == null || connectionString.isEmpty()) {
            // Back-compat: older builds stored connection codes here.
            connectionString = prefs.getString(PREF_CONNECTION_CODE, "");
        }

        switchNetplay.setChecked(netplayEnabled);
        switchOnscreenControls.setChecked(showOnscreenControls);
        if (switchShowSaveButton != null) switchShowSaveButton.setChecked(showSaveButton);

        applyConfigTheme();

        editLocalPort.setText(String.valueOf(localPort));

        if (editSecret != null && sharedSecret != null && !sharedSecret.isEmpty()) {
            editSecret.setText(sharedSecret);
        }

        if (connectionString != null && !connectionString.isEmpty()) editConnectionCode.setText(connectionString);

        if (btnPickRom != null) btnPickRom.setOnClickListener(v -> pickFile(REQ_PICK_ROM));

        applyInteractiveTheme();

        editRomPath.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) { updateStartButtonEnabled(); }
        });
        editLocalPort.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) { updateStartButtonEnabled(); }
        });
        if (editSecret != null) {
            editSecret.addTextChangedListener(new TextWatcher() {
                @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
                @Override public void afterTextChanged(Editable s) { updateStartButtonEnabled(); }
            });
        }

        switchNetplay.setOnCheckedChangeListener((buttonView, isChecked) -> updateNetplayUi(isChecked));
        updateNetplayUi(netplayEnabled);

        // Always use the bundled Snes9x core. Extract it to internal storage on first run.
        btnStart.setEnabled(false);
        setStatus("Preparing core...");
        new Thread(() -> {
            try {
                File dst = new File(getFilesDir(), "cores/" + DEFAULT_CORE_FILENAME);
                if (!dst.exists() || dst.length() == 0) {
                    extractAssetToFile(DEFAULT_CORE_ASSET_PATH, dst);
                }
                corePath = dst.getAbsolutePath();
                coreReady = true;
                runOnUiThread(() -> {
                    updateStartButtonEnabled();
                    setStatus("Core ready");
                });
            } catch (Exception e) {
                coreReady = false;
                runOnUiThread(() -> {
                    btnStart.setEnabled(false);
                    setStatus("Bundled core failed: " + e.getMessage());
                });
            }
        }, "BundledCorePrepare").start();

        applyConnectionUiState(ConnectionUiState.IDLE);

        btnStart.setOnClickListener(v -> {
            romPath = editRomPath.getText().toString().trim();

            netplayEnabled = switchNetplay.isChecked();
            showOnscreenControls = switchOnscreenControls.isChecked();
            showSaveButton = (switchShowSaveButton != null) && switchShowSaveButton.isChecked();
            localPort = parseIntOr(editLocalPort.getText().toString().trim(), 7000);

            if (!coreReady || corePath.isEmpty()) {
                setStatus("Core not ready yet");
                return;
            }

            if (romPath.isEmpty()) {
                setStatus("Pick a ROM");
                return;
            }

            if (!isValidPort(localPort)) {
                setStatus("Local UDP port must be 1..65535");
                return;
            }

            // Persist basics
            prefs.edit()
                    .putString(PREF_ROM_PATH, romPath)
                    .putBoolean(PREF_NETPLAY_ENABLED, netplayEnabled)
                    .putBoolean(PREF_SHOW_ONSCREEN_CONTROLS, showOnscreenControls)
                    .putBoolean(PREF_SHOW_SAVE_BUTTON, showSaveButton)
                    .putInt(PREF_LOCAL_PORT, localPort)
                    .apply();

            if (!netplayEnabled) {
                // Non-netplay starts immediately.
                Intent i = new Intent(this, GameActivity.class);
                i.putExtra("romPath", romPath);
                i.putExtra("statePath", "");
                i.putExtra("savePath", "");
                i.putExtra("enableNetplay", false);
                i.putExtra("showOnscreenControls", showOnscreenControls);
                i.putExtra("showSaveButton", showSaveButton);
                i.putExtra("localPort", localPort);
                i.putExtra("remoteHost", "");
                i.putExtra("remotePort", 0);
                i.putExtra("localPlayerNum", 1);
                i.putExtra("roomServerUrl", "");
                i.putExtra("roomCode", "");
                i.putExtra("roomPassword", "");
                startActivity(i);
                return;
            }

            // Netplay still starts when Start Game is pressed, but the UI must be "ready" first.
            final int role = prefs.getInt(PREF_NETPLAY_ROLE, netplayRole);
            if (role == 1) {
                if (connectionUiState != ConnectionUiState.HOST_READY || connectionString == null || connectionString.trim().isEmpty()) {
                    setStatus("Press Start connection first");
                    return;
                }

                String secret = (editSecret != null) ? editSecret.getText().toString().trim() : "";
                if (secret.isEmpty()) {
                    setStatus("Set a secret word");
                    return;
                }

                setStatus("Starting netplay as Host...");
                Intent i = new Intent(this, GameActivity.class);
                i.putExtra("romPath", romPath);
                i.putExtra("statePath", "");
                i.putExtra("savePath", "");
                i.putExtra("enableNetplay", true);
                i.putExtra("showOnscreenControls", showOnscreenControls);
                i.putExtra("showSaveButton", showSaveButton);
                i.putExtra("localPort", localPort);
                i.putExtra("remoteHost", "");
                i.putExtra("remotePort", 0);
                i.putExtra("localPlayerNum", 1);
                i.putExtra("sharedSecret", secret);
                i.putExtra("roomServerUrl", "");
                i.putExtra("roomCode", "");
                i.putExtra("roomPassword", "");
                startActivity(i);
                return;
            }

            // role == 2 (join)
            if (connectionUiState != ConnectionUiState.JOIN_READY) {
                setStatus("Paste the code and press Join connection");
                return;
            }

            String savedHost = prefs.getString(PREF_REMOTE_HOST, "");
            int savedPort = prefs.getInt(PREF_REMOTE_PORT, 0);
            String savedSecret = prefs.getString(PREF_SHARED_SECRET, "");
            if (savedHost == null || savedHost.isEmpty() || savedPort < 1 || savedPort > 65535) {
                setStatus("Join endpoint missing. Press Join connection again.");
                applyConnectionUiState(ConnectionUiState.JOIN_INPUT);
                updateStartButtonEnabled();
                return;
            }
            if (savedSecret == null || savedSecret.trim().isEmpty()) {
                setStatus("Secret missing. Press Join connection again.");
                applyConnectionUiState(ConnectionUiState.JOIN_INPUT);
                updateStartButtonEnabled();
                return;
            }

            setStatus("Starting netplay as Join...");
            Intent i = new Intent(this, GameActivity.class);
            i.putExtra("romPath", romPath);
            // Joiner state selection is ignored; only the host's state is used.
            i.putExtra("statePath", "");
            i.putExtra("savePath", "");
            i.putExtra("enableNetplay", true);
            i.putExtra("showOnscreenControls", showOnscreenControls);
            i.putExtra("showSaveButton", showSaveButton);
            i.putExtra("localPort", localPort);
            i.putExtra("remoteHost", savedHost);
            i.putExtra("remotePort", savedPort);
            i.putExtra("localPlayerNum", 2);
            i.putExtra("sharedSecret", savedSecret);
            i.putExtra("roomServerUrl", "");
            i.putExtra("roomCode", "");
            i.putExtra("roomPassword", "");
            startActivity(i);
        });

        btnStartConnection.setOnClickListener(v -> {
            showOnscreenControls = switchOnscreenControls.isChecked();
            localPort = parseIntOr(editLocalPort.getText().toString().trim(), 7000);
            if (!isValidPort(localPort)) {
                setStatus("Host: local UDP port must be 1..65535");
                return;
            }

            String secret = (editSecret != null) ? editSecret.getText().toString().trim() : "";
            if (secret.isEmpty()) {
                setStatus("Host: secret word is required");
                return;
            }
            if (secret.contains(":")) {
                setStatus("Host: secret cannot contain ':'");
                return;
            }

            setStatus("Discovering public endpoint...");
            new Thread(() -> {
                try {
                    String mapped = NativeBridge.nativeStunMappedAddress(localPort);
                    if (mapped == null || mapped.trim().isEmpty() || !mapped.contains(":")) {
                        throw new Exception("STUN failed");
                    }
                    HostPort hp = parseHostPort(mapped);
                    String pubIp = hp.host;
                    int pubPort = hp.port;
                    if (pubIp.isEmpty() || pubPort < 1 || pubPort > 65535) throw new Exception("STUN returned invalid port");

                    String hostPort = canonicalizeHostPortForShare(pubIp, pubPort);
                    String conn = hostPort + ":" + secret;
                    connectionString = conn;
                    sharedSecret = secret;
                    lastHostConnectionString = conn;

                    prefs.edit()
                            .putBoolean(PREF_NETPLAY_ENABLED, true)
                            .putBoolean(PREF_SHOW_ONSCREEN_CONTROLS, showOnscreenControls)
                            .putInt(PREF_LOCAL_PORT, localPort)
                            .putString(PREF_CONNECTION_STRING, connectionString)
                            .putString(PREF_SHARED_SECRET, sharedSecret)
                            .putInt(PREF_NETPLAY_ROLE, 1)
                            .putString(PREF_REMOTE_HOST, "")
                            .putInt(PREF_REMOTE_PORT, 0)
                            .apply();

                    runOnUiThread(() -> {
                        editConnectionCode.setText(connectionString);
                        copyToClipboard(this, "Connection string", connectionString);
                        toastCopied();
                        netplayRole = 1;
                        switchNetplay.setChecked(true);
                        applyConnectionUiState(ConnectionUiState.HOST_READY);
                        setStatus("");
                        updateStartButtonEnabled();
                    });
                } catch (Exception e) {
                    runOnUiThread(() -> setStatus("Host failed: " + e.getMessage()));
                }
            }, "StunHostInfo").start();
        });

        btnJoinConnection.setOnClickListener(v -> {
            if (connectionUiState == ConnectionUiState.IDLE || connectionUiState == ConnectionUiState.HOST_READY) {
                // First press enters Join mode.
                netplayRole = 2;
                applyConnectionUiState(ConnectionUiState.JOIN_INPUT);
                setStatus("Paste the connection string");
                updateStartButtonEnabled();
                return;
            }

            showOnscreenControls = switchOnscreenControls.isChecked();
            localPort = parseIntOr(editLocalPort.getText().toString().trim(), 7000);
            connectionString = editConnectionCode.getText().toString().trim();
            if (!isValidPort(localPort)) {
                setStatus("Join: local UDP port must be 1..65535");
                return;
            }
            if (connectionString.isEmpty()) {
                setStatus("Paste the connection string from Player 1");
                return;
            }

            setStatus("Parsing connection string...");
            new Thread(() -> {
                try {
                    ParsedConnectionString info = parseConnectionString(connectionString);

                    // Join: the secret comes from the connection string.
                    // If the user typed something in the secret field, we overwrite it to match.
                    sharedSecret = info.secret;

                    // Keep endpoints consistent across devices: always use the public endpoint from the code.
                    final String finalHost = info.host;
                    final int finalPort = info.port;
                    if (finalHost == null || finalHost.isEmpty() || finalPort < 1 || finalPort > 65535) {
                        throw new Exception("Invalid host endpoint");
                    }

                    prefs.edit()
                            .putBoolean(PREF_NETPLAY_ENABLED, true)
                            .putBoolean(PREF_SHOW_ONSCREEN_CONTROLS, showOnscreenControls)
                            .putInt(PREF_LOCAL_PORT, localPort)
                            .putString(PREF_CONNECTION_STRING, connectionString)
                            .putString(PREF_SHARED_SECRET, sharedSecret)
                            .putInt(PREF_NETPLAY_ROLE, 2)
                            .putString(PREF_REMOTE_HOST, finalHost)
                            .putInt(PREF_REMOTE_PORT, finalPort)
                            .apply();

                    runOnUiThread(() -> {
                        netplayRole = 2;
                        resolvedRemoteHost = finalHost;
                        resolvedRemotePort = finalPort;
                        if (editSecret != null) editSecret.setText(sharedSecret);
                        switchNetplay.setChecked(true);
                        applyConnectionUiState(ConnectionUiState.JOIN_READY);
                        setStatus("");
                        updateStartButtonEnabled();
                    });
                } catch (Exception e) {
                    runOnUiThread(() -> {
                        setStatus("Join failed: " + e.getMessage());
                        applyConnectionUiState(ConnectionUiState.JOIN_INPUT);
                        updateStartButtonEnabled();
                    });
                }
            }, "JoinConnection").start();
        });

        if (btnCopyConnectionIcon != null) {
            btnCopyConnectionIcon.setOnClickListener(v -> {
                if (connectionString == null || connectionString.trim().isEmpty()) return;
                copyToClipboard(this, "Connection string", connectionString);
                toastCopied();
            });
        }

        btnCancelConnection.setOnClickListener(v -> {
            prefs.edit()
                    .putString(PREF_CONNECTION_STRING, "")
                    .putString(PREF_SHARED_SECRET, "")
                    .putString(PREF_CONNECTION_CODE, "")
                    .putInt(PREF_NETPLAY_ROLE, 1)
                    .putString(PREF_REMOTE_HOST, "")
                    .putInt(PREF_REMOTE_PORT, 0)
                    .apply();
            connectionString = "";
            sharedSecret = "";
            resolvedRemoteHost = "";
            resolvedRemotePort = 0;
            lastHostConnectionString = "";
            editConnectionCode.setText("");
            applyConnectionUiState(ConnectionUiState.IDLE);
            setStatus("");
            updateStartButtonEnabled();
        });
    }

    private void applyConfigTheme() {
        if (configRoot != null) {
            configRoot.setBackgroundColor(UiStyle.configBackground());
        }

        int text = UiStyle.configText();
        int hint = Color.argb(160, Color.red(text), Color.green(text), Color.blue(text));

        if (configRoot instanceof android.view.ViewGroup) {
            applyTextThemeRecursive((android.view.ViewGroup) configRoot, text, hint);
        }

        if (txtStatus != null) {
            txtStatus.setTextColor(text);
        }
    }

    private void applyTextThemeRecursive(android.view.ViewGroup root, int textColor, int hintColor) {
        for (int i = 0; i < root.getChildCount(); i++) {
            View v = root.getChildAt(i);
            if (v instanceof android.view.ViewGroup) {
                applyTextThemeRecursive((android.view.ViewGroup) v, textColor, hintColor);
            }
            if (v instanceof TextView) {
                ((TextView) v).setTextColor(textColor);
            }
            if (v instanceof EditText) {
                ((EditText) v).setHintTextColor(hintColor);
            }
        }
    }

    private void applyInteractiveTheme() {
        applyButtonTheme(btnPickRom);
        applyButtonTheme(btnStart);
        applyButtonTheme(btnStartConnection);
        applyButtonTheme(btnJoinConnection);
        applyButtonTheme(btnCancelConnection);

        applySwitchTheme(switchNetplay);
        applySwitchTheme(switchOnscreenControls);
        applySwitchTheme(switchShowSaveButton);
    }

    private void applyButtonTheme(Button b) {
        if (b == null) return;
        final int enabledBg = UiStyle.ACCENT_2;      // dark purple
        // Slightly lighter than PRIMARY_BASE for a more "disabled" feel.
        final int disabledBg = 0xFFE6E3E5;
        // Enabled buttons should read clearly.
        final int enabledText = 0xFFFFFFFF;
        // Disabled text should match the previous disabled background color.
        final int disabledText = UiStyle.PRIMARY_BASE;

        boolean en = b.isEnabled();
        b.setTextColor(en ? enabledText : disabledText);

        try {
            if (Build.VERSION.SDK_INT >= 21) {
                int[][] states = new int[][]{
                        new int[]{android.R.attr.state_enabled},
                        new int[]{-android.R.attr.state_enabled}
                };
                int[] colors = new int[]{enabledBg, disabledBg};
                b.setBackgroundTintList(new ColorStateList(states, colors));
            } else {
                b.setBackgroundColor(en ? enabledBg : disabledBg);
            }
        } catch (Exception ignored) {
        }
    }

    private void applySwitchTheme(Switch s) {
        if (s == null) return;
        try {
            if (Build.VERSION.SDK_INT < 21) return;

            final int checked = UiStyle.ACCENT_2;
            final int unchecked = UiStyle.SECONDARY_BASE;

            int[][] states = new int[][]{
                    new int[]{android.R.attr.state_checked},
                    new int[]{-android.R.attr.state_checked}
            };

            int[] thumb = new int[]{checked, unchecked};
            s.setThumbTintList(new ColorStateList(states, thumb));

            int checkedTrack = (checked & 0x00FFFFFF) | 0x66000000;
            int uncheckedTrack = (unchecked & 0x00FFFFFF) | 0x55000000;
            int[] track = new int[]{checkedTrack, uncheckedTrack};
            s.setTrackTintList(new ColorStateList(states, track));
        } catch (Exception ignored) {
        }
    }

    private void updateNetplayUi(boolean enabled) {
        netplayEnabled = enabled;
        btnStartConnection.setEnabled(enabled);
        btnJoinConnection.setEnabled(enabled);

        applyInteractiveTheme();

        if (!enabled) {
            applyConnectionUiState(ConnectionUiState.IDLE);
        }

        updateStartButtonEnabled();
    }

    private void updateStartButtonEnabled() {
        boolean enabled = coreReady && corePath != null && !corePath.isEmpty();

        String rp = "";
        if (editRomPath != null) rp = editRomPath.getText().toString().trim();
        enabled = enabled && rp != null && !rp.isEmpty();

        int p = parseIntOr(editLocalPort != null ? editLocalPort.getText().toString().trim() : "", 7000);
        enabled = enabled && isValidPort(p);

        if (switchNetplay != null && switchNetplay.isChecked()) {
            // Config is considered "established" when it's ready to start the game with netplay parameters.
            enabled = enabled && (connectionUiState == ConnectionUiState.HOST_READY || connectionUiState == ConnectionUiState.JOIN_READY);

            String secret = (editSecret != null) ? editSecret.getText().toString().trim() : "";
            enabled = enabled && secret != null && !secret.isEmpty();
        }

        if (btnStart != null) btnStart.setEnabled(enabled);

        applyInteractiveTheme();
    }

    private void applyConnectionUiState(ConnectionUiState st) {
        connectionUiState = st;

        boolean showNetplay = (switchNetplay != null && switchNetplay.isChecked());
        if (!showNetplay) st = ConnectionUiState.IDLE;

        if (txtOr != null) txtOr.setVisibility(st == ConnectionUiState.IDLE ? View.VISIBLE : View.GONE);

        if (btnStartConnection != null) {
            btnStartConnection.setVisibility((st == ConnectionUiState.JOIN_INPUT || st == ConnectionUiState.JOIN_READY) ? View.GONE : View.VISIBLE);
        }
        if (btnJoinConnection != null) {
            btnJoinConnection.setVisibility((st == ConnectionUiState.HOST_READY || st == ConnectionUiState.JOIN_READY) ? View.GONE : View.VISIBLE);
            btnJoinConnection.setText((st == ConnectionUiState.JOIN_INPUT || st == ConnectionUiState.JOIN_READY) ? "ACCEPT" : "Join connection");
        }

        if (editConnectionCode != null) {
            editConnectionCode.setVisibility((st == ConnectionUiState.JOIN_INPUT || st == ConnectionUiState.JOIN_READY) ? View.VISIBLE : View.GONE);
            editConnectionCode.setEnabled(st != ConnectionUiState.JOIN_READY);
        }

        boolean showHostWaiting = (st == ConnectionUiState.HOST_READY && lastHostConnectionString != null && !lastHostConnectionString.isEmpty());
        if (hostWaitingRow != null) hostWaitingRow.setVisibility(showHostWaiting ? View.VISIBLE : View.GONE);
        if (txtHostWaiting != null) txtHostWaiting.setText(showHostWaiting ? ("Share: " + lastHostConnectionString) : "");
        if (btnCopyConnectionIcon != null) btnCopyConnectionIcon.setVisibility(showHostWaiting ? View.VISIBLE : View.GONE);

        if (txtJoinTarget != null) {
            if (st == ConnectionUiState.JOIN_READY && resolvedRemoteHost != null && !resolvedRemoteHost.isEmpty() && resolvedRemotePort > 0) {
                txtJoinTarget.setText("Will connect to " + resolvedRemoteHost + ":" + resolvedRemotePort + " (secret set)");
                txtJoinTarget.setVisibility(View.VISIBLE);
            } else {
                txtJoinTarget.setVisibility(View.GONE);
            }
        }

        if (btnCancelConnection != null) {
            btnCancelConnection.setVisibility(st == ConnectionUiState.IDLE ? View.GONE : View.VISIBLE);
        }

        applyInteractiveTheme();
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


    private static String sanitizeFilename(String name) {
        if (name == null) return "";
        String t = name.trim();
        if (t.isEmpty()) return "";
        // Remove path separators and other problematic characters.
        t = t.replace("/", "_").replace("\\\\", "_");
        t = t.replace(":", "_").replace("*", "_").replace("?", "_");
        t = t.replace("\"", "_").replace("<", "_").replace(">", "_").replace("|", "_");
        // Avoid extremely long filenames.
        if (t.length() > 80) t = t.substring(0, 80);
        return t;
    }

    private String displayNameFromUri(Uri uri) {
        if (uri == null) return "";
        try {
            android.database.Cursor c = getContentResolver().query(uri, new String[]{android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (c != null) {
                try {
                    if (c.moveToFirst()) {
                        int idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                        if (idx >= 0) {
                            String name = c.getString(idx);
                            return (name != null) ? name : "";
                        }
                    }
                } finally {
                    c.close();
                }
            }
        } catch (Exception ignored) {
        }
        return "";
    }

    private static File uniqueFile(File dir, String filename) {
        if (dir == null) return null;
        dir.mkdirs();
        File f = new File(dir, filename);
        if (!f.exists()) return f;
        String base = filename;
        String ext = "";
        int dot = filename.lastIndexOf('.');
        if (dot > 0 && dot + 1 < filename.length()) {
            base = filename.substring(0, dot);
            ext = filename.substring(dot);
        }
        for (int i = 2; i < 1000; i++) {
            File g = new File(dir, base + "_" + i + ext);
            if (!g.exists()) return g;
        }
        return f;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;

        try {
            if (requestCode == REQ_PICK_ROM) {
                String name = sanitizeFilename(displayNameFromUri(uri));
                if (name.isEmpty()) name = "rom.sfc";
                File dir = new File(getFilesDir(), "roms");
                File dst = uniqueFile(dir, name);
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
