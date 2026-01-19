# Android (APK)

This folder contains a minimal Android app that **always launches the configuration screen first** (`ConfigActivity`), then starts gameplay (`GameActivity`).

## Prerequisites

- Android Studio (recommended)
- Android SDK Platform 34
- Android NDK (r26+ recommended)
- CMake 3.22.1 (Android Studio can install this)
- JDK 17 (Android Studio ships an embedded JDK)

## Build (Android Studio)

1. Open this folder in Android Studio: `platform/android/`
2. Let Android Studio install missing SDK/NDK/CMake components if prompted
3. Build APK:
   - **Build → Build Bundle(s) / APK(s) → Build APK(s)**

The debug APK will be at:

- `platform/android/app/build/outputs/apk/debug/app-debug.apk`

## Build (command line)

1. Ensure `JAVA_HOME` points to a JDK 17 install (or Android Studio’s embedded JDK).
2. Ensure Android SDK/NDK are installed and discoverable (Android Studio will normally generate `local.properties`).
3. Run:

- `platform/android/gradlew.bat :app:assembleRelease`

### Windows note (MAX_PATH)

When GGPO is enabled, the Android native build uses CMake FetchContent to download/build GGPO. On Windows, very deep build paths can hit the 260-character path limit.

- The build passes `-DFETCHCONTENT_BASE_DIR` to a short directory by default (`C:/_snesonline_fc`).
- You can override it with an environment variable:
   - `SNESONLINE_FETCHCONTENT_BASE=C:/_fc`

### Release APK signing (installable)

`assembleRelease` must be **signed** to be installable.

- If `platform/android/keystore.properties` exists, the build signs with your **release key**.
- If it does not exist, the build **falls back to the debug key** so the release APK is still installable for local testing.

To create a proper release key:

1. Generate a keystore (example):
   - `keytool -genkeypair -v -keystore snesonline-release.jks -keyalg RSA -keysize 2048 -validity 10000 -alias snesonline`
2. Create `platform/android/keystore.properties`:
   - `storeFile=snesonline-release.jks`
   - `storePassword=...`
   - `keyAlias=snesonline`
   - `keyPassword=...`

Do not commit the keystore or properties file (they are gitignored).

## App flow

- Launches to **ConfigActivity** (always).
- The APK bundles a default SNES core (Snes9x) in assets during the build.
- In ConfigActivity you can:
   - **Use Bundled Core (Snes9x)**: extracts the bundled core to app storage and selects it.
   - **Download/Update Core (Snes9x)**: downloads the core from buildbot.libretro.com and installs it to app storage.
- You pick a ROM via the system file picker.
- The app copies the ROM into private app storage and starts **GameActivity**.

## Netplay (Direct Connect, UDP lockstep)

Android uses a lightweight **UDP lockstep** exchange (no rollback). The recommended flow is **Direct Connect** using a shareable **Connection Code**.

In **ConfigActivity**, set:
- **Local UDP Port** (default `7000`)

Host (Player 1):
- Tap **Get Connection Info (Host)** to discover the public UDP mapping via **STUN**.
- Share the generated **Connection Code**.

Join (Player 2):
- Paste the code and tap **Join Connection**.

Connection happens at game start and only exists while the game is running.

### Internet play

- Recommended: use a VPN overlay (Tailscale/ZeroTier) on both devices.
- Port-forwarding: works best when the Android device is on Wi‑Fi behind a router you control. Forward the chosen **local UDP port** to that device.
- Cellular networks and carrier-grade NAT often block inbound UDP; VPN overlay is usually the practical option there.
