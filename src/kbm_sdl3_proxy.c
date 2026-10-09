/* bbport keyboard & mouse input layer.
 *
 * A proxy SDL3.dll placed in out\ in front of the real one (renamed SDL3_real.dll). Every SDL
 * export is forwarded to SDL3_real.dll except the gamepad, keyboard-state, event and window
 * calls the runtime (bb-probe.exe) uses for the pad. Those add a virtual gamepad that is fed
 * from the keyboard and the mouse, configured with shadPS4's input_config format
 * (<root>\input_config\default.ini and global.ini), including mouse_to_joystick.
 *
 * - With no real gamepad connected the runtime opens the virtual one ("Keyboard & Mouse").
 * - With a real gamepad connected the keyboard and mouse are merged into it.
 * - The runtime's own hard-coded keyboard fallback is hidden (it only sees F9 for pad recording).
 * - The mouse is captured (relative mode) while the game window has focus and the bbport
 *   overlay menu (Insert / L3+R3) is closed. F7 toggles the capture, F8 reloads the config
 *   (hotkey_toggle_mouse_to_joystick / hotkey_reload_inputs in global.ini).
 * - Keys move a stick along its rim (stick_smoothing_ms) instead of jumping: FromSoftware games
 *   play a stumbling turn when the stick direction jumps by 45 degrees while sprinting.
 * - BB_KBM=0 in the environment turns all of this off (pure pass-through).
 *
 * License: GPL-2.0-or-later (see LICENSE).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __declspec(dllexport)

typedef uint32_t SDL_JoystickID;
typedef uint32_t SDL_PropertiesID;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Window SDL_Window;
typedef union SDL_Event { uint32_t type; uint8_t padding[128]; } SDL_Event;

/* SDL3 event types and field offsets (SDL_events.h, SDL 3.2+). */
#define EV_WINDOW_FOCUS_GAINED 0x20E
#define EV_WINDOW_FOCUS_LOST 0x20F
#define EV_KEY_DOWN 0x300
#define EV_MOUSE_MOTION 0x400
#define EV_MOUSE_BUTTON_DOWN 0x401
#define EV_MOUSE_BUTTON_UP 0x402
#define EV_MOUSE_WHEEL 0x403

#define SC_F9 66
#define SC_INSERT 73
#define SC_COUNT 512

enum { BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_BACK, BTN_GUIDE, BTN_START, BTN_LSTICK, BTN_RSTICK,
       BTN_L1, BTN_R1, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_MISC1, BTN_RPADDLE1, BTN_LPADDLE1,
       BTN_RPADDLE2, BTN_LPADDLE2, BTN_TOUCHPAD, BTN_COUNT };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT, AX_COUNT };

/* ---- real SDL ------------------------------------------------------------------------------ */

static HMODULE real_sdl;
static SDL_JoystickID *(*r_GetGamepads)(int *);
static SDL_Gamepad *(*r_OpenGamepad)(SDL_JoystickID);
static void (*r_CloseGamepad)(SDL_Gamepad *);
static bool (*r_GamepadConnected)(SDL_Gamepad *);
static const char *(*r_GetGamepadName)(SDL_Gamepad *);
static bool (*r_GetGamepadButton)(SDL_Gamepad *, int);
static int16_t (*r_GetGamepadAxis)(SDL_Gamepad *, int);
static int (*r_GetNumGamepadTouchpads)(SDL_Gamepad *);
static int (*r_GetNumGamepadTouchpadFingers)(SDL_Gamepad *, int);
static bool (*r_GetGamepadTouchpadFinger)(SDL_Gamepad *, int, int, bool *, float *, float *, float *);
static bool (*r_RumbleGamepad)(SDL_Gamepad *, uint16_t, uint16_t, uint32_t);
static const bool *(*r_GetKeyboardState)(int *);
static bool (*r_PollEvent)(SDL_Event *);
static SDL_Window *(*r_CreateWindowWithProperties)(SDL_PropertiesID);
static bool (*r_SetWindowRelativeMouseMode)(SDL_Window *, bool);
static void *(*r_malloc)(size_t);
static void (*r_free)(void *);

/* ---- state --------------------------------------------------------------------------------- */

#define FAKE_ID ((SDL_JoystickID)0x6B626D31u) /* "kbm1" */
static struct { int dummy; } fake_pad_obj;
#define FAKE_PAD ((SDL_Gamepad *)&fake_pad_obj)

#define IN_MOUSE 512                /* + SDL mouse button 1..5 */
#define IN_WHEEL_UP 520
#define IN_WHEEL_DOWN 521
#define IN_WHEEL_LEFT 522
#define IN_WHEEL_RIGHT 523
#define IN_COUNT 528

enum { OUT_BUTTON, OUT_AXIS, OUT_HALF_LEFT, OUT_HALF_RIGHT };
enum { TOUCH_NONE, TOUCH_LEFT, TOUCH_CENTER, TOUCH_RIGHT };

typedef struct {
    int nkeys, keys[3];
    int kind, id, value, touch; /* touch: TOUCH_* for touchpad outputs */
} Binding;

#define MAX_BINDINGS 256
typedef struct {
    Binding b[MAX_BINDINGS];
    int n;
    int mouse_stick; /* 0 none, 1 left, 2 right */
    float deadzone_offset, speed, speed_offset;
    int poll_ms;
    bool square_sticks; /* stick_shape = square: keyboard diagonals reach the corners, as in shadPS4 */
    float smooth_ms;    /* stick_smoothing_ms: time a key-driven stick needs to turn by 90 degrees */
    int key_toggle_capture, key_reload;
    bool uses_mouse;
} Config;

static CRITICAL_SECTION lock;
static bool enabled = true;
static Config cfg;
static wchar_t root_dir[MAX_PATH], log_path[MAX_PATH];
static FILE *log_file;

static SDL_Window *game_window;
static volatile bool focused = true, capture_enabled = true, captured, menu_guess;
static volatile const uint8_t *menu_open_flag; /* BbOverlay::menu_open inside bb-probe.exe */
static bool mouse_down[8];
static float acc_dx, acc_dy;
static volatile int wheel_pending[4];
static double wheel_until[4];
static volatile bool reload_requested;

typedef struct {
    bool button[BTN_COUNT];
    int16_t axis[AX_COUNT];
    int touch;
} Snapshot;
static Snapshot snap;
static double snap_time = -1e9, mouse_sample_time = -1e9;
static float mouse_ax, mouse_ay;
static float key_stick[2][2]; /* key-driven stick vectors after smoothing */
static double smooth_time = -1e9;
static bool real_pad_present;
static double real_pad_check_time = -1e9;

static double now_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void logf_(const char *fmt, ...) {
    if (!log_file) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}

/* ---- key names (shadPS4 input_handler.h, mapped to layout-independent scancodes) ------------ */

typedef struct { const char *name; int code; } Name;
static const Name key_names[] = {
    {"a", 4}, {"b", 5}, {"c", 6}, {"d", 7}, {"e", 8}, {"f", 9}, {"g", 10}, {"h", 11}, {"i", 12},
    {"j", 13}, {"k", 14}, {"l", 15}, {"m", 16}, {"n", 17}, {"o", 18}, {"p", 19}, {"q", 20},
    {"r", 21}, {"s", 22}, {"t", 23}, {"u", 24}, {"v", 25}, {"w", 26}, {"x", 27}, {"y", 28},
    {"z", 29}, {"1", 30}, {"2", 31}, {"3", 32}, {"4", 33}, {"5", 34}, {"6", 35}, {"7", 36},
    {"8", 37}, {"9", 38}, {"0", 39}, {"enter", 40}, {"escape", 41}, {"backspace", 42},
    {"tab", 43}, {"space", 44}, {"minus", 45}, {"equals", 46}, {"lbracket", 47},
    {"rbracket", 48}, {"backslash", 49}, {"semicolon", 51}, {"apostrophe", 52}, {"grave", 53},
    {"comma", 54}, {"period", 55}, {"slash", 56}, {"capslock", 57}, {"f1", 58}, {"f2", 59},
    {"f3", 60}, {"f4", 61}, {"f5", 62}, {"f6", 63}, {"f7", 64}, {"f8", 65}, {"f9", 66},
    {"f10", 67}, {"f11", 68}, {"f12", 69}, {"printscreen", 70}, {"scrolllock", 71},
    {"pausebreak", 72}, {"insert", 73}, {"home", 74}, {"pgup", 75}, {"delete", 76}, {"end", 77},
    {"pgdown", 78}, {"right", 79}, {"left", 80}, {"down", 81}, {"up", 82}, {"kpslash", 84},
    {"kpasterisk", 85}, {"kpminus", 86}, {"kpplus", 87}, {"kpenter", 88}, {"kp1", 89},
    {"kp2", 90}, {"kp3", 91}, {"kp4", 92}, {"kp5", 93}, {"kp6", 94}, {"kp7", 95}, {"kp8", 96},
    {"kp9", 97}, {"kp0", 98}, {"kpperiod", 99}, {"kpequals", 103}, {"kpcomma", 133},
    {"lctrl", 224}, {"lshift", 225}, {"lalt", 226}, {"lmeta", 227}, {"lwin", 227},
    {"rctrl", 228}, {"rshift", 229}, {"ralt", 230}, {"rmeta", 231}, {"rwin", 231},
    /* shifted symbols: the key they are typed with */
    {"tilde", 53}, {"exclamation", 30}, {"at", 31}, {"hash", 32}, {"dollar", 33},
    {"percent", 34}, {"caret", 35}, {"ampersand", 36}, {"asterisk", 37}, {"lparen", 38},
    {"rparen", 39}, {"underscore", 45}, {"plus", 46}, {"lbrace", 47}, {"rbrace", 48},
    {"pipe", 49}, {"colon", 51}, {"quote", 52}, {"less", 54}, {"greater", 55}, {"question", 56},
    /* mouse */
    {"leftbutton", IN_MOUSE + 1}, {"middlebutton", IN_MOUSE + 2}, {"rightbutton", IN_MOUSE + 3},
    {"sidebuttonback", IN_MOUSE + 4}, {"sidebuttonforward", IN_MOUSE + 5},
    {"mousewheelup", IN_WHEEL_UP}, {"mousewheeldown", IN_WHEEL_DOWN},
    {"mousewheelleft", IN_WHEEL_LEFT}, {"mousewheelright", IN_WHEEL_RIGHT},
};

static int key_code(const char *s) {
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++)
        if (!strcmp(key_names[i].name, s)) return key_names[i].code;
    return -1;
}

typedef struct { const char *name; int kind, id, value, touch; } OutName;
static const OutName out_names[] = {
    {"cross", OUT_BUTTON, BTN_SOUTH, 0, 0}, {"circle", OUT_BUTTON, BTN_EAST, 0, 0},
    {"square", OUT_BUTTON, BTN_WEST, 0, 0}, {"triangle", OUT_BUTTON, BTN_NORTH, 0, 0},
    {"l1", OUT_BUTTON, BTN_L1, 0, 0}, {"r1", OUT_BUTTON, BTN_R1, 0, 0},
    {"l3", OUT_BUTTON, BTN_LSTICK, 0, 0}, {"r3", OUT_BUTTON, BTN_RSTICK, 0, 0},
    {"pad_up", OUT_BUTTON, BTN_UP, 0, 0}, {"pad_down", OUT_BUTTON, BTN_DOWN, 0, 0},
    {"pad_left", OUT_BUTTON, BTN_LEFT, 0, 0}, {"pad_right", OUT_BUTTON, BTN_RIGHT, 0, 0},
    {"options", OUT_BUTTON, BTN_START, 0, 0}, {"back", OUT_BUTTON, BTN_BACK, 0, 0},
    {"share", OUT_BUTTON, BTN_BACK, 0, 0},
    {"touchpad", OUT_BUTTON, BTN_TOUCHPAD, 0, TOUCH_CENTER},
    {"touchpad_center", OUT_BUTTON, BTN_TOUCHPAD, 0, TOUCH_CENTER},
    {"touchpad_left", OUT_BUTTON, BTN_TOUCHPAD, 0, TOUCH_LEFT},
    {"touchpad_right", OUT_BUTTON, BTN_TOUCHPAD, 0, TOUCH_RIGHT},
    {"l2", OUT_AXIS, AX_LT, 127, 0}, {"r2", OUT_AXIS, AX_RT, 127, 0},
    {"axis_left_x_plus", OUT_AXIS, AX_LX, 127, 0}, {"axis_left_x_minus", OUT_AXIS, AX_LX, -127, 0},
    {"axis_left_y_plus", OUT_AXIS, AX_LY, 127, 0}, {"axis_left_y_minus", OUT_AXIS, AX_LY, -127, 0},
    {"axis_right_x_plus", OUT_AXIS, AX_RX, 127, 0}, {"axis_right_x_minus", OUT_AXIS, AX_RX, -127, 0},
    {"axis_right_y_plus", OUT_AXIS, AX_RY, 127, 0}, {"axis_right_y_minus", OUT_AXIS, AX_RY, -127, 0},
    {"leftjoystick_halfmode", OUT_HALF_LEFT, 0, 0, 0},
    {"rightjoystick_halfmode", OUT_HALF_RIGHT, 0, 0, 0},
};

/* ---- config -------------------------------------------------------------------------------- */

static const char default_config[] =
    "# bbport keyboard & mouse bindings (shadPS4 input_config format).\n"
    "# output = key[,key[,key]]   - several keys = a combination (all held).\n"
    "# Mouse: leftbutton, rightbutton, middlebutton, sidebuttonback, sidebuttonforward,\n"
    "#        mousewheelup, mousewheeldown, mousewheelleft, mousewheelright.\n"
    "# F7 toggles mouse capture, F8 reloads this file (see global.ini).\n\n"
    "cross = e\ncircle = space\ntriangle = lshift,e\nsquare = r\n\n"
    "pad_up = 1\npad_down = 2\npad_left = 3\npad_right = 4\n\n"
    "l1 = rightbutton\nl2 = lshift,rightbutton\nr1 = leftbutton\nr2 = lshift,leftbutton\n"
    "l3 = q\nr3 = middlebutton\n\n"
    "options = escape\ntouchpad_center = g\ntouchpad_left = tab\ntouchpad_right = backspace\n\n"
    "axis_left_y_minus = w\naxis_left_y_plus = s\naxis_left_x_minus = a\naxis_left_x_plus = d\n\n"
    "axis_right_y_minus = up\naxis_right_y_plus = down\naxis_right_x_minus = left\n"
    "axis_right_x_plus = right\n\n"
    "leftjoystick_halfmode = lctrl\n\n"
    "# keyboard-driven sticks: circle or square diagonals, ms per 90 degree turn (0 = instant)\n"
    "stick_shape = circle\nstick_smoothing_ms = 100\n\n"
    "mouse_to_joystick = right\n"
    "# deadzone offset, speed, speed offset\n"
    "mouse_movement_params = 0.5, 1.0, 0.125\n";

static const char default_global[] =
    "# Loaded for all games alongside default.ini.\n"
    "hotkey_toggle_mouse_to_joystick = f7\n"
    "hotkey_reload_inputs = f8\n";

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int count_keys(const Binding *b) { return b->nkeys; }

static int binding_cmp(const void *pa, const void *pb) {
    const Binding *a = pa, *b = pb;
    if (count_keys(a) != count_keys(b)) return count_keys(b) - count_keys(a);
    int ra = a->kind == OUT_BUTTON ? 0 : 1, rb = b->kind == OUT_BUTTON ? 0 : 1;
    return ra - rb;
}

static void parse_line(Config *c, char *line, const char *file, int lineno) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = 0;
    char *out = trim(line), *in = trim(eq + 1);
    if (!*out || !*in) return;

    if (!strcmp(out, "mouse_to_joystick")) {
        c->mouse_stick = !strcmp(in, "left") ? 1 : !strcmp(in, "right") ? 2 : 0;
        return;
    }
    if (!strcmp(out, "mouse_movement_params")) {
        float a, b, d;
        if (sscanf(in, "%f , %f , %f", &a, &b, &d) == 3) {
            c->deadzone_offset = a; c->speed = b; c->speed_offset = d;
        }
        return;
    }
    if (!strcmp(out, "stick_shape")) { /* bbport extension: circle (default) or square */
        c->square_sticks = !strcmp(in, "square");
        return;
    }
    if (!strcmp(out, "stick_smoothing_ms")) { /* bbport extension, 0 = instant */
        float v = (float)atof(in);
        if (v >= 0 && v <= 1000) c->smooth_ms = v;
        return;
    }
    if (!strcmp(out, "mouse_poll_ms")) { /* bbport extension, shadPS4 uses 33 ms */
        int v = atoi(in);
        if (v >= 1 && v <= 200) c->poll_ms = v;
        return;
    }
    if (!strcmp(out, "hotkey_toggle_mouse_to_joystick") || !strcmp(out, "hotkey_reload_inputs")) {
        int k = key_code(in);
        if (k >= 0 && k < SC_COUNT) {
            if (out[7] == 't') c->key_toggle_capture = k; else c->key_reload = k;
        }
        return;
    }

    const OutName *o = NULL;
    for (size_t i = 0; i < sizeof out_names / sizeof *out_names; i++)
        if (!strcmp(out_names[i].name, out)) { o = &out_names[i]; break; }
    if (!o) return; /* hotkeys, deadzones, colours...: not ours */

    Binding b = {0};
    b.kind = o->kind; b.id = o->id; b.value = o->value; b.touch = o->touch;
    char *save = NULL;
    for (char *tok = strtok_s(in, ",", &save); tok; tok = strtok_s(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok) continue;
        int k = key_code(tok);
        if (k < 0) return; /* controller binding ("cross = cross") or unknown: not a keyboard line */
        if (b.nkeys < 3) b.keys[b.nkeys++] = k;
    }
    if (!b.nkeys) return;
    if (c->n >= MAX_BINDINGS) { logf_("%s:%d: too many bindings", file, lineno); return; }
    for (int i = 0; i < b.nkeys; i++) if (b.keys[i] >= IN_MOUSE) c->uses_mouse = true;
    c->b[c->n++] = b;
}

static bool load_file(Config *c, const wchar_t *path, const char *fallback) {
    FILE *f = _wfopen(path, L"rb");
    if (!f && fallback) {
        FILE *w = _wfopen(path, L"wb");
        if (w) { fputs(fallback, w); fclose(w); logf_("Created %ls with the default bindings", path); }
        f = _wfopen(path, L"rb");
    }
    if (!f) { logf_("Cannot open %ls", path); return false; }
    char line[512];
    int n = 0;
    char name[64];
    snprintf(name, sizeof name, "%ls", wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path);
    while (fgets(line, sizeof line, f)) parse_line(c, line, name, ++n);
    fclose(f);
    return true;
}

static void load_config(void) {
    Config *c = calloc(1, sizeof *c);
    c->deadzone_offset = 0.5f; c->speed = 1.0f; c->speed_offset = 0.125f; c->poll_ms = 33;
    c->smooth_ms = 100;
    c->key_toggle_capture = 64; c->key_reload = 65; /* F7, F8 */
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\input_config", root_dir);
    CreateDirectoryW(dir, NULL);
    swprintf(path, MAX_PATH, L"%ls\\global.ini", dir);
    load_file(c, path, default_global);
    swprintf(path, MAX_PATH, L"%ls\\default.ini", dir);
    if (!load_file(c, path, default_config)) {
        char *copy = _strdup(default_config), *save = NULL;
        int n = 0;
        for (char *l = strtok_s(copy, "\n", &save); l; l = strtok_s(NULL, "\n", &save)) parse_line(c, l, "builtin", ++n);
        free(copy);
    }
    qsort(c->b, c->n, sizeof *c->b, binding_cmp);
    EnterCriticalSection(&lock);
    cfg = *c;
    LeaveCriticalSection(&lock);
    logf_("Config: %d keyboard/mouse bindings, mouse_to_joystick=%s, params %.3f %.3f %.3f, poll %d ms, "
          "stick_shape=%s, stick_smoothing_ms=%.0f",
          c->n, c->mouse_stick == 1 ? "left" : c->mouse_stick == 2 ? "right" : "none",
          c->deadzone_offset, c->speed, c->speed_offset, c->poll_ms,
          c->square_sticks ? "square" : "circle", c->smooth_ms);
    free(c);
}

/* ---- input sampling ------------------------------------------------------------------------ */

static bool menu_open(void) {
    if (menu_open_flag) return *menu_open_flag != 0;
    return menu_guess;
}

/* The runtime keeps the high byte (+128) of a stick axis, so v * 256 gives the PS4 byte 128 + v:
 * -127..127 -> 1..255, symmetric around the centre (32767 / -32767 would give 255 / 0). */
static int16_t stick_axis16(float v127) {
    if (v127 > 127.0f) v127 = 127.0f;
    if (v127 < -127.0f) v127 = -127.0f;
    return (int16_t)(lrintf(v127) * 256); /* whole units: the runtime truncates the low byte */
}

static int16_t trigger_axis16(float v127) {
    if (v127 <= 0) return 0;
    if (v127 >= 127.0f) return 32767;
    return (int16_t)lrintf(v127 * (32767.0f / 127.0f));
}

/* Called by the pad thread; the runtime reads all buttons and axes within microseconds, so one
 * snapshot serves a whole scePadReadState. */
static void update_snapshot(void) {
    double t = now_ms();
    if (t - snap_time < 1.0) return;
    snap_time = t;
    if (reload_requested) { reload_requested = false; load_config(); }

    Snapshot s = {0};
    bool active = focused && !menu_open();
    bool pressed[IN_COUNT] = {0};
    if (active) {
        const bool *ks = r_GetKeyboardState(NULL);
        if (ks) for (int i = 0; i < SC_COUNT; i++) pressed[i] = ks[i];
    }

    EnterCriticalSection(&lock);
    if (active && captured) {
        for (int i = 1; i <= 5; i++) pressed[IN_MOUSE + i] = mouse_down[i];
        for (int i = 0; i < 4; i++) {
            if (wheel_pending[i]) { wheel_pending[i] = 0; wheel_until[i] = t + 33.0; }
            pressed[IN_WHEEL_UP + i] = t < wheel_until[i];
        }
    }
    /* mouse_to_joystick, as shadPS4 EmulateJoystick: re-sampled every poll_ms and held */
    if (t - mouse_sample_time >= cfg.poll_ms) {
        float dx = acc_dx, dy = acc_dy;
        acc_dx = acc_dy = 0;
        mouse_sample_time = t;
        if ((dx != 0 || dy != 0) && active && captured) {
            float speed = sqrtf(dx * dx + dy * dy) * cfg.speed + cfg.speed_offset * 128.0f;
            float lo = cfg.deadzone_offset * 128.0f;
            if (speed < lo) speed = lo;
            if (speed > 128.0f) speed = 128.0f;
            float a = atan2f(dy, dx);
            mouse_ax = cosf(a) * speed;
            mouse_ay = sinf(a) * speed;
        } else {
            mouse_ax = mouse_ay = 0;
        }
    }

    float axes[AX_COUNT] = {0};
    bool used[IN_COUNT] = {0}, half_l = false, half_r = false;
    for (int i = 0; i < cfg.n; i++) {
        const Binding *b = &cfg.b[i];
        bool on = true;
        for (int k = 0; k < b->nkeys && on; k++) on = pressed[b->keys[k]] && !used[b->keys[k]];
        if (!on) continue;
        if (!(b->kind == OUT_AXIS && b->value < 0))
            for (int k = 0; k < b->nkeys; k++) used[b->keys[k]] = true;
        switch (b->kind) {
        case OUT_BUTTON:
            s.button[b->id] = true;
            if (b->touch) s.touch = b->touch;
            break;
        case OUT_AXIS: axes[b->id] += (float)b->value; break;
        case OUT_HALF_LEFT: half_l = true; break;
        case OUT_HALF_RIGHT: half_r = true; break;
        }
    }
    float mouse_x = mouse_ax, mouse_y = mouse_ay;
    int mouse_stick = cfg.mouse_stick;
    bool square = cfg.square_sticks;
    float smooth = cfg.smooth_ms;
    LeaveCriticalSection(&lock);

    double dt = t - smooth_time;
    smooth_time = t;
    if (dt > 100.0) dt = 100.0;
    for (int stick = 0; stick < 2; stick++) {
        float tx = axes[stick * 2], ty = axes[stick * 2 + 1];
        tx = tx > 127 ? 127 : tx < -127 ? -127 : tx;
        ty = ty > 127 ? 127 : ty < -127 ? -127 : ty;
        /* A real stick moves inside a circle: W+A gives about (-90, -90), not the corner (-127, -127). */
        float tlen = sqrtf(tx * tx + ty * ty);
        if (tlen > 127.0f) { tx *= 127.0f / tlen; ty *= 127.0f / tlen; tlen = 127.0f; }

        /* Keys change the direction instantly, a thumb turns the stick along its rim. A 45 degree
         * jump while sprinting makes the game play a stumbling turn, so the key-driven direction
         * turns at 90 degrees per smooth_ms. Reversals (> 135 degrees) go through the centre at
         * once, as with a real stick. */
        float *k = key_stick[stick];
        float klen = sqrtf(k[0] * k[0] + k[1] * k[1]);
        if (tlen < 1.0f || klen < 1.0f || smooth <= 0) {
            k[0] = tx; k[1] = ty;
        } else {
            float cur = atan2f(k[1], k[0]), target = atan2f(ty, tx), d = target - cur;
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            float step = (float)(dt / smooth) * 1.57079633f;
            float a = fabsf(d) > 2.35619449f || fabsf(d) <= step ? target : cur + (d > 0 ? step : -step);
            k[0] = cosf(a) * tlen; k[1] = sinf(a) * tlen;
        }
        float x = k[0], y = k[1];
        if (square && (x != 0 || y != 0)) { /* stick_shape = square: push the circle out to the corners */
            float a = atan2f(y, x), m = fmaxf(fabsf(cosf(a)), fabsf(sinf(a)));
            x /= m; y /= m;
        }
        if (mouse_stick == stick + 1) {
            x += mouse_x; y += mouse_y;
            float len = sqrtf(x * x + y * y);
            if (!square && len > 127.0f) { x *= 127.0f / len; y *= 127.0f / len; }
        }
        float half = (stick == 0 ? half_l : half_r) ? 0.5f : 1.0f;
        s.axis[stick * 2] = stick_axis16(x * half);
        s.axis[stick * 2 + 1] = stick_axis16(y * half);
    }
    s.axis[AX_LT] = trigger_axis16(axes[AX_LT]);
    s.axis[AX_RT] = trigger_axis16(axes[AX_RT]);
    snap = s;
}

/* Window thread (inside SDL_PollEvent): mouse capture follows focus, the overlay menu and F7. */
static void update_capture(void) {
    if (!game_window || !r_SetWindowRelativeMouseMode) return;
    bool want = (cfg.uses_mouse || cfg.mouse_stick) && capture_enabled && focused && !menu_open();
    if (want != captured) {
        if (r_SetWindowRelativeMouseMode(game_window, want) || !want) {
            captured = want;
            EnterCriticalSection(&lock);
            acc_dx = acc_dy = 0;
            memset(mouse_down, 0, sizeof mouse_down);
            LeaveCriticalSection(&lock);
        }
    }
}

static void handle_event(const SDL_Event *e) {
    const uint8_t *p = (const uint8_t *)e;
    switch (e->type) {
    case EV_WINDOW_FOCUS_GAINED: focused = true; break;
    case EV_WINDOW_FOCUS_LOST: focused = false; break;
    case EV_KEY_DOWN: {
        uint32_t sc = *(const uint32_t *)(p + 24);
        bool repeat = p[37];
        if (repeat) break;
        if ((int)sc == cfg.key_toggle_capture) {
            capture_enabled = !capture_enabled;
            logf_("Mouse capture %s (F7)", capture_enabled ? "on" : "off");
        } else if ((int)sc == cfg.key_reload) {
            reload_requested = true;
            logf_("Reloading input config (F8)");
        } else if (sc == SC_INSERT && !menu_open_flag) {
            menu_guess = !menu_guess;
        }
        break;
    }
    case EV_MOUSE_MOTION:
        if (captured) {
            EnterCriticalSection(&lock);
            acc_dx += *(const float *)(p + 36);
            acc_dy += *(const float *)(p + 40);
            LeaveCriticalSection(&lock);
        }
        break;
    case EV_MOUSE_BUTTON_DOWN:
    case EV_MOUSE_BUTTON_UP: {
        uint8_t b = p[24];
        if (b < 8) {
            EnterCriticalSection(&lock);
            mouse_down[b] = e->type == EV_MOUSE_BUTTON_DOWN;
            LeaveCriticalSection(&lock);
        }
        break;
    }
    case EV_MOUSE_WHEEL: {
        float x = *(const float *)(p + 24), y = *(const float *)(p + 28);
        if (*(const uint32_t *)(p + 32) == 1) { x = -x; y = -y; } /* SDL_MOUSEWHEEL_FLIPPED */
        if (captured) {
            if (y > 0) wheel_pending[0] = 1;
            if (y < 0) wheel_pending[1] = 1;
            if (x < 0) wheel_pending[2] = 1;
            if (x > 0) wheel_pending[3] = 1;
        }
        break;
    }
    }
}

/* ---- exported overrides -------------------------------------------------------------------- */

static BOOL CALLBACK init(PINIT_ONCE, PVOID, PVOID *);
static INIT_ONCE init_once;
#define ENSURE() InitOnceExecuteOnce(&init_once, init, NULL, NULL)

static bool real_pads_available(void) {
    double t = now_ms();
    if (t - real_pad_check_time > 1000.0) {
        real_pad_check_time = t;
        int n = 0;
        SDL_JoystickID *ids = r_GetGamepads(&n);
        if (ids) r_free(ids);
        real_pad_present = n > 0;
    }
    return real_pad_present;
}

EXPORT SDL_JoystickID *SDL_GetGamepads(int *count) {
    ENSURE();
    int n = 0;
    SDL_JoystickID *ids = r_GetGamepads(&n);
    if (!enabled || (ids && n > 0)) {
        if (count) *count = n;
        return ids;
    }
    if (ids) r_free(ids);
    ids = r_malloc(2 * sizeof *ids);
    if (!ids) { if (count) *count = 0; return NULL; }
    ids[0] = FAKE_ID; ids[1] = 0;
    if (count) *count = 1;
    return ids;
}

EXPORT SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    ENSURE();
    if (enabled && id == FAKE_ID) { logf_("Virtual keyboard & mouse gamepad opened"); return FAKE_PAD; }
    SDL_Gamepad *g = r_OpenGamepad(id);
    if (g && enabled) logf_("Real gamepad opened: %s (keyboard & mouse merged into it)", r_GetGamepadName(g));
    return g;
}

EXPORT void SDL_CloseGamepad(SDL_Gamepad *g) {
    ENSURE();
    if (g == FAKE_PAD) return;
    r_CloseGamepad(g);
}

EXPORT bool SDL_GamepadConnected(SDL_Gamepad *g) {
    ENSURE();
    if (g == FAKE_PAD) return !real_pads_available(); /* lets the runtime switch to a real pad */
    return r_GamepadConnected(g);
}

EXPORT const char *SDL_GetGamepadName(SDL_Gamepad *g) {
    ENSURE();
    if (g == FAKE_PAD) return "Keyboard & Mouse (bbport kbm)";
    return r_GetGamepadName(g);
}

EXPORT bool SDL_GetGamepadButton(SDL_Gamepad *g, int button) {
    ENSURE();
    bool real = g != FAKE_PAD && r_GetGamepadButton(g, button);
    if (!enabled) return real;
    update_snapshot();
    return real || (button >= 0 && button < BTN_COUNT && snap.button[button]);
}

EXPORT int16_t SDL_GetGamepadAxis(SDL_Gamepad *g, int axis) {
    ENSURE();
    int16_t real = g != FAKE_PAD ? r_GetGamepadAxis(g, axis) : 0;
    if (!enabled || axis < 0 || axis >= AX_COUNT) return real;
    update_snapshot();
    int16_t k = snap.axis[axis];
    return abs(k) > abs(real) ? k : real;
}

EXPORT int SDL_GetNumGamepadTouchpads(SDL_Gamepad *g) {
    ENSURE();
    if (g == FAKE_PAD) return 1;
    return r_GetNumGamepadTouchpads(g);
}

EXPORT int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad *g, int touchpad) {
    ENSURE();
    if (g == FAKE_PAD) return touchpad == 0 ? 1 : 0;
    return r_GetNumGamepadTouchpadFingers(g, touchpad);
}

EXPORT bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad *g, int touchpad, int finger, bool *down,
                                         float *x, float *y, float *pressure) {
    if (enabled && touchpad == 0 && finger == 0) {
        update_snapshot();
        if (snap.touch || g == FAKE_PAD) {
            bool on = snap.touch != TOUCH_NONE;
            if (down) *down = on;
            if (x) *x = snap.touch == TOUCH_LEFT ? 0.25f : snap.touch == TOUCH_RIGHT ? 0.75f : 0.5f;
            if (y) *y = 0.5f;
            if (pressure) *pressure = on ? 1.0f : 0.0f;
            return true;
        }
    }
    if (g == FAKE_PAD) return false;
    return r_GetGamepadTouchpadFinger(g, touchpad, finger, down, x, y, pressure);
}

EXPORT bool SDL_RumbleGamepad(SDL_Gamepad *g, uint16_t lo, uint16_t hi, uint32_t ms) {
    ENSURE();
    if (g == FAKE_PAD) return true;
    return r_RumbleGamepad(g, lo, hi, ms);
}

/* The runtime reads the keyboard state only for its hard-coded keyboard pad. Hide it so it does
 * not fight the configured bindings; keep F9 (stops BB_PAD_RECORD). */
EXPORT const bool *SDL_GetKeyboardState(int *numkeys) {
    ENSURE();
    const bool *ks = r_GetKeyboardState(numkeys);
    if (!enabled || !ks) return ks;
    static bool filtered[SC_COUNT];
    filtered[SC_F9] = ks[SC_F9];
    return filtered;
}

EXPORT bool SDL_PollEvent(SDL_Event *e) {
    ENSURE();
    bool r = r_PollEvent(e);
    if (enabled) {
        if (r && e) handle_event(e);
        update_capture();
    }
    return r;
}

EXPORT SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
    ENSURE();
    SDL_Window *w = r_CreateWindowWithProperties(props);
    if (w && !game_window) game_window = w;
    return w;
}

/* ---- setup --------------------------------------------------------------------------------- */

static void find_menu_flag(void) {
    /* bb-probe.exe of Bloodborne PC Offline v0.1: pad_read_state checks BbOverlay::menu_open with
     * `movzx eax, byte ptr [rip+disp32]` at RVA 0x40c4e. Only used when the bytes match. */
    static const uint8_t sig[] = {0x0f, 0xb6, 0x05, 0x2e, 0xb1, 0x1f, 0x03};
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    if (!base) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uint8_t *ins = base + 0x40c4e;
    if (nt->OptionalHeader.SizeOfImage > 0x40c4e + 16 && !memcmp(ins, sig, sizeof sig)) {
        menu_open_flag = ins + 7 + *(int32_t *)(ins + 3);
        logf_("Overlay menu flag found; mouse is released while the menu is open");
    } else {
        logf_("Overlay menu flag not found (other build): Insert toggles the mouse release");
    }
}

/* ---- touchpad fix for the v0.1 runtime ------------------------------------------------------
 * The game only accepts a touchpad press when the touch looks like a DualShock 4 one: every finger
 * that goes down gets a new id (1..127, kept while it stays down) and the time since it went down.
 * The v0.1 runtime reports id 0 and no hold time, so the gesture menu never opened (from keys, Back
 * or a real touchpad); bbport fixed this later in runtime_pad.c (touch_ids). Here pad_read_state is
 * wrapped and the same fields are filled in after it. OrbisPadData: touch_count 0x34,
 * touch_held_time 0x38, touches[2] 0x3c (id at +4, 8 bytes each), timestamp 0x50 (microseconds). */

typedef int32_t(__attribute__((sysv_abi)) * PadReadStateFn)(int32_t handle, uint8_t *data);
static PadReadStateFn pad_read_state_orig;
static CRITICAL_SECTION touch_lock;

static void fix_touch_ids(uint8_t *d) {
    static uint8_t next_id = 1, ids[2];
    static bool down[2];
    static uint64_t since;
    uint8_t count = d[0x34];
    uint64_t timestamp;
    memcpy(&timestamp, d + 0x50, 8);
    EnterCriticalSection(&touch_lock);
    for (int i = 0; i < 2; i++) {
        bool now = i < count;
        if (now && !down[i]) { ids[i] = next_id; next_id = next_id == 127 ? 1 : next_id + 1; }
        down[i] = now;
        if (now) d[0x3c + 8 * i + 4] = ids[i];
    }
    if (!count) since = 0;
    else if (!since) since = timestamp;
    uint32_t held = count ? (uint32_t)(timestamp - since) : 0;
    LeaveCriticalSection(&touch_lock);
    memcpy(d + 0x38, &held, 4);
}

static __attribute__((sysv_abi)) int32_t pad_read_state_hook(int32_t handle, uint8_t *data) {
    int32_t r = pad_read_state_orig(handle, data);
    if (r == 0 && data) fix_touch_ids(data);
    return r;
}

static void hook_pad_read_state(void) {
    /* pad_read_state at RVA 0x40b20: push rbp/r15/r14/r13/r12/rbx; sub rsp, 0xe8 (17 bytes, no
     * RIP-relative operands). Only hooked when these bytes and the overlay signature match. */
    static const uint8_t prologue[17] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                         0x53, 0x48, 0x81, 0xec, 0xe8, 0x00, 0x00, 0x00};
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    uint8_t *fn = base + 0x40b20;
    if (!menu_open_flag || memcmp(fn, prologue, sizeof prologue)) {
        logf_("pad_read_state not recognised (other build): touchpad ids left as they are");
        return;
    }
    InitializeCriticalSection(&touch_lock);
    /* trampoline: the 17 original bytes, then jmp [rip+0] -> fn + 17 */
    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, prologue, sizeof prologue);
    uint8_t jmp_abs[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(tramp + 17, jmp_abs, 6);
    uint64_t back = (uint64_t)(fn + 17);
    memcpy(tramp + 23, &back, 8);
    pad_read_state_orig = (PadReadStateFn)(void *)tramp;

    uint8_t patch[17];
    memset(patch, 0x90, sizeof patch); /* nop the rest of the replaced prologue */
    memcpy(patch, jmp_abs, 6);
    uint64_t target = (uint64_t)(void *)pad_read_state_hook;
    memcpy(patch + 6, &target, 8);
    DWORD old;
    if (!VirtualProtect(fn, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(fn, patch, sizeof patch);
    VirtualProtect(fn, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof patch);
    logf_("Touchpad fix on: touches get DualShock 4 ids and hold times (gesture menu)");
}

#define LOAD(var, name) (*(FARPROC *)&var = GetProcAddress(real_sdl, name))

static HMODULE self_module;

static BOOL CALLBACK init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    HMODULE self = self_module;
    InitializeCriticalSection(&lock);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    wchar_t real_path[MAX_PATH];
    swprintf(real_path, MAX_PATH, L"%ls\\SDL3_real.dll", path);
    real_sdl = LoadLibraryW(real_path);

    wchar_t data[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BB_DATA_DIR", data, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wcscpy(root_dir, data);
    } else {
        wcscpy(root_dir, path); /* out\ -> package root */
        slash = wcsrchr(root_dir, L'\\');
        if (slash) *slash = 0;
    }
    wchar_t logs[MAX_PATH];
    swprintf(logs, MAX_PATH, L"%ls\\logs", root_dir);
    CreateDirectoryW(logs, NULL);
    swprintf(log_path, MAX_PATH, L"%ls\\kbm-input.log", logs);
    log_file = _wfopen(log_path, L"w");

    wchar_t env[8];
    if (GetEnvironmentVariableW(L"BB_KBM", env, 8) && env[0] == L'0') enabled = false;
    if (!real_sdl) { logf_("SDL3_real.dll not found next to SDL3.dll"); enabled = false; return TRUE; }

    LOAD(r_GetGamepads, "SDL_GetGamepads");
    LOAD(r_OpenGamepad, "SDL_OpenGamepad");
    LOAD(r_CloseGamepad, "SDL_CloseGamepad");
    LOAD(r_GamepadConnected, "SDL_GamepadConnected");
    LOAD(r_GetGamepadName, "SDL_GetGamepadName");
    LOAD(r_GetGamepadButton, "SDL_GetGamepadButton");
    LOAD(r_GetGamepadAxis, "SDL_GetGamepadAxis");
    LOAD(r_GetNumGamepadTouchpads, "SDL_GetNumGamepadTouchpads");
    LOAD(r_GetNumGamepadTouchpadFingers, "SDL_GetNumGamepadTouchpadFingers");
    LOAD(r_GetGamepadTouchpadFinger, "SDL_GetGamepadTouchpadFinger");
    LOAD(r_RumbleGamepad, "SDL_RumbleGamepad");
    LOAD(r_GetKeyboardState, "SDL_GetKeyboardState");
    LOAD(r_PollEvent, "SDL_PollEvent");
    LOAD(r_CreateWindowWithProperties, "SDL_CreateWindowWithProperties");
    LOAD(r_SetWindowRelativeMouseMode, "SDL_SetWindowRelativeMouseMode");
    LOAD(r_malloc, "SDL_malloc");
    LOAD(r_free, "SDL_free");

    logf_("bbport kbm: %s", enabled ? "keyboard & mouse layer on" : "disabled (BB_KBM=0)");
    logf_("Data folder: %ls", root_dir);
    if (enabled) {
        load_config();
        find_menu_flag();
        hook_pad_read_state();
    }
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        self_module = inst;
    }
    return TRUE;
}
