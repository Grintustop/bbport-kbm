# bbport-kbm — keyboard & mouse for Bloodborne PC

**English** · [Русский](README.ru.md)

An add-on for the experimental native Bloodborne port
[**Bloodborne PC / bbport**](https://github.com/DarkIzuku/bloodborne_pc) (Windows build
“Bloodborne PC Offline v0.1”). The port only has a gamepad and a fixed keyboard fallback
(no mouse, no rebinding). This add-on adds:

- **Mouse camera** (mouse → right stick, same model as shadPS4's `mouse_to_joystick`).
- **Fully rebindable keyboard and mouse buttons**, including combinations (`lshift,leftbutton`),
  mouse wheel and side buttons.
- **shadPS4 config compatibility**: uses shadPS4's `input_config` format, so your existing
  shadPS4 `default.ini` / `CUSA03173.ini` works as-is (one-click import).
- **BloodborneControls.exe** — a bindings editor (English / Russian).
- **Smooth stick turning for keys**: removes the stumble when you add A or D while sprinting
  forward (see below).
- Real gamepads keep working; keyboard and mouse are merged into them.

No game files and no files of the port are included.

## Install

1. Download `bbport-kbm-x.y.z.zip` from [Releases](../../releases).
2. Extract it **into the Bloodborne PC folder** (the one with `BloodborneLauncher.exe`).
3. Run `install-kbm.bat`. It keeps the original `out\SDL3.dll` as `out\SDL3_real.dll`.
4. Run `BloodborneControls.exe` to set up your keys (or *Import from shadPS4*), press *Save*.
5. Start the game from the launcher as usual.

**Uninstall:** run `uninstall-kbm.bat` (restores the original `SDL3.dll`; your bindings stay in
`input_config\`). To disable it for one run, set the environment variable `BB_KBM=0`.

## In game

| Key | Action |
|---|---|
| **F7** | capture / release the mouse |
| **F8** | reload the bindings (edit them in BloodborneControls.exe while the game runs) |
| **Insert** | the port's own menu — the mouse is released while it is open |

The mouse is also released when the game window loses focus.

## Configuration

`input_config\default.ini` and `input_config\global.ini` in the Bloodborne PC folder, in
[shadPS4's input format](https://github.com/shadps4-emu/shadPS4):

```ini
cross = e
r1 = leftbutton
r2 = lshift,leftbutton        # combination: both held
pad_up = mousewheelup
axis_left_y_minus = w
leftjoystick_halfmode = tab
mouse_to_joystick = right
mouse_movement_params = 0.5, 1.0, 0.125   # deadzone offset, speed, speed offset
```

Extra options of this add-on (ignored by shadPS4):

| Option | Default | Meaning |
|---|---|---|
| `stick_smoothing_ms` | `100` | Time for a key-driven stick to turn by 90°. `0` = instant. |
| `stick_shape` | `circle` | `circle`: W+A tilts the stick like a real thumb (≈ 0.71, 0.71). `square`: corners, like shadPS4. |
| `mouse_poll_ms` | `33` | Mouse sampling period (shadPS4 uses 33 ms). |

Hotkeys in `global.ini`: `hotkey_toggle_mouse_to_joystick` (F7), `hotkey_reload_inputs` (F8).
Keys are matched by position (scancodes), so the keyboard layout (e.g. Russian) does not matter.

### Why the stick smoothing?

With keys the stick direction jumps by 45° in one frame (W → W+A). A thumb cannot do that, and
FromSoftware's engine (Bloodborne, also Dark Souls 3) reacts to such a jump while sprinting with a
stumbling turn animation — on one side or both depending on the camera. The add-on turns the
key-driven stick along its rim instead (45° in ~50 ms at the default). Reversals (W → S) still go
through the centre at once.

### KeyTest.exe

Prints every key press/release with a timestamp — useful to check keyboard ghosting (keys that
the keyboard drops when several are held).

## How it works

`out\SDL3.dll` is replaced by a small proxy; every SDL function is forwarded to the original
(`out\SDL3_real.dll`) except the gamepad, keyboard-state, event and window calls that the port's
runtime (`bb-probe.exe`) uses for the pad. The proxy adds a virtual “Keyboard & Mouse” gamepad
fed from your bindings, hides the runtime's hard-coded keyboard fallback, and puts the window in
SDL relative mouse mode while playing. While the port's overlay menu is open the runtime ignores
the pad and the mouse is released (the menu flag is located by a byte signature of the v0.1 build;
other builds fall back to toggling with Insert). Log: `logs\kbm-input.log`.

The `.def` file of the proxy is generated from the exports of the port's own SDL3.dll, so a
port update that ships a different SDL3 needs a rebuild.

## Build

Requirements: Windows 10/11, Python 3, `pip install ziglang` (C compiler); the C# compiler of
.NET Framework 4 is part of Windows.

```bat
python build.py --bbport "D:\Games\Bloodborne PC"            :: build into dist\
python build.py --bbport "D:\Games\Bloodborne PC" --install  :: and install
python build.py --bbport "D:\Games\Bloodborne PC" --zip      :: and pack the release zip
```

`--bbport` points to the Bloodborne PC package (its SDL3.dll exports are needed).

## Credits and license

GPL-2.0-or-later ([LICENSE](LICENSE)), like bbport and shadPS4.
The input config format, key names and the mouse-to-joystick model follow
[shadPS4](https://github.com/shadps4-emu/shadPS4) (GPL-2.0-or-later).
[SDL](https://libsdl.org) (zlib) is used through the port's own SDL3.dll.
Not affiliated with Sony Interactive Entertainment, FromSoftware, the bbport or shadPS4 authors.
