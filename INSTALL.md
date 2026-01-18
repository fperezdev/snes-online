# Portable ZIP (Windows)

These are the steps to create a **portable ZIP** containing a single folder with:
- `snesonline_win.exe`
- `snesonline_config.exe` (network/settings tool)
- required runtime DLLs
- `cores/` (e.g. `snes9x_libretro.dll` if present)
- `roms/`

## Prereqs
- CMake
- Visual Studio 2022 Build Tools (Desktop development with C++)

## Build + Package

From the repo root:

```powershell
# Configure (enable the Windows app)
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build_vs -G "Visual Studio 17 2022" -A x64 -DSNESONLINE_BUILD_WINDOWS_APP=ON

# Build Release
& "C:\Program Files\CMake\bin\cmake.exe" --build build_vs --config Release -j

# Create the portable ZIP (contains a top-level folder)
Push-Location build_vs
& "C:\Program Files\CMake\bin\cpack.exe" -G ZIP
Pop-Location
```

Portable netplay note:
- Netplay configuration is room-only (Room Server URL + Room Code + Room Password + Local UDP Port).
- Connection happens at game start and only exists while the game is running.

The ZIP is typically created in:
- `build_vs\snes-online-*-win64-portable.zip`
