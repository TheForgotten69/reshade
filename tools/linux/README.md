# ReShade native Linux/Vulkan beta 2

Experimental x86-64 ReShade for Vulkan applications running under Wayland, X11, XWayland, and compatible Wine/Proton hosts.

## Install

Extract the archive and run:

```sh
./install.sh
```

The default prefix is `~/.local`; override it with `PREFIX=/custom/prefix ./install.sh`. The installer prints the full host-library, Vulkan-manifest, and shader/add-on paths when it finishes.
The installer always installs the opt-in Vulkan layer and standard ReShade shaders. It presents one numbered add-on menu; enter the numbers to install (for example, `1,6`), `all`, or press Enter for none. Choosing an add-on installs it, which makes it load automatically when ReShade is enabled.

For unattended installs, set `INSTALL_ADDONS` to `none`, `all`, or a comma-separated list such as:

```sh
INSTALL_ADDONS=fps_limit,texture_dump ./install.sh
```

Generic Depth and Effect Runtime Sync are built into the host and are not separate selections. This archive deliberately omits the FFmpeg Video Capture add-on, since its external codec dependency can conflict with applications that bundle their own FFmpeg.

Launch an application with:

```sh
RESHADE_ENABLE=1 /path/to/application
```

For a Steam game using a Proton build with native Wayland support, set:

```sh
RESHADE_ENABLE=1 PROTON_ENABLE_WAYLAND=1 %command%
```

In this mode Wine receives input on a Wayland connection of its own, so the overlay works but ReShade cannot block input from reaching the game. Use the default Proton setup (without `PROTON_ENABLE_WAYLAND`) if you need input blocking.

Configurations are created under `${XDG_CONFIG_HOME:-~/.config}/reshade`; logs and add-ons live below `${XDG_DATA_HOME:-~/.local/share}/reshade`.

## Shaders and add-ons

The archive already installs the official standard shader collection. To add another shader pack, copy its `.fx` and `.fxh` files into the `Shaders` directory below the shader path printed by the installer, and its images into the matching `Textures` directory. With the default prefix, those are:

```text
~/.local/share/reshade/reshade-shaders/Shaders
~/.local/share/reshade/reshade-shaders/Textures
```

New application configurations use those paths automatically. For an existing application configuration, set `EffectSearchPaths` and `TextureSearchPaths` in its `[GENERAL]` section to the corresponding `Shaders/**` and `Textures/**` paths, then reload effects in the ReShade editor.

Select the portable example add-ons from the installer menu, or manually copy a native Linux `.addon` or `.addon64` module into the add-on path printed by the installer (normally `~/.local/share/reshade`). Restart the application after adding one. Windows `.addon64` binaries are not Linux modules and cannot be loaded by this build.

## Uninstall

```sh
./uninstall.sh
```

This removes only the ReShade host and Vulkan layer manifest. It deliberately preserves shaders, optional add-ons, configurations, presets and logs.

## Scope

- Linux x86-64 Vulkan on Wayland, X11, or XWayland, with experimental Wine/Proton hosting
- Native Linux add-ons only; Windows `.addon64` binaries are reported as incompatible and are not loaded
- No Windows add-on binaries, OpenGL injection, or gamepad navigation
- Editor input is still experimental in non-fullscreen applications; Wayland cannot fully stop the application receiving input while the overlay is open
