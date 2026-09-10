# ReShade native Linux/Vulkan beta 2

Experimental x86-64 ReShade for native Vulkan applications running under Wayland. It does not use Wine or Proton.

## Install

Extract the archive and run:

```sh
./install.sh
```

The default prefix is `~/.local`; override it with `PREFIX=/custom/prefix ./install.sh`.
The installer always installs the opt-in Vulkan layer and standard ReShade shaders. It then asks about each optional native add-on. Choosing an add-on installs it, which makes it load automatically when ReShade is enabled. The default answer is **No** for every optional add-on.

For unattended installs, set `INSTALL_ADDONS` to `none`, `all`, or a comma-separated list such as:

```sh
INSTALL_ADDONS=fps_limit,texture_dump ./install.sh
```

Generic Depth and Effect Runtime Sync are built into the host and are not separate selections. This archive deliberately omits the FFmpeg Video Capture add-on, since its external codec dependency can conflict with applications that bundle their own FFmpeg.

Launch an application with:

```sh
RESHADE_ENABLE=1 /path/to/application
```

Configurations are created under `${XDG_CONFIG_HOME:-~/.config}/reshade`; logs and add-ons live below `${XDG_DATA_HOME:-~/.local/share}/reshade`.

## Uninstall

```sh
./uninstall.sh
```

This removes only the ReShade host and Vulkan layer manifest. It deliberately preserves shaders, optional add-ons, configurations, presets and logs.

## Scope

- Linux x86-64, Vulkan and Wayland
- Native Linux add-ons only; Windows `.addon64` binaries are reported as incompatible and are not loaded
- No Wine/Proton integration, OpenGL injection or gamepad navigation

