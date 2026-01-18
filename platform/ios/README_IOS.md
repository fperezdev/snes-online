# iOS (UIKit) app

This repo currently builds Windows + Android in this workspace. iOS requires **macOS + Xcode**.

## File formats
- Android distributable: `.apk`
- iOS distributable: `.ipa` (signed)

An `.ipa` can only be produced by Xcode (or `xcodebuild`) **with valid Apple code signing**.

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
- `cores/snes9x_libretro_ios.dylib`

You can change the default in `IOSConfigViewController.swift`.

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
