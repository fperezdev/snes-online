# iOS (UIKit) app

This repo currently builds Windows + Android in this workspace. iOS requires **macOS + Xcode**.

## File formats
- Android distributable: `.apk`
- iOS distributable: `.ipa` (signed)

An `.ipa` can only be produced by Xcode (or `xcodebuild`) **with valid Apple code signing**.

If you don’t have a Mac, you can still build a signed `.ipa` using **GitHub Actions** (macOS runners) as long as you have:
- An Apple Developer account/team (for signing)
- A signing certificate (`.p12`) and a provisioning profile (`.mobileprovision`)

## What’s included here
- A lightweight iOS UIKit app scaffold (Config screen → Game screen)
- An Objective-C++ native bridge that reuses the same C++ core + UDP lockstep netplay used on Android
- Optional on-screen touch controls (toggle in Config)

## Requirements (on a Mac)
- Xcode (latest stable)
- CocoaPods not required
- Optional: **XcodeGen** (recommended) to generate an `.xcodeproj` from YAML

## Generate the Xcode project (recommended)
1. Install XcodeGen:
   - `brew install xcodegen`
2. From the repo root:
   - `cd platform/ios`
   - `xcodegen generate`
3. Open the generated project:
   - Open `platform/ios/SnesOnline.xcodeproj`

## Provide an iOS libretro core
The engine loads a libretro core via dynamic loading. You must add an iOS-built core binary to the app bundle.

Expected path inside the app bundle (default):
- `Resources/cores/snes9x_libretro_ios.dylib`

You can change the default in `IOSConfigViewController.swift`.

Recommended (CI): the GitHub Actions workflow can download and bundle the official prebuilt Snes9x iOS libretro core automatically (from libretro buildbot). If you disable core bundling, the app can install but core loading will fail at runtime.

## Build an IPA via GitHub Actions (no Mac required locally)
This repo includes a workflow:
- `.github/workflows/ios-ipa.yml`

It supports two modes:
- **Unsigned IPA** (no Apple paid membership required): intended for **AltStore / Sideloadly** signing with your Apple ID on Windows.
- **Signed IPA** (requires Apple code signing assets): uses your certificate + provisioning profile in GitHub Secrets.

### Free mode: build in CI, then sideload with AltStore/Sideloadly (Windows)
1. In GitHub, go to **Actions → iOS: Build IPA → Run workflow**.
2. Download the artifact named like `SnesOnline-unsigned.ipa`.
3. Install to your iPhone using one of these:
   - **Sideloadly** (Windows): installs an `.ipa` using your Apple ID (free Apple ID works). Apps typically expire in ~7 days and must be reinstalled.
   - **AltStore** (Windows): runs a small helper on your PC to install/refresh apps using your Apple ID.

Free Apple-ID limitations are controlled by Apple (not by this repo):
- App signatures typically expire after ~7 days.
- There are limits on the number of sideloaded apps.

Workflow option:
- `include_core` (default `true`): builds + bundles `snes9x_libretro_ios.dylib` into the app.

### Required repo secrets (SIGNED mode only)
Create these in GitHub: **Settings → Secrets and variables → Actions**

- `IOS_CERT_P12_BASE64`: base64 of your signing certificate `.p12`
- `IOS_CERT_PASSWORD`: password for the `.p12`
- `IOS_PROVISION_PROFILE_BASE64`: base64 of your `.mobileprovision`
- `IOS_PROFILE_NAME`: provisioning profile *Name* (the “Profile Name” shown in Apple Developer portal)
- `IOS_TEAM_ID`: your Apple Team ID
- `IOS_BUNDLE_ID`: must match the profile’s App ID, e.g. `com.snesonline.snesonline`
- `IOS_KEYCHAIN_PASSWORD`: any strong random string (used to create an ephemeral keychain in CI)

### Triggering builds
- Manual: Actions → “iOS: Build IPA” → Run workflow
- Tag: push a tag like `ios-v0.1.0` and the workflow will run

For tag builds (`ios-v*`), the workflow also attaches the generated `.ipa` to a **GitHub Release** so you can download it from the Releases page (in addition to Actions artifacts).

### Notes
- The workflow uses **manual signing**; your provisioning profile must match `IOS_BUNDLE_ID`.
- The build will succeed even if the iOS libretro core dylib is missing, but the app won’t be able to load a core at runtime.

## Run on device
- Select a real iOS device (recommended) or Simulator.
- Pick a ROM via the document picker.
- For netplay, set the same ports as Android/Windows lockstep mode.

## Build an IPA
In Xcode:
1. Set your **Signing & Capabilities** team.
2. Product → Archive
3. Distribute App → Ad Hoc/TestFlight

CLI (example):
- `xcodebuild -scheme SnesOnline -configuration Release -archivePath build/SnesOnline.xcarchive archive`
- Then export with an `ExportOptions.plist`.

Notes:
- The iOS build/sign step cannot be done on Windows.
