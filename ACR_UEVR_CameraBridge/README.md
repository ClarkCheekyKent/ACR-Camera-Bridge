# ACR Camera Bridge

UEVR plugin for Assetto Corsa Rally. After the game updates its camera, the plugin publishes the head-facing orientation into the camera cache for visibility work. Before UEVR applies headset tracking, paired stereo callbacks restore the clean game-camera base, preventing double rotation and handling camera switches.

The Lua panel shows **Active**, **Disabled**, **Waiting**, or **Error**, with one enable checkbox. **F9** toggles the bridge. Active means a matched stereo update completed within the last two seconds; it does not measure scenery visibility. Settings save in `ACR_CameraBridge.ini`. Old experimental settings are ignored. F10, gain/FOV controls, alternate timing modes, and periodic pose logging have been removed.

## Build and test

Use Windows x64, Visual Studio 2022 C++ tools, CMake, and a UEVR checkout matching your injector. This version was built against nightly01143, API 2.39.0, commit `4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d`.

From this directory:

```bat
build.bat C:\src\UEVR
cmake -S . -B build -DBRIDGE_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Lua panel test also runs when the checkout contains `dependencies/lua/src`. Native tests mock the SDK boundary and exercise the real plugin callbacks; they cannot verify in-game rendering.

Close ACR, then run `install_example.bat "%APPDATA%\UnrealVRMod\acr"` to install the DLL and Lua panel. Restart ACR and inject the matching UEVR nightly. Leave UEVR headset tracking enabled. The plugin supplies a clean camera base automatically, including when Freeze Rotation is enabled.

The resolver derives camera layout from reflection and validates the UpdateCamera target before hooking it. A game update can make resolution fail; the panel then reports Error.
