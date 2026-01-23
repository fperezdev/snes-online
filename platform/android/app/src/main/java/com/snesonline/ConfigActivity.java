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
import android.util.Base64;
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
import java.security.MessageDigest;

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

    private String connectionCode = "";

    private enum ConnectionUiState {
        IDLE,
        HOST_READY,
        JOIN_INPUT,
        JOIN_READY
    }

    private ConnectionUiState connectionUiState = ConnectionUiState.IDLE;

    private String lastHostPublicEndpoint = ""; // ip:port (public mapped)

    private static final class ConnInfo {
        String publicIp = "";
        int publicPort = 0;
        String lanIp = "";
        int lanPort = 0;
    }

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

    private static String encodeConnectionCode(String publicIp, int publicPort, String lanIp, int lanPort) {
        // v=2 includes a signature so small changes (like port) noticeably change the code.
        String p = "v=2";
        if (publicIp != null && !publicIp.isEmpty() && publicPort > 0) {
            String host = publicIp;
            // Canonicalize IPv6 endpoints to bracket form.
            if (host.contains(":") && !host.startsWith("[")) host = "[" + host + "]";
            p += "&pub=" + host + ":" + publicPort;
        }
        if (lanIp != null && !lanIp.isEmpty() && lanPort > 0) {
            p += "&lan=" + lanIp + ":" + lanPort;
        }

        String sig = shortSigB64Url(p);
        if (!sig.isEmpty()) p += "&sig=" + sig;

        byte[] data = p.getBytes(StandardCharsets.UTF_8);
        String b64 = Base64.encodeToString(data, Base64.URL_SAFE | Base64.NO_WRAP);
        while (b64.endsWith("=")) b64 = b64.substring(0, b64.length() - 1);
        return "SNO2:" + b64;
    }

    private static String shortSigB64Url(String payload) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] h = md.digest(payload.getBytes(StandardCharsets.UTF_8));
            // 12 bytes => 16 chars base64url-ish (without padding), enough to visibly change.
            int n = Math.min(12, h.length);
            byte[] shortHash = new byte[n];
            System.arraycopy(h, 0, shortHash, 0, n);
            String b64 = Base64.encodeToString(shortHash, Base64.URL_SAFE | Base64.NO_WRAP);
            while (b64.endsWith("=")) b64 = b64.substring(0, b64.length() - 1);
            return b64;
        } catch (Exception ignored) {
            return "";
        }
    }

    private static ConnInfo decodeConnectionCode(String code) throws Exception {
        if (code == null) throw new Exception("Empty code");
        String t = code.trim();
        String prefix;
        if (t.startsWith("SNO2:")) prefix = "SNO2:";
        else if (t.startsWith("SNO1:")) prefix = "SNO1:";
        else throw new Exception("Invalid code (missing SNO prefix)");

        String b64 = t.substring(prefix.length()).trim();
        if (b64.isEmpty()) throw new Exception("Invalid code");
        // Restore padding.
        int mod = b64.length() % 4;
        if (mod == 2) b64 += "==";
        else if (mod == 3) b64 += "=";
        else if (mod != 0) throw new Exception("Invalid code length");

        byte[] data = Base64.decode(b64, Base64.URL_SAFE);
        String payload = new String(data, StandardCharsets.UTF_8);
        ConnInfo out = new ConnInfo();
        String[] parts = payload.split("&");
        for (String part : parts) {
            int eq = part.indexOf('=');
            if (eq <= 0) continue;
            String k = part.substring(0, eq);
            String v = part.substring(eq + 1);
            if (k.equals("pub")) {
                HostPort hp = parseHostPort(v);
                out.publicIp = hp.host;
                out.publicPort = hp.port;
            } else if (k.equals("lan")) {
                String[] hp = v.split(":");
                if (hp.length == 2) {
                    out.lanIp = hp[0];
                    out.lanPort = parseIntOr(hp[1], 0);
                }
            }
        }
        if (out.publicIp == null) out.publicIp = "";
        if (out.lanIp == null) out.lanIp = "";
        if (out.publicIp.isEmpty() || out.publicPort < 1 || out.publicPort > 65535) {
            throw new Exception("Code is missing a valid public endpoint");
        }
        if (!out.lanIp.isEmpty() && (out.lanPort < 1 || out.lanPort > 65535)) {
            // If LAN IP exists but port doesn't, ignore LAN hint.
            out.lanIp = "";
            out.lanPort = 0;
        }
        return out;
    }

    private static void copyToClipboard(Activity a, String label, String text) {
        ClipboardManager cm = (ClipboardManager) a.getSystemService(CLIPBOARD_SERVICE);
        if (cm == null) return;
        cm.setPrimaryClip(ClipData.newPlainText(label, text));
    }

    private static String makeInviteLink(String code) {
        if (code == null) code = "";
        return new Uri.Builder()
                .scheme("snesonline")
                .authority("join")
                .appendQueryParameter("code", code)
                .build()
                .toString();
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

        connectionCode = prefs.getString(PREF_CONNECTION_CODE, "");

        switchNetplay.setChecked(netplayEnabled);
        switchOnscreenControls.setChecked(showOnscreenControls);
        if (switchShowSaveButton != null) switchShowSaveButton.setChecked(showSaveButton);

        applyConfigTheme();

        editLocalPort.setText(String.valueOf(localPort));

        if (connectionCode != null && !connectionCode.isEmpty()) editConnectionCode.setText(connectionCode);

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
                if (connectionUiState != ConnectionUiState.HOST_READY || connectionCode == null || connectionCode.trim().isEmpty()) {
                    setStatus("Press Start connection first");
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
            if (savedHost == null || savedHost.isEmpty() || savedPort < 1 || savedPort > 65535) {
                setStatus("Join endpoint missing. Press Join connection again.");
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

                    String lanIp = localLanIpv4BestEffort();
                    String code = encodeConnectionCode(pubIp, pubPort, lanIp, localPort);
                    connectionCode = code;
                    lastHostPublicEndpoint = pubIp + ":" + pubPort;

                    prefs.edit()
                            .putBoolean(PREF_NETPLAY_ENABLED, true)
                            .putBoolean(PREF_SHOW_ONSCREEN_CONTROLS, showOnscreenControls)
                            .putInt(PREF_LOCAL_PORT, localPort)
                            .putString(PREF_CONNECTION_CODE, connectionCode)
                            .putInt(PREF_NETPLAY_ROLE, 1)
                            .putString(PREF_REMOTE_HOST, "")
                            .putInt(PREF_REMOTE_PORT, 0)
                            .apply();

                    runOnUiThread(() -> {
                        editConnectionCode.setText(connectionCode);
                        copyToClipboard(this, "Invite link", makeInviteLink(connectionCode));
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
                setStatus("Paste the code");
                updateStartButtonEnabled();
                return;
            }

            showOnscreenControls = switchOnscreenControls.isChecked();
            localPort = parseIntOr(editLocalPort.getText().toString().trim(), 7000);
            connectionCode = editConnectionCode.getText().toString().trim();
            if (!isValidPort(localPort)) {
                setStatus("Join: local UDP port must be 1..65535");
                return;
            }
            if (connectionCode.isEmpty()) {
                setStatus("Paste the connection code from Player 1");
                return;
            }

            setStatus("Parsing code...");
            new Thread(() -> {
                try {
                    ConnInfo info = decodeConnectionCode(connectionCode);

                    // Keep endpoints consistent across devices: always use the public endpoint from the code.
                    final String finalHost = info.publicIp;
                    final int finalPort = info.publicPort;
                    if (finalHost == null || finalHost.isEmpty() || finalPort < 1 || finalPort > 65535) {
                        throw new Exception("Invalid host endpoint");
                    }

                    prefs.edit()
                            .putBoolean(PREF_NETPLAY_ENABLED, true)
                            .putBoolean(PREF_SHOW_ONSCREEN_CONTROLS, showOnscreenControls)
                            .putInt(PREF_LOCAL_PORT, localPort)
                            .putString(PREF_CONNECTION_CODE, connectionCode)
                            .putInt(PREF_NETPLAY_ROLE, 2)
                            .putString(PREF_REMOTE_HOST, finalHost)
                            .putInt(PREF_REMOTE_PORT, finalPort)
                            .apply();

                    runOnUiThread(() -> {
                        netplayRole = 2;
                        resolvedRemoteHost = finalHost;
                        resolvedRemotePort = finalPort;
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
                if (connectionCode == null || connectionCode.trim().isEmpty()) return;
                copyToClipboard(this, "Invite link", makeInviteLink(connectionCode));
                toastCopied();
            });
        }

        btnCancelConnection.setOnClickListener(v -> {
            prefs.edit()
                    .putString(PREF_CONNECTION_CODE, "")
                    .putInt(PREF_NETPLAY_ROLE, 1)
                    .putString(PREF_REMOTE_HOST, "")
                    .putInt(PREF_REMOTE_PORT, 0)
                    .apply();
            connectionCode = "";
            resolvedRemoteHost = "";
            resolvedRemotePort = 0;
            lastHostPublicEndpoint = "";
            editConnectionCode.setText("");
            applyConnectionUiState(ConnectionUiState.IDLE);
            setStatus("");
            updateStartButtonEnabled();
        });

        // Handle snesonline://join?code=... deep links.
        handleInviteIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleInviteIntent(intent);
    }

    private void handleInviteIntent(Intent intent) {
        if (intent == null) return;
        Uri data = intent.getData();
        if (data == null) return;
        if (!"snesonline".equalsIgnoreCase(data.getScheme())) return;
        if (!"join".equalsIgnoreCase(data.getHost())) return;
        String code = data.getQueryParameter("code");
        if (code == null) code = "";
        code = code.trim();
        if (code.isEmpty()) return;

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        // Best-effort: ensure netplay is enabled and we are in join flow.
        netplayEnabled = true;
        netplayRole = 2;
        if (switchNetplay != null) switchNetplay.setChecked(true);

        if (editConnectionCode != null) editConnectionCode.setText(code);
        connectionCode = code;

        localPort = parseIntOr(editLocalPort.getText().toString().trim(), 7000);
        if (!isValidPort(localPort)) localPort = 7000;

        try {
            ConnInfo info = decodeConnectionCode(code);
            resolvedRemoteHost = info.publicIp;
            resolvedRemotePort = info.publicPort;

            prefs.edit()
                    .putBoolean(PREF_NETPLAY_ENABLED, true)
                    .putInt(PREF_LOCAL_PORT, localPort)
                    .putString(PREF_CONNECTION_CODE, connectionCode)
                    .putInt(PREF_NETPLAY_ROLE, 2)
                    .putString(PREF_REMOTE_HOST, resolvedRemoteHost)
                    .putInt(PREF_REMOTE_PORT, resolvedRemotePort)
                    .apply();

            applyConnectionUiState(ConnectionUiState.JOIN_READY);
            setStatus("");
        } catch (Exception e) {
            applyConnectionUiState(ConnectionUiState.JOIN_INPUT);
            setStatus("Invalid invite link: " + e.getMessage());
        }

        updateStartButtonEnabled();
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

        boolean showHostWaiting = (st == ConnectionUiState.HOST_READY && lastHostPublicEndpoint != null && !lastHostPublicEndpoint.isEmpty());
        if (hostWaitingRow != null) hostWaitingRow.setVisibility(showHostWaiting ? View.VISIBLE : View.GONE);
        if (txtHostWaiting != null) txtHostWaiting.setText(showHostWaiting ? ("The other player should connect at " + lastHostPublicEndpoint) : "");
        if (btnCopyConnectionIcon != null) btnCopyConnectionIcon.setVisibility(showHostWaiting ? View.VISIBLE : View.GONE);

        if (txtJoinTarget != null) {
            if (st == ConnectionUiState.JOIN_READY && resolvedRemoteHost != null && !resolvedRemoteHost.isEmpty() && resolvedRemotePort > 0) {
                txtJoinTarget.setText("Will connect to " + resolvedRemoteHost + ":" + resolvedRemotePort);
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
