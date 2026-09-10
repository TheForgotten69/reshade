ReShade
=======

This is a generic post-processing injector for games and video software. It exposes an automated way to access both frame color and depth information and a custom shader language called ReShade FX to write effects like ambient occlusion, depth of field, color correction and more which work everywhere.

ReShade can optionally load **add-ons**, DLLs that make use of the ReShade API to extend functionality of both ReShade and/or the application ReShade is being applied to. To get started on how to write your own add-on, check out the [API reference](REFERENCE.md).

The ReShade FX shader compiler contained in this repository is standalone, so can be integrated into other projects as well. Simply add all `source/effect_*.*` files to your project and use it similar to the [fxc example](tools/fxc.cpp).

## Building

### Windows

You'll need Visual Studio 2017 or higher to build ReShade. And Python in the PATH environment variable for the `glad` dependency to build.

1. Clone this repository including all Git submodules\
```git clone --recurse-submodules https://github.com/crosire/reshade```
2. Open the Visual Studio solution
3. Select either the `32-bit` or `64-bit` target platform and build the solution.\
   This will build ReShade and all dependencies. To build the setup tool, first build the `Release` configuration for both `32-bit` and `64-bit` targets and only afterwards build the `Release Setup` configuration (does not matter which target is selected then).

### Linux

The native Linux build currently supports x86-64 Vulkan applications running on Wayland. It requires CMake, a C++17 compiler, Python, pkg-config, and the development packages for Wayland client, Wayland protocols, xkbcommon, and Fontconfig.

```sh
cmake -S . -B build-linux -DRESHADE_VERSION=<major.minor.patch>
cmake --build build-linux
cmake --install build-linux --prefix "$HOME/.local"
```

The version option is only needed when the source tree has no reachable ReShade version tag. The install registers an opt-in Vulkan implicit layer. Launch an application with `RESHADE_ENABLE=1`, for example:

```sh
RESHADE_ENABLE=1 /path/to/application
```

Configuration is stored below the XDG config directory (normally `~/.config/reshade`), and logs below `$XDG_DATA_HOME/reshade/logs` (normally `~/.local/share/reshade/logs`). Generic Depth and Effect Runtime Sync are built in. Native Linux `.addon` and `.addon64` libraries are loaded from the user's XDG data directory and the installation's share directory; user copies take precedence by filename. Setting `[ADDON] AddonPath` explicitly restricts discovery to that directory. Windows add-on binaries are listed as incompatible and are not loaded.

Configure with `-DRESHADE_BUILD_LINUX_ADDON_EXAMPLES=ON` to build the portable example add-ons. Generic Depth and Effect Runtime Sync are also built as standalone examples, but are not installed again because they are built into the host. Video capture requires FFmpeg development packages, and ray tracing requires DXC. These examples retain their individual behavior: Texture Replace reads `texreplace` beside the executable; shader/texture dumps and shader replacements use the user's ReShade data directory.

To produce a distributable Linux archive with the optional example add-ons, configure that option and run `tools/package_linux.sh <build-dir> <official-reshade-shaders-checkout> [output-dir] [version]`. The archive installer installs no optional add-ons unless the user selects them interactively (or sets `INSTALL_ADDONS` for an unattended installation), so a fresh install does not activate development tools by default.

The initial Linux port does not support Windows add-on binaries, OpenGL injection, VR overlays, gamepad navigation, screenshot sounds or post-save commands, or automatic update checks. FFmpeg-dependent add-ons may require compatible system libraries when the application bundles its own dependencies.

On Linux/Vulkan, Generic Depth can infer normal versus reversed depth from matching clear values and depth-test comparisons for the selected buffer. It observes at most 600 rendered frames and requires 120 consecutive matching observations before applying a result. Missing or conflicting evidence leaves the setting unchanged. No GPU readback is used. An explicit `RESHADE_DEPTH_INPUT_IS_REVERSED` definition takes precedence; automatic results are saved in the active preset, so use separate presets for games with different depth conventions. DisplayDepth's live-preview controls do not change other effects' preprocessor definitions.

Automatic depth-convention detection is best-effort and can be disabled with `AutoDetectReversedDepth=0` in the `[DEPTH]` section of `ReShade.ini`. It applies at most once per runtime; removing a saved definition and restarting the application allows a new detection attempt. It does not detect logarithmic depth or other custom depth encodings.

Linux portability regression tests can be run without a game or compositor:

```sh
cmake -S . -B build-linux -DRESHADE_BUILD_LINUX_TESTS=ON
cmake --build build-linux --target reshade_linux_tests
ctest --test-dir build-linux --output-on-failure
```

A quick overview of what some of the source code files contain:

|File                                                                  |Description                                                            |
|----------------------------------------------------------------------|-----------------------------------------------------------------------|
|[dll_log.cpp](source/dll_log.cpp)                                     |Simple file logger implementation                                      |
|[dll_main.cpp](source/dll_main.cpp)                                   |Main entry point (and optional test application)                       |
|[dll_resources.cpp](source/dll_resources.cpp)                         |Access to DLL resource data (e.g. built-in shaders)                    |
|[effect_lexer.cpp](source/effect_lexer.cpp)                           |Lexical analyzer for C-like languages                                  |
|[effect_parser_stmt.cpp](source/effect_parser_stmt.cpp)               |Parser for the ReShade FX shader language                              |
|[effect_preprocessor.cpp](source/effect_preprocessor.cpp)             |C-like preprocessor implementation                                     |
|[hook.cpp](source/hook.cpp)                                           |Wrapper around MinHook which tracks associated function pointers       |
|[hook_manager.cpp](source/hook_manager.cpp)                           |Automatic hook installation based on DLL exports                       |
|[input.cpp](source/input.cpp)                                         |Keyboard and mouse input management                                    |
|[runtime.cpp](source/runtime.cpp)                                     |Core ReShade runtime including effect and preset management            |
|[runtime_gui.cpp](source/runtime_gui.cpp)                             |Overlay rendering and everything user interface related                |

## Contributing

Any contributions to the project are welcomed, it's recommended to use GitHub [pull requests](https://help.github.com/articles/using-pull-requests/).

## Feedback and Support

See the [ReShade Forum](https://reshade.me/forum) and [Discord](https://discord.gg/PrwndfH) server for feedback and support.

## License

ReShade is licensed under the terms of the [BSD 3-clause license](LICENSE.md).\
Some source code files are dual-licensed and are also available under the terms of the MIT license, when stated as such at the top of those files.
