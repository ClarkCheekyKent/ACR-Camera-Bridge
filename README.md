# ACR Camera Bridge

A UEVR plugin for **Assetto Corsa Rally** that fixes scenery disappearing when you look left or right in VR.

ACR can use its game-camera direction for visibility while UEVR renders your headset direction. This plugin updates the game camera cache to follow your head, then supplies the clean game-camera rotation to UEVR so head tracking is applied only once. It also handles camera switches.

This fix is specific to ACR, not a universal Unreal Engine culling fix.

## Install and use

1. Download **[ACR-Camera-Bridge-v2.3.0.zip](https://github.com/ClarkCheekyKent/ACR-Camera-Bridge/releases/latest/download/ACR-Camera-Bridge-v2.3.0.zip)**.
2. Close ACR.
3. Extract the ZIP into `%APPDATA%\UnrealVRMod\acr\`, merging its `plugins` and `scripts` folders with the existing ones.

Restart ACR and inject UEVR. Leave UEVR head tracking enabled; manually freezing rotation is not required.

Open **ACR Camera Bridge** in the Lua panel to see **Active**, **Disabled**, **Waiting**, or **Error**. Use the enable checkbox or **F9** to toggle it. Settings save automatically. Active confirms recent matched stereo updates, not the visibility of individual objects.

Built and tested against UEVR **nightly01143 / API 2.39.0**. Game or UEVR updates may require an update to the plugin.

## Build

Requires Windows x64, Visual Studio 2022 C++ tools, CMake, and the UEVR source matching your injector. The tested UEVR commit is `4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d`.

```bat
git clone https://github.com/praydog/UEVR.git C:\src\UEVR
git -C C:\src\UEVR checkout 4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d
cd ACR_UEVR_CameraBridge
build.bat C:\src\UEVR
```

Output: `ACR_UEVR_CameraBridge/build/Release/ACR_CameraBridge.dll`.

See [build and test details](ACR_UEVR_CameraBridge/README.md) for automated tests and the install script.

## Show your appreciation

If ACR Camera Bridge has helped keep scenery visible when you look around in VR, please consider donating. Your support is appreciated!

[![Donate with Stripe](https://img.shields.io/badge/Donate_with_Stripe-635BFF?style=for-the-badge&logo=stripe&logoColor=white)](https://donate.stripe.com/fZufZg5am97b0b12IO8ww00)
