Bundled libretro cores

This folder is packaged into the APK as assets.
On first run, the app can extract a core to internal storage (files/cores/) so it can be loaded via dlopen.

This project currently bundles (arm64-v8a):
- snes9x_libretro_android.so (downloaded from buildbot.libretro.com during build)
