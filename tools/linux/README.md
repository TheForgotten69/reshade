# ReShade for native Linux (Vulkan)

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

## Per-game configuration

The first launch of a game creates `${XDG_DATA_HOME:-~/.local/share}/reshade/configurations/{profile}/ReShade.ini` with default settings. The game's preset is stored next to it, but the overlay can point any game at a shared preset file instead. Logs are written to `${XDG_DATA_HOME:-~/.local/share}/reshade/logs/{profile}/`. As on Windows, a `ReShade.ini` next to a native executable is used in place instead. As on Windows, the file name is not case-sensitive: a `reshade.ini` in either place is renamed to `ReShade.ini` and used.

Without `RESHADE_PROFILE`, a profile name is derived automatically in this order, and the log records the result as `Resolved ... identity '{profile}'`:

1. Steam games: install directory name and app ID, such as `Age_of_Mythology_Retold-1934680`. Wine helper processes and the Proton launcher share the game's configuration and write their own `ReShade-{process}.log` next to the game log.
2. Other Wine games: install directory or Windows executable name, and the prefix name, such as `Game-My_Prefix`.
3. Native applications: the application name reported to Vulkan, or the executable name, with the Steam app ID when available. Emulators append the game file passed on the command line, such as `RPCS3-inFamous`. An emulator started without a game argument uses one configuration for the emulator, even for games booted from its game list afterwards.

### Choosing a profile with `RESHADE_PROFILE`

Set `RESHADE_PROFILE` to pick the configuration explicitly, for example when two native applications report the same name, when a launcher hides the game, or to keep several setups for one game:

```sh
RESHADE_ENABLE=1 RESHADE_PROFILE="Elden Ring" %command%
```

For emulators, either launch the game directly (for example `rpcs3 --no-gui /path/to/EBOOT.BIN`, or a Steam shortcut doing so) or set the profile when opening the game list:

```sh
RESHADE_ENABLE=1 RESHADE_PROFILE=inFamous rpcs3
```

The profile is used as the directory name, with punctuation other than `-` and `_` replaced by `_`, so `RESHADE_PROFILE="Elden Ring"` uses `configurations/Elden_Ring/`. Launches with the same name share one configuration and preset.

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
