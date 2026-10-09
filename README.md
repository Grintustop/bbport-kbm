# bbport-kbm — shadPS4-style keyboard & mouse for Bloodborne PC

**English** · [Русский](README.ru.md)

An alternative control layer for the native Bloodborne port
[**Bloodborne PC / bbport**](https://github.com/deadinside28/bloodborne_pc) by deadinside28,
Windows builds from the [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc)
fork's Actions. You map **PS4 buttons to keys**, exactly like shadPS4's keyboard config, instead
of the port's action list:

- **shadPS4 input config**: the same `input_config` format and key names; your existing shadPS4
  `default.ini` / `CUSA03173.ini` works as-is (one-click import).
- **Any key or mouse button on any PS4 button**, combinations (`lshift,leftbutton`), mouse wheel
  and side buttons, two bindings per button.
- **BloodborneControls.exe**: a bindings editor — click a field, press a key (English / Russian).
- **Mouse camera**: on v0.4 the port's native mouse camera and menu pointer are kept; otherwise
  shadPS4's mouse-to-stick model.
- **Smooth stick turning for keys** (no stumble when adding A/D while sprinting), circle or square
  diagonals.
- Real gamepads keep working; keyboard and mouse are merged into them.

No game files and no files of the port are included.

## Supported port builds

| Port build | Camera | Notes |
|---|---|---|
| **v0.4** Windows (PC controls on in the launcher, default) | native mouse camera and menu pointer of the port | the port's own key bindings (*Controls*) are switched off while the layer runs; keys and mouse buttons come from `input_config` |
| **v0.4** with PC controls off | mouse → right stick (`mouse_to_joystick`) | everything comes from `input_config` |
| **v0.1** (“Bloodborne PC Offline v0.1”) | mouse → right stick | also fixes the touchpad (gestures, Personal Effects) of that build |

The layer finds what it needs in the port's `bb-probe.exe` by its symbol names, so it adapts to
the build it is installed in and leaves out what a build does not have.

### Compared with the port's own PC controls (v0.4)

| | port's PC controls | bbport-kbm |
|---|---|---|
| What you bind | 29 game actions (attack, strong attack + modifier, …) | PS4 buttons (R1, R2, ○, …), like a pad |
| Editing | key names typed / captured per action in the launcher | click and press in BloodborneControls.exe, or a text file |
| Config | `bbport.ini` `bind.*` | shadPS4 `input_config` (portable to/from shadPS4) |
| Combinations | strong-attack modifier only | any 2–3 keys on any button |
| Mouse camera, menu pointer | native | native (kept) |

## Install

1. Download `bbport-kbm-x.y.z.zip` from [Releases](../../releases).
2. Extract it **into the Bloodborne PC folder** (the one with `BloodborneLauncher.exe`).
3. Run `install-kbm.bat`. It keeps the original `out\SDL3.dll` as `out\SDL3_real.dll`.
4. Run `BloodborneControls.exe` to set up your keys (or *Import from shadPS4*), press *Save*.
5. Start the game from the launcher as usual.

**Uninstall:** run `uninstall-kbm.bat` (restores the original `SDL3.dll`; your bindings stay in
`input_config\`, the port's own bindings work again). To disable the layer for one run, set the
environment variable `BB_KBM=0`.

## In game

| Key | Action |
|---|---|
| **F7** | capture / release the mouse (stick camera mode) |
| **F8** | reload the bindings (edit them in BloodborneControls.exe while the game runs) |
| **Insert** | the port's own menu — the mouse is released while it is open |

The mouse is also released when the game window loses focus or a text box is open.

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

`mouse_to_joystick` and `mouse_movement_params` apply only when the port's native mouse camera is
off; with it, sensitivity and inversion are set in the port's launcher.
Hotkeys in `global.ini`: `hotkey_toggle_mouse_to_joystick` (F7), `hotkey_reload_inputs` (F8).
Keys are matched by position (scancodes), so the keyboard layout (e.g. Russian) does not matter.

### Why the stick smoothing?

With keys the stick direction jumps by 45° in one frame (W → W+A). A thumb cannot do that, and
FromSoftware's engine may react to such a jump while sprinting with a stumbling turn animation
(I experienced this in Bloodborne and Dark Souls 3) — on one side or both depending on the camera.
The add-on turns the key-driven stick along its rim instead (45° in ~50 ms at the default).
Reversals (W → S) still go through the centre at once.

### Touchpad: gestures and Personal Effects

| Binding | Opens |
|---|---|
| `touchpad_center`, `touchpad_left` | Gestures |
| `touchpad_right` | Personal Effects |

The game accepts a touchpad press only when the touch looks like a DualShock 4 one: a new touch
id (1–127) for every press and the time it has been held. The v0.1 runtime reports id 0 and no
hold time, so the touchpad was ignored from every source (bbport fixed this in 0.4,
[deadinside28/bloodborne_pc#48](https://github.com/deadinside28/bloodborne_pc/issues/48)). On v0.1
the add-on wraps the runtime's pad read and fills these fields in; on builds that have the fix it
does nothing.

### KeyTest.exe

Prints every key press/release with a timestamp — useful to check keyboard ghosting (keys that
the keyboard drops when several are held).

## How it works

`out\SDL3.dll` is replaced by a small proxy; every SDL function is forwarded to the original
(`out\SDL3_real.dll`) except the gamepad, keyboard-state, event and window calls that the port's
runtime (`bb-probe.exe`) uses for the pad. The proxy adds a virtual “Keyboard & Mouse” gamepad
fed from your bindings and hides the keyboard from the runtime's own key handling (v0.1's fixed
layout, v0.4's `bind.*`). On v0.4 with PC controls, mouse button and wheel presses are kept from
the runtime while the mouse is captured (they are yours), but mouse movement and menu clicks still
reach it, so the native camera and pointer work. Without the native camera the proxy captures the
mouse itself (SDL relative mode) and turns its movement into the right stick. The port's overlay
menu and text prompts are found through the runtime's symbols; while they are open the mouse is
released. Log: `logs\kbm-input.log`.

The `.def` file of the proxy is generated from the exports of the port's own SDL3.dll (the same in
v0.1 and v0.4), so a port update that ships a different SDL3 needs a rebuild.

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
