# hypr-float-memory

A Hyprland plugin that restores floating window sizes and positions and remembers interactive mouse moves and resizes across sessions. The default size is 1200 × 720 logical pixels. Window policies are customizable in Lua; storage retention is configured in TOML.

## Build

Requirements: a C++23 compiler, CMake 3.25+, a CMake-supported build tool, pkg-config, and development headers matching the running Hyprland instance.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Release builds enable `-O3`, `-march=native`, and CMake interprocedural optimization (LTO) for project targets. The compiler and linker are checked during configuration. For binaries intended for other CPUs or cross-compilation, use `-DHFM_NATIVE_ARCH=OFF`; LTO can be disabled with `-DHFM_ENABLE_LTO=OFF`. Debug and RelWithDebInfo builds do not receive these Release-only options.

CMake FetchContent retrieves hash-verified nlohmann/json, toml++, and ReaderWriterQueue sources. They are header-only dependencies compiled into the plugin. The storage core is statically linked. Hyprland interfaces use the installed headers and symbols provided by the running compositor. Rebuild against matching headers whenever Hyprland changes. The project does not pin a Hyprland release.

## Install and load

```sh
cmake --install build --prefix "$HOME/.local"
hyprctl plugin load "$HOME/.local/lib/hyprland/hypr-float-memory.so"
```

This per-user installation requires no root access. For startup loading, add this to a Lua file sourced by your Hyprland configuration:

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd('hyprctl plugin load "$HOME/.local/lib/hyprland/hypr-float-memory.so"')
end)
```

Keep plugin loading in the startup callback, not in a direct `hl.plugin.load()` call during configuration parsing. If startup also runs `hyprpm reload`, wait for it to finish before loading a manually managed plugin; a later hyprpm reload may unload that plugin. Hyprland reloads its configuration after loading a plugin.

Replace size-setting logic in your float keybind with a plain toggle:

```lua
hl.unbind("SUPER + T")
hl.bind("SUPER + T", function()
    local w = hl.get_active_window()
    if w then
        hl.dispatch(hl.dsp.window.float({window = w, action = "toggle"}))
    end
end)
```

To unload:

```sh
hyprctl plugin unload "$HOME/.local/lib/hyprland/hypr-float-memory.so"
```

Unload before reinstalling or replacing a loaded binary. The dynamic loader can retain an unloaded library; use a distinct path for an in-session replacement, or restart the compositor after installation. Building in the checkout does not modify the installed copy. Loading does not resize windows already on screen; a subsequent float transition or newly opened floating window applies the policy.

## Lua window policies

Place policy configuration in the regular configuration body, outside the startup callback. See [examples/config.lua](examples/config.lua).

```lua
local fm = hl.plugin.float_memory
if fm and fm.configure then
    fm.configure({
        default_size = {width = 1200, height = 720},
        center = true,
        remember_position = true,
        rule = function(w)
            if w.initial_class == "org.example.PasswordPrompt" then
                return false
            end
            if w.initial_class == "org.example.Viewer" then
                return {
                    key = w.initial_class,
                    role = w.initial_title:match("^Preview") and "preview" or "main",
                    default_size = {width = 960, height = 540},
                    center = false,
                }
            end
            return nil
        end,
    })
end
```

The application names above are examples. The plugin contains no application-specific matching logic.

| Setting | Default | Meaning |
| --- | --- | --- |
| `default_size` | `{width=1200,height=720}` | Size when no record exists |
| `center` | `true` | Center on restore; false preserves the position assigned by Hyprland |
| `remember_position` | `true` | Remember interactive moves and restore saved offsets on the current display |
| `rule` | absent | Function receiving a native `HL.Window` |

A rule can read properties including `class`, `initial_class`, `title`, `initial_title`, `xwayland`, and `monitor`. Return `nil` for the default policy, `false` to exclude the window, or a table overriding `key`, `role`, `default_size`, `center`, and/or `remember_position`. Keys and roles contain 1–4096 bytes; dimensions are integers from 1 to 32768.

The default identity is initial app ID (current app ID if absent) plus `main` or `dialog` according to the transient flag. Matching `(key, role)` pairs share persistent state. Individual windows retain their own interactive sizes within a session. Stored dimensions take precedence over defaults.

Rules are evaluated when a window first needs tracking and cached for its lifetime. `configure()` replaces the entire policy configuration and invalidates this cache; new policies apply on the next float transition or interactive resize. Invalid configuration leaves the previous configuration intact. Removing the configuration on a successful reload restores defaults.

Rules should only compute policies. Do not perform blocking I/O, reload configuration, change window state, or call `configure()` inside a rule. Calls use Hyprland's protected Lua execution with a 50 ms instruction timeout. Rule errors exclude that window and report the error. Lua references follow the compositor's configuration-state lifetime.

Saved positions are offsets from the workspace work area's top-left corner, in logical pixels. Restoration uses the current display and clamps the offset to keep the window within its work area. A window larger than the work area is aligned to its top-left corner. Temporary clamping does not replace the preferred saved position.

A saved position takes precedence over `center`. Without a saved position, `center` retains its usual behavior. Set `remember_position = false` globally or in a rule to ignore saved positions and disable recording new positions for those windows; size memory remains enabled. First-time restoration does not create a saved position: an interactive move or resize does.

## Retention

Copy the supplied [config.toml](config.toml) once:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/hypr-float-memory"
cp config.toml "${XDG_CONFIG_HOME:-$HOME/.config}/hypr-float-memory/config.toml"
```

```toml
[retention]
max_age_days = 30
```

The value must be an integer from 0 to 36500; zero disables expiry. The default is 30 when the file or setting is absent. Unknown settings, malformed TOML, and invalid values prevent initialization without changing saved data.

Cleanup runs on plugin load. Restoring geometry or recording an interactive move or resize refreshes its last-use timestamp; merely leaving a window open does not. TOML changes require unloading and loading the plugin, not just reloading the Hyprland configuration. Cleanup failure prevents initialization.

## Behavior and saved data

- Restoration fits the workspace and respects client size limits. Client minimum dimensions can exceed the available area.
- Interactive mouse moves and resizes handled by Hyprland's drag controller are remembered. Keyboard/programmatic geometry dispatchers and application-driven changes are not recorded.
- Tiling, fullscreen/maximized geometry and temporary floating during tiled dragging are excluded.
- Dimensions, optional position offsets, and last-use timestamps are persisted. Workspace, display identity, and floating state are not restored.
- X11 override-redirect windows and native Wayland popups are not managed.

Saved sizes live in `$XDG_STATE_HOME/hypr-float-memory/sizes.cbor`, falling back to `~/.local/state/hypr-float-memory/sizes.cbor`. Invalid state files prevent initialization. To reset saved sizes, unload the plugin and delete this file.

Updates are saved after a 300 ms quiet interval. Drag completion and window closure request a flush; unloading waits for pending writes. Abrupt process termination can lose pending updates. Each state directory supports one writer; nested sessions need separate XDG directories.

For tests, storage format, and concurrency constraints, see [development](docs/development.md).

## License

Original project code is licensed under [BSD-3-Clause](LICENSE), copyright 2026 andrew.
Third-party components retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).
Binary distributions must include both files. CMake installs them under
`share/licenses/hypr-float-memory/` by default. The hyprpm manifest builds and loads
the plugin but does not run CMake installation; include the notices separately
when distributing its cached binary.
