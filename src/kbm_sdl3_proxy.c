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
 * - The runtime's own keyboard handling is hidden (v0.1's fixed layout, v0.4's bind.* keys; it
 *   only sees F9 for pad recording). On v0.4 with PC controls the mouse buttons and the wheel are
 *   kept from it while playing, and its native mouse camera and menu pointer stay in place.
 * - Otherwise the mouse is captured (relative mode) while the game window has focus and the
 *   bbport overlay menu (Insert / L3+R3) is closed. F7 toggles the capture, F8 reloads the config
 *   (hotkey_toggle_mouse_to_joystick / hotkey_reload_inputs in global.ini).
 * - The runtime is recognised by its COFF symbols (v0.1, v0.4 and later builds).
 * - Keys move a stick along its rim (stick_smoothing_ms) instead of jumping: FromSoftware games
 *   play a stumbling turn when the stick direction jumps by 45 degrees while sprinting.
 * - BB_KBM=0 in the environment turns all of this off (pure pass-through).
 *
 * License: GPL-2.0-or-later (see LICENSE).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
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
static bool (*r_GetWindowRelativeMouseMode)(SDL_Window *);
typedef struct SDL_GUID { uint8_t data[16]; } SDL_GUID;
static const char *(*r_GetGamepadNameForID)(SDL_JoystickID);
static SDL_GUID (*r_GetGamepadGUIDForID)(SDL_JoystickID);
static SDL_JoystickID (*r_GetGamepadID)(SDL_Gamepad *);
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

enum { OUT_BUTTON, OUT_AXIS, OUT_HALF_LEFT, OUT_HALF_RIGHT, OUT_JUMP };
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
    float jump_runup_ms; /* jump_runup_ms: sprint before a jump from walking (0 = only while sprinting) */
    bool jump_key_only;  /* jump_key_only: ○ after a sprint always rolls, only the jump key jumps */
    bool menu_mouse_while_moving; /* v0.4: the menu pointer also works while the left stick is tilted */
    int menu_pointer_linger_ms;   /* v0.4: how long a closed menu keeps the pointer (port: 400 ms) */
    int key_toggle_capture, key_reload;
    bool uses_mouse, uses_wheel;
} Config;

static CRITICAL_SECTION lock;
static bool enabled = true;
static Config cfg;
static wchar_t root_dir[MAX_PATH], log_path[MAX_PATH];
static FILE *log_file;

static SDL_Window *game_window;
static volatile bool focused = true, capture_enabled = true, captured, menu_guess;
static volatile const uint8_t *menu_open_flag; /* BbOverlay::menu_open inside bb-probe.exe */
static volatile const uint8_t *prompt_flag;    /* BbOverlay::prompt_active (v0.4+): text box open */
/* v0.4+ runtimes have their own PC controls (bbport.ini pc_controls=1). Then the runtime's key
 * bindings are hidden from it, mouse buttons and the wheel are taken from it while the mouse is
 * captured, and with its mouse_camera=1 the runtime keeps its native mouse camera and owns the
 * capture (the menus keep its mouse pointer). */
static bool runtime_has_pc_controls, pc_controls_mode, native_mouse_camera;
static bool debug_log;                 /* BB_KBM_DEBUG=1: mouse / menu state changes in the log */
static bool (*rt_menu_pointer_in_menu)(void); /* the runtime's own "a game menu is open" (v0.4) */
static void apply_direction_patch(bool on);
static void apply_linger_patch(int ms);
static bool mouse_down[8], down_in_game[8]; /* down_in_game: pressed while the mouse was captured */
static bool last_down_taken; /* the last button press is ours: keep it from the runtime */
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
static bool jump_was_held;
static int jump_mode; /* 0 idle, 1 re-tap of a held sprint, 2 run-up then re-tap */
static double jump_flag_until; /* jump_key_only: when the jump check goes back to "never" */
static bool jump_flag_set;
static volatile bool jump_flag_script;
static void set_jump_flag(bool on);
static double jump_start;
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
    {"jump", OUT_JUMP, 0, 0, 0}, /* bbport extension: a dedicated jump key */
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
    "stick_shape = circle\nstick_smoothing_ms = 70\n\n"
    "# jump = key: a dedicated jump key while sprinting. jump_runup_ms > 0 (about 600) also makes it\n"
    "# start a sprint and jump from walking; 0 = only while sprinting\n"
    "jump_runup_ms = 0\n"
    "# jump_key_only = 1: ○ after a sprint always rolls, only the jump key jumps (on game start)\n"
    "jump_key_only = 0\n\n"
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
    if (!strcmp(out, "menu_pointer_linger_ms")) { /* bbport extension */
        int v = atoi(in);
        if (v >= 0 && v <= 2000) c->menu_pointer_linger_ms = v;
        return;
    }
    if (!strcmp(out, "menu_mouse_while_moving")) { /* bbport extension */
        c->menu_mouse_while_moving = atoi(in) != 0;
        return;
    }
    if (!strcmp(out, "jump_key_only")) { /* bbport extension, read when the game starts */
        c->jump_key_only = atoi(in) != 0;
        return;
    }
    if (!strcmp(out, "jump_runup_ms")) { /* bbport extension */
        float v = (float)atof(in);
        if (v >= 0 && v <= 2000) c->jump_runup_ms = v;
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
    for (int i = 0; i < b.nkeys; i++) {
        if (b.keys[i] >= IN_MOUSE) c->uses_mouse = true;
        if (b.keys[i] >= IN_WHEEL_UP && b.keys[i] <= IN_WHEEL_RIGHT) c->uses_wheel = true;
    }
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
    c->smooth_ms = 70;
    c->jump_runup_ms = 0;
    c->menu_mouse_while_moving = true;
    c->menu_pointer_linger_ms = 80;
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

/* bbport.ini value of `key` (0/1), or `fallback` when missing. BB_CONFIG overrides the path. */
static int ini_flag(const char *key, int fallback) {
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BB_CONFIG", path, MAX_PATH);
    if (!n || n >= MAX_PATH) swprintf(path, MAX_PATH, L"%ls\\bbport.ini", root_dir);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return fallback;
    char line[256];
    size_t len = strlen(key);
    int value = fallback;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!strncmp(s, key, len) && (s[len] == '=' || isspace((unsigned char)s[len]))) {
            char *eq = strchr(s, '=');
            if (eq) value = atoi(trim(eq + 1)) != 0;
        }
    }
    fclose(f);
    return value;
}

static void detect_pc_controls(void) {
    pc_controls_mode = runtime_has_pc_controls && ini_flag("pc_controls", 1);
    native_mouse_camera = pc_controls_mode && ini_flag("mouse_camera", 1);
    if (!runtime_has_pc_controls) return;
    if (!pc_controls_mode)
        logf_("Runtime PC controls are off: this layer drives the keyboard and the mouse "
              "(mouse_to_joystick camera)");
    else if (native_mouse_camera)
        logf_("Runtime PC controls with mouse camera: keys and mouse buttons come from input_config, "
              "the camera and the menu pointer stay native (mouse_to_joystick is ignored; its speed scales "
              "the launcher's sensitivity)");
    else
        logf_("Runtime PC controls without mouse camera: keys and mouse buttons come from input_config, "
              "the mouse camera from mouse_to_joystick");
}

/* ---- input sampling ------------------------------------------------------------------------ */

static bool menu_open(void) {
    if (menu_open_flag) return *menu_open_flag != 0 || (prompt_flag && *prompt_flag != 0);
    return menu_guess;
}

/* Mouse buttons and the wheel count while the mouse is captured: by this layer, or by the runtime's
 * native mouse camera (its relative mode is off in menus, where the mouse is a pointer). */
static bool mouse_in_game(void) {
    if (native_mouse_camera)
        return focused && !menu_open() && game_window && r_GetWindowRelativeMouseMode &&
               r_GetWindowRelativeMouseMode(game_window);
    return captured;
}

/* The wheel also counts in the game's menus when it is bound (e.g. the d-pad on the wheel, to
 * change values when levelling up): there the runtime's pointer would only scroll. Not in the
 * port's own overlay menu. */
static bool wheel_in_game(void) {
    if (mouse_in_game()) return true;
    return native_mouse_camera && cfg.uses_wheel && focused && !menu_open();
}

/* In the game's menus the runtime's pointer uses a left click to click and a right click as Back.
 * A mouse button bound in input_config still counts there when one of its bindings can fire now
 * (e.g. L2 on the right button to switch storage / inventory), except a plain left click, which
 * stays a pointer click; a left click with the other keys of a combination held
 * (r2 = lshift,leftbutton) counts too. Window thread. */
static bool button_in_menu(int button) {
    if (!native_mouse_camera || !focused || menu_open()) return false;
    const bool *ks = r_GetKeyboardState(NULL);
    int code = IN_MOUSE + button;
    for (int i = 0; i < cfg.n; i++) {
        const Binding *b = &cfg.b[i];
        bool has = false, others = true;
        for (int k = 0; k < b->nkeys; k++) {
            int key = b->keys[k];
            if (key == code) has = true;
            else if (key < SC_COUNT) others = others && ks && ks[key];
            else if (key < IN_WHEEL_UP) others = others && mouse_down[key - IN_MOUSE];
            else others = false;
        }
        if (!has || !others) continue;
        if (button != 1 || b->nkeys > 1) return true;
    }
    return false;
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
    if (reload_requested) {
        reload_requested = false;
        load_config();
        detect_pc_controls();
        apply_direction_patch(cfg.menu_mouse_while_moving);
        apply_linger_patch(cfg.menu_pointer_linger_ms);
    }

    Snapshot s = {0};
    bool active = focused && !menu_open();
    bool pressed[IN_COUNT] = {0};
    if (active) {
        const bool *ks = r_GetKeyboardState(NULL);
        if (ks) for (int i = 0; i < SC_COUNT; i++) pressed[i] = ks[i];
    }

    bool wheel_on = active && wheel_in_game();
    EnterCriticalSection(&lock);
    if (active) /* down_in_game: pressed while captured, or a bound button in a game menu */
        for (int i = 1; i <= 5; i++) pressed[IN_MOUSE + i] = mouse_down[i] && down_in_game[i];
    if (wheel_on) {
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
        if ((dx != 0 || dy != 0) && active && captured && !native_mouse_camera) {
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
    bool used[IN_COUNT] = {0}, half_l = false, half_r = false, jump_held = false;
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
        case OUT_JUMP: jump_held = true; break;
        }
    }
    /* Jump: Bloodborne jumps on the release of a quick ○ tap while sprinting. The jump key plays
     * it: ○ off for 60 ms, on for 90 ms, off for 60 ms, then back to the ○ key's own state (a held
     * sprint goes on after landing). From walking (left stick tilted, ○ not held) ○ is first held
     * for jump_runup_ms to start the sprint. */
    if (jump_held && !jump_was_held && !jump_mode) {
        float lx = axes[AX_LX], ly = axes[AX_LY];
        if (s.button[BTN_EAST]) { jump_mode = 1; jump_start = t; }
        else if (lx * lx + ly * ly > 60.0f * 60.0f && cfg.jump_runup_ms > 0) { jump_mode = 2; jump_start = t; }
        /* jump_key_only: the patched player script jumps on this tap only */
        if (jump_mode && jump_flag_script) { set_jump_flag(true); jump_flag_set = true; }
    }
    jump_was_held = jump_held;
    if (jump_mode) {
        double e = t - jump_start - (jump_mode == 2 ? cfg.jump_runup_ms : 0);
        if (e < 0) s.button[BTN_EAST] = true;
        else if (e < 60) s.button[BTN_EAST] = false;
        else if (e < 150) s.button[BTN_EAST] = true;
        else if (e < 210) s.button[BTN_EAST] = false;
        else { jump_mode = 0; jump_flag_until = t + 500; } /* the game acts on the tap a few hundred ms later */
    }
    if (jump_flag_set && !jump_mode && t > jump_flag_until) { set_jump_flag(false); jump_flag_set = false; }

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
    if (!game_window || !r_SetWindowRelativeMouseMode || native_mouse_camera) return; /* runtime owns it */
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
            bool down = e->type == EV_MOUSE_BUTTON_DOWN;
            /* a click in a menu that closes it does not turn into an attack */
            bool in_game = down && (mouse_in_game() || button_in_menu(b));
            if (down) last_down_taken = in_game;
            EnterCriticalSection(&lock);
            mouse_down[b] = down;
            down_in_game[b] = in_game;
            LeaveCriticalSection(&lock);
        }
        break;
    }
    case EV_MOUSE_WHEEL: {
        float x = *(const float *)(p + 24), y = *(const float *)(p + 28);
        if (*(const uint32_t *)(p + 32) == 1) { x = -x; y = -y; } /* SDL_MOUSEWHEEL_FLIPPED */
        if (wheel_in_game()) {
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

/* v0.4+ picks the pad by name / GUID (BB_GAMEPAD, the launcher's controller choice) before
 * opening it, and compares the name as a string: the virtual pad must have both. */
EXPORT const char *SDL_GetGamepadNameForID(SDL_JoystickID id) {
    ENSURE();
    if (enabled && id == FAKE_ID) return "Keyboard & Mouse (bbport kbm)";
    return r_GetGamepadNameForID(id);
}

EXPORT SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID id) {
    ENSURE();
    if (enabled && id == FAKE_ID) {
        SDL_GUID g = {{'b', 'b', 'p', 'o', 'r', 't', '-', 'k', 'b', 'm', 0, 0, 0, 0, 0, 1}};
        return g;
    }
    return r_GetGamepadGUIDForID(id);
}

EXPORT SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g) {
    ENSURE();
    if (g == FAKE_PAD) return FAKE_ID;
    return r_GetGamepadID(g);
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

/* With the runtime's PC controls on, its own bindings must not see the mouse buttons and the wheel
 * while playing (they come from input_config here). Releases always pass, so nothing sticks. */
static bool hide_from_runtime(const SDL_Event *e) {
    if (!pc_controls_mode) return false;
    if (e->type == EV_MOUSE_BUTTON_DOWN) return last_down_taken; /* set by handle_event */
    if (e->type == EV_MOUSE_WHEEL) return wheel_in_game();
    return false;
}

/* BB_KBM_DEBUG=1 diagnostics (window thread). */
static void debug_event(const SDL_Event *e, bool hidden) {
    const uint8_t *p = (const uint8_t *)e;
    if (e->type == EV_MOUSE_BUTTON_DOWN || e->type == EV_MOUSE_BUTTON_UP)
        logf_("dbg %.0f: mouse button %u %s -> %s", now_ms(), p[24], e->type == EV_MOUSE_BUTTON_DOWN ? "down" : "up",
              hidden ? "kept (binding)" : "runtime");
    else if (e->type == EV_MOUSE_WHEEL)
        logf_("dbg %.0f: wheel -> %s", now_ms(), hidden ? "kept (binding)" : "runtime");
}

static void debug_state(void) {
    static int last = -1;
    int rel = game_window && r_GetWindowRelativeMouseMode ? r_GetWindowRelativeMouseMode(game_window) : -1;
    int in_menu = rt_menu_pointer_in_menu ? rt_menu_pointer_in_menu() : -1;
    int state = (rel + 1) | (in_menu + 1) << 2 | focused << 4 | menu_open() << 5 | mouse_in_game() << 6;
    if (state != last) {
        logf_("dbg %.0f: relative=%d game_menu=%d focused=%d overlay=%d mouse_in_game=%d stick L=(%d,%d)", now_ms(), rel,
              in_menu, focused, menu_open(), mouse_in_game(), snap.axis[AX_LX] / 256, snap.axis[AX_LY] / 256);
        last = state;
    }
}

/* The runtime's native mouse camera adds the relative motion of these events (xrel/yrel, floats)
 * to its turn counts, at the launcher's mouse_sensitivity; the speed of mouse_movement_params
 * scales it on top (1 = as in the launcher). The menu pointer uses the absolute position. */
static void scale_native_motion(SDL_Event *e) {
    if (!native_mouse_camera || e->type != EV_MOUSE_MOTION || cfg.speed == 1.0f) return;
    float k = cfg.speed < 0.1f ? 0.1f : cfg.speed;
    uint8_t *p = (uint8_t *)e;
    *(float *)(p + 36) *= k;
    *(float *)(p + 40) *= k;
}

EXPORT bool SDL_PollEvent(SDL_Event *e) {
    ENSURE();
    for (;;) {
        bool r = r_PollEvent(e);
        if (enabled) {
            if (r && e) {
                handle_event(e);
                scale_native_motion(e);
                bool hidden = hide_from_runtime(e);
                if (debug_log) debug_event(e, hidden);
                if (hidden) continue;
            }
            update_capture();
            if (debug_log) debug_state();
        }
        return r;
    }
}

EXPORT SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
    ENSURE();
    SDL_Window *w = r_CreateWindowWithProperties(props);
    if (w && !game_window) game_window = w;
    return w;
}

/* ---- setup --------------------------------------------------------------------------------- */

/* The runtime is located through the COFF symbol table that bb-probe.exe builds keep (read from
 * the file on disk), so the same layer works on v0.1 and v0.4: the overlay menu / text prompt
 * flags, pad_read_state, whether PC controls (bbgpu_pc_input) and the touch id fix (touch_ids)
 * exist. Without symbols the v0.1 byte signature is the fallback. */
static uint32_t rva_pad_read_state;
static bool runtime_has_touch_ids;

typedef struct { const char *prefix; uint32_t *rva; } SymWant;

static void scan_symbols(const SymWant *want, int nwant) {
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    DWORD nsec = nt->FileHeader.NumberOfSections;
    DWORD symptr = nt->FileHeader.PointerToSymbolTable, nsym = nt->FileHeader.NumberOfSymbols;
    if (!symptr || !nsym) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    uint64_t table = (uint64_t)nsym * 18;
    if (symptr + table + 4 > (uint64_t)size.QuadPart) { CloseHandle(f); return; }
    uint64_t total = (uint64_t)size.QuadPart - symptr; /* symbols + string table up to the end */
    if (total > (64u << 20)) total = 64u << 20;
    uint8_t *buf = malloc((size_t)total);
    DWORD got = 0;
    LARGE_INTEGER at = {.QuadPart = symptr};
    bool ok = buf && SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, buf, (DWORD)total, &got, NULL) &&
              got == total;
    CloseHandle(f);
    if (!ok) { free(buf); return; }
    const char *strtab = (const char *)buf + table;
    uint64_t strsize = total - table;
    for (DWORD i = 0; i < nsym; i++) {
        const uint8_t *s = buf + (uint64_t)i * 18;
        uint8_t naux = s[17];
        int16_t secno;
        memcpy(&secno, s + 12, 2);
        if (secno > 0 && (DWORD)secno <= nsec) {
            char shortname[9] = {0};
            const char *name;
            uint32_t zero, off;
            memcpy(&zero, s, 4);
            memcpy(&off, s + 4, 4);
            if (zero == 0) name = off < strsize ? strtab + off : "";
            else { memcpy(shortname, s, 8); name = shortname; }
            uint32_t value;
            memcpy(&value, s + 8, 4);
            for (int w = 0; w < nwant; w++)
                if (!*want[w].rva && !strncmp(name, want[w].prefix, strlen(want[w].prefix)))
                    *want[w].rva = sec[secno - 1].VirtualAddress + value;
        }
        i += naux;
    }
    free(buf);
}

/* ---- menu pointer while moving (v0.4) --------------------------------------------------------
 * The port's menu pointer (gpu/shim/input/menu_pointer.cpp, pc_list_update) ignores hovering and
 * clicks while direction_held() is true: a d-pad button, or the left stick tilted past half. With
 * movement on the left stick, the inventory could not be used with the mouse while walking. The
 * stick part of direction_held is replaced by "not held" (xor eax, eax + nops); the d-pad part stays,
 * so d-pad navigation still wins over the pointer. Matched by its bytes; undone when switched off. */
static uint8_t *dir_patch_at;
static uint8_t dir_patch_orig[19];
static bool dir_patch_on;

static void find_direction_patch(uint8_t *fn) {
    /* mov al,1; test r14b,0xf0; jne +0x13; <19 bytes: left stick x / y outside 0x41..0xbf -> al> */
    static const uint8_t sig[] = {0xb0, 0x01, 0x41, 0xf6, 0xc6, 0xf0, 0x75, 0x13, 0x40, 0x80, 0xc7, 0x3f, 0x40, 0x80, 0xff,
                                  0x7f, 0x72, 0x09, 0x80, 0xc3, 0x3f, 0x80, 0xfb, 0x7f, 0x0f, 0x92, 0xc0};
    for (int i = 0; i < 0x80; i++) {
        if (!memcmp(fn + i, sig, sizeof sig)) {
            dir_patch_at = fn + i + 8;
            memcpy(dir_patch_orig, dir_patch_at, sizeof dir_patch_orig);
            return;
        }
    }
    logf_("Menu pointer: direction_held not recognised, the pointer stays off while moving");
}

static void apply_direction_patch(bool on) {
    if (!dir_patch_at || on == dir_patch_on) return;
    uint8_t bytes[sizeof dir_patch_orig];
    if (on) {
        memset(bytes, 0x90, sizeof bytes);
        bytes[0] = 0x31; bytes[1] = 0xc0; /* xor eax, eax: the stick never counts as a held direction */
    } else {
        memcpy(bytes, dir_patch_orig, sizeof bytes);
    }
    DWORD old;
    if (!VirtualProtect(dir_patch_at, sizeof bytes, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(dir_patch_at, bytes, sizeof bytes);
    VirtualProtect(dir_patch_at, sizeof bytes, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dir_patch_at, sizeof bytes);
    dir_patch_on = on;
    logf_("Menu pointer while moving: %s", on ? "on (the left stick no longer blocks it)" : "off (as in the port)");
}

/* ---- pointer linger after a menu closes (v0.4) ----------------------------------------------
 * menu_pointer_in_menu() keeps reporting a menu for 400 ms after the last list update (cmp rdx,
 * 0x190), so the pointer stayed on screen after closing the inventory. The constant is patched to
 * menu_pointer_linger_ms (default 80 ms: more than two frames at 30 FPS, so menus do not flicker). */
static uint8_t *linger_imm;

static void find_linger_patch(uint8_t *fn) {
    static const uint8_t sig[] = {0x48, 0x81, 0xfa, 0x90, 0x01, 0x00, 0x00, 0x0f, 0x9c, 0xc0}; /* cmp rdx,400; setl al */
    for (int i = 0; i < 0x200; i++)
        if (!memcmp(fn + i, sig, sizeof sig)) { linger_imm = fn + i + 3; return; }
    logf_("Menu pointer: linger constant not recognised, the pointer stays 400 ms after menus");
}

static void apply_linger_patch(int ms) {
    if (!linger_imm) return;
    int32_t cur;
    memcpy(&cur, linger_imm, 4);
    if (cur == ms) return;
    DWORD old;
    if (!VirtualProtect(linger_imm, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(linger_imm, &ms, 4);
    VirtualProtect(linger_imm, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), linger_imm, 4);
    logf_("Menu pointer: hidden %d ms after a menu closes (port: 400)", ms);
}

/* jump_key_only. Bloodborne's player script (action/script/c0000.hks, HavokScript bytecode)
 * decides in Act_AttackRight: a roll request (Arm_Rolling, the release of a quick ○ tap) while
 * special effect 390 is on (a sprint and a moment after it) fires W_Jump, otherwise a roll. When
 * the runtime opens that file it gets a copy in which the jump checks for a special effect id held
 * in a new constant instead of 390. The constant names no effect (JUMP_FLAG_OFF), so ○ after a
 * sprint always rolls, at once. While the jump key plays its ○ tap the constant is set to 390 in
 * the loaded script (found in the game's memory next to the second new constant, JUMP_FLAG_MARK),
 * so that one tap jumps. The game's files are not changed: the copy is out\kbm-c0000-jump.hks,
 * made from whatever file the game opens (a mod's player script works the same way). */
#define JUMP_FLAG_OFF -73191.0f
#define JUMP_FLAG_MARK -73192.0f
#define JUMP_FLAG_ON 390.0f

typedef struct { const uint8_t *d; size_t n, o; bool bad; } HksReader;

static uint32_t hks_u32(HksReader *r) {
    if (r->o + 4 > r->n) { r->bad = true; return 0; }
    const uint8_t *p = r->d + r->o;
    r->o += 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t hks_u8(HksReader *r) {
    if (r->o + 1 > r->n) { r->bad = true; return 0; }
    return r->d[r->o++];
}

/* A string constant: 8-byte big-endian length including the terminator, then the bytes. */
static const char *hks_str(HksReader *r, size_t *len) {
    uint32_t hi = hks_u32(r), lo = hks_u32(r);
    if (hi || lo > r->n - r->o) { r->bad = true; return NULL; }
    const char *s = (const char *)r->d + r->o;
    r->o += lo;
    *len = lo;
    return lo ? s : NULL;
}

typedef struct { size_t code, nk_at, k_end; uint32_t nk, loadk; bool found; } HksSite;

#define HKS_OP(i) ((i) >> 25)
#define HKS_A(i) ((i) & 0xff)
#define HKS_BX(i) (((i) >> 8) & 0x1ffff)
#define HKS_GETGLOBAL_MEM 91
#define HKS_LOADK 25

static uint32_t hks_word(const uint8_t *d, size_t at) {
    return (uint32_t)d[at] << 24 | (uint32_t)d[at + 1] << 16 | (uint32_t)d[at + 2] << 8 | d[at + 3];
}

static void hks_function(HksReader *r, HksSite *site, int depth) {
    if (depth > 8) { r->bad = true; return; }
    hks_u32(r); hks_u32(r); hks_u8(r); hks_u32(r); hks_u32(r); /* upvalues, params, vararg, stack, ? */
    uint32_t ninstr = hks_u32(r);
    r->o = (r->o + 3) & ~(size_t)3;
    size_t code = r->o;
    if (r->bad || r->o > r->n || ninstr > (r->n - r->o) / 4) { r->bad = true; return; }
    r->o += (size_t)ninstr * 4;
    size_t nk_at = r->o;
    uint32_t nk = hks_u32(r);
    int k_rolling = -1, k_390 = -1, k_jump = -1;
    for (uint32_t i = 0; i < nk && !r->bad; i++) {
        size_t len;
        const char *s;
        switch (hks_u8(r)) {
        case 0: break;
        case 1: hks_u8(r); break;
        case 3: if (hks_u32(r) == 0x43c30000) k_390 = (int)i; break; /* 390.0f */
        case 4:
            s = hks_str(r, &len);
            if (s && len == 12 && !memcmp(s, "Arm_Rolling", 12)) k_rolling = (int)i;
            if (s && len == 7 && !memcmp(s, "W_Jump", 7)) k_jump = (int)i;
            break;
        default: r->bad = true;
        }
    }
    size_t k_end = r->o;
    if (hks_u32(r)) { /* debug info */
        uint32_t nlines = hks_u32(r), nlocals = hks_u32(r), nups = hks_u32(r);
        size_t len;
        hks_u32(r); hks_u32(r);
        hks_str(r, &len); hks_str(r, &len);
        r->o += (size_t)nlines * 4;
        for (uint32_t i = 0; i < nlocals && !r->bad; i++) { hks_str(r, &len); hks_u32(r); hks_u32(r); }
        for (uint32_t i = 0; i < nups && !r->bad; i++) hks_str(r, &len);
    }
    /* the jump check: GETGLOBAL_MEM r2 'Arm_Rolling', LOADK r2 390 soon after, then LOADK 'W_Jump' */
    if (!r->bad && !site->found && k_rolling >= 0 && k_390 >= 0 && k_jump >= 0) {
        for (uint32_t pc = 0; pc + 25 < ninstr && !site->found; pc++) {
            uint32_t w = hks_word(r->d, code + pc * 4);
            if (HKS_OP(w) != HKS_GETGLOBAL_MEM || HKS_A(w) != 2 || (int)HKS_BX(w) != k_rolling) continue;
            uint32_t loadk = 0;
            for (uint32_t q = pc; q < pc + 25; q++) {
                uint32_t x = hks_word(r->d, code + q * 4);
                if (HKS_OP(x) != HKS_LOADK) continue;
                if ((int)HKS_BX(x) == k_390 && q < pc + 12) loadk = q;
                if ((int)HKS_BX(x) == k_jump && loadk) {
                    *site = (HksSite){code, nk_at, k_end, nk, loadk, true};
                    break;
                }
            }
        }
    }
    uint32_t nchildren = hks_u32(r);
    for (uint32_t i = 0; i < nchildren && !r->bad; i++) hks_function(r, site, depth + 1);
}

static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* The patched script, or NULL. Four constants are added: JUMP_FLAG_OFF (the jump check now loads
 * it in place of 390), JUMP_FLAG_MARK and two nils, 12 bytes, so the 4-byte alignment of every
 * later function's code stays as it is. */
static uint8_t *hks_patch(const uint8_t *d, size_t n, size_t *out_n) {
    static const uint8_t magic[] = {0x1b, 'L', 'u', 'a', 0x51, 0x0e};
    if (n < 0x100 || memcmp(d, magic, sizeof magic)) return NULL;
    HksReader r = {d, n, 14, false}; /* after the 14-byte header: the type table */
    uint32_t ntypes = hks_u32(&r);
    for (uint32_t i = 0; i < ntypes && !r.bad; i++) { hks_u32(&r); r.o += hks_u32(&r); }
    HksSite site = {0};
    hks_function(&r, &site, 0);
    if (r.bad || !site.found) return NULL;
    uint8_t add[12] = {3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0};
    float off = JUMP_FLAG_OFF, mark = JUMP_FLAG_MARK;
    uint32_t u;
    memcpy(&u, &off, 4); put_be32(add + 1, u);
    memcpy(&u, &mark, 4); put_be32(add + 6, u);
    uint8_t *o = malloc(n + sizeof add);
    if (!o) return NULL;
    memcpy(o, d, site.k_end);
    memcpy(o + site.k_end, add, sizeof add);
    memcpy(o + site.k_end + sizeof add, d + site.k_end, n - site.k_end);
    put_be32(o + site.nk_at, site.nk + 4);
    size_t at = site.code + (size_t)site.loadk * 4;
    put_be32(o + at, (hks_word(o, at) & ~(0x1ffffu << 8)) | site.nk << 8);
    *out_n = n + sizeof add;
    return o;
}

/* ---- the loaded script's jump constant ---- */

typedef int(__cdecl *OpenFn)(const char *, int, int);
typedef int(__cdecl *Stat64Fn)(const char *, void *);
static OpenFn real_open;
static Stat64Fn real_stat64;
static SRWLOCK hks_lock = SRWLOCK_INIT;
static char hks_patched_path[MAX_PATH * 3];
static bool hks_tried;
static HANDLE scan_event;
static volatile bool scan_wanted; /* a jump found no loaded copy */

/* Every loaded copy of the jump constant (the player's and other c0000 characters'), by the
 * address of its value. A HavokScript constant is 16 bytes: type (3 = number) at +0, value at +8;
 * the marker constant follows. */
#define MAX_JUMP_SLOTS 16
static float *jump_slots[MAX_JUMP_SLOTS];
static volatile int jump_nslots;
static volatile bool jump_flag_on;

static bool jump_slot_valid(const float *v) {
    const uint32_t *w = (const uint32_t *)v;
    return w[-2] == 3 && w[2] == 3 && v[4] == JUMP_FLAG_MARK && (*v == JUMP_FLAG_OFF || *v == JUMP_FLAG_ON);
}

typedef BOOL(WINAPI *QueryWorkingSetExFn)(HANDLE, PVOID, DWORD);
static QueryWorkingSetExFn query_ws;

typedef struct { float *found[MAX_JUMP_SLOTS]; int n; uint8_t *where; } JumpScan;

/* Looks through the resident pages of [lo, hi): pages the game has not touched are skipped, so
 * reading does not grow its memory. */
static void scan_pages(uint8_t *lo, uint8_t *hi, JumpScan *js, uint8_t *alloc) {
    enum { PAGES = 256 };
    static PSAPI_WORKING_SET_EX_INFORMATION ws[PAGES];
    static uint8_t buf[PAGES * 4096];
    float off = JUMP_FLAG_OFF, mark = JUMP_FLAG_MARK;
    uint32_t uoff, umark;
    memcpy(&uoff, &off, 4);
    memcpy(&umark, &mark, 4);
    for (uint8_t *p = lo; p < hi && js->n < MAX_JUMP_SLOTS; p += PAGES * 4096) {
        int np = (size_t)(hi - p) / 4096 < PAGES ? (int)((size_t)(hi - p) / 4096) : PAGES;
        for (int i = 0; i < np; i++) ws[i].VirtualAddress = p + (size_t)i * 4096;
        if (!query_ws || !query_ws(GetCurrentProcess(), ws, sizeof ws[0] * np)) continue;
        for (int i = 0; i < np; i++) {
            if (!ws[i].VirtualAttributes.Valid) continue;
            int j = i;
            while (j < np && ws[j].VirtualAttributes.Valid) j++;
            SIZE_T got = 0;
            uint8_t *from = p + (size_t)i * 4096;
            ReadProcessMemory(GetCurrentProcess(), from, buf, (size_t)(j - i) * 4096, &got);
            /* a constant whose marker lies on the next, non-resident page is missed; the next
             * scan, after the game touches it, finds it */
            for (size_t k = 8; k + 24 <= got && js->n < MAX_JUMP_SLOTS; k += 8) {
                uint32_t v, t, t2, m;
                memcpy(&v, buf + k, 4);
                if (v != uoff) continue;
                memcpy(&t, buf + k - 8, 4); memcpy(&t2, buf + k + 8, 4); memcpy(&m, buf + k + 16, 4);
                if (t == 3 && t2 == 3 && m == umark) { js->found[js->n++] = (float *)(from + k); js->where = alloc; }
            }
            i = j;
        }
    }
}

/* The committed, readable, mapped regions of one allocation (base != NULL), or of the game's
 * memory (base == NULL: mapped allocations at 64 GiB .. 1 TiB, the runtime's first view of the
 * PS4 memory). */
static void scan_memory(uint8_t *base, JumpScan *js) {
    uint8_t *p = base ? base : (uint8_t *)0x1000000000ull;
    uint8_t *end = base ? (uint8_t *)UINTPTR_MAX : (uint8_t *)0x10000000000ull;
    MEMORY_BASIC_INFORMATION mi;
    while (p < end && js->n < MAX_JUMP_SLOTS && VirtualQuery(p, &mi, sizeof mi)) {
        if (base && mi.AllocationBase != base) break;
        if (mi.State == MEM_COMMIT && mi.Type == MEM_MAPPED && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            scan_pages(mi.BaseAddress, (uint8_t *)mi.BaseAddress + mi.RegionSize, js, mi.AllocationBase);
        p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
}

/* Background thread. The loaded copies move when the game loads an area, so they are checked
 * every few seconds and searched for again: first in the allocation where they were last found
 * (remembered in out\kbm-jump-scan.txt: the game's memory layout repeats from run to run), then,
 * after a jump that found none or every 30 s while none is known, in all of the game's memory. */
static DWORD WINAPI jump_scan_thread(LPVOID unused) {
    (void)unused;
    wchar_t hint_path[MAX_PATH];
    swprintf(hint_path, MAX_PATH, L"%ls\\out\\kbm-jump-scan.txt", root_dir);
    uint8_t *hint = NULL;
    FILE *f = _wfopen(hint_path, L"r");
    if (f) {
        unsigned long long v;
        if (fscanf(f, "%llx", &v) == 1) hint = (uint8_t *)(uintptr_t)v;
        fclose(f);
    }
    query_ws = (QueryWorkingSetExFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32QueryWorkingSetEx");
    double last_full = now_ms();
    int last_n = -1;
    for (;;) {
        WaitForSingleObject(scan_event, 4000);
        bool known = false;
        AcquireSRWLockShared(&hks_lock);
        for (int i = 0; i < jump_nslots && !known; i++) known = jump_slot_valid(jump_slots[i]);
        ReleaseSRWLockShared(&hks_lock);
        if (known) continue;
        JumpScan js = {0};
        if (hint) scan_memory(hint, &js);
        bool full = !js.n && (scan_wanted || now_ms() - last_full > 30000);
        if (full) {
            double t0 = now_ms();
            scan_memory(NULL, &js);
            last_full = now_ms();
            logf_("Jump key only: searched the game's memory in %.0f ms", last_full - t0);
            if (js.n && js.where != hint) {
                hint = js.where;
                FILE *w = _wfopen(hint_path, L"w");
                if (w) { fprintf(w, "%llx\n", (unsigned long long)(uintptr_t)hint); fclose(w); }
            }
        }
        scan_wanted = false;
        AcquireSRWLockExclusive(&hks_lock);
        memcpy(jump_slots, js.found, sizeof(float *) * js.n);
        jump_nslots = js.n;
        ReleaseSRWLockExclusive(&hks_lock);
        if (js.n != last_n || full) logf_("Jump key only: %d loaded copies of the jump check", js.n);
        last_n = js.n;
    }
    return 0;
}

static void set_jump_flag(bool on) {
    if (on == jump_flag_on) return;
    jump_flag_on = on;
    int done = 0;
    AcquireSRWLockShared(&hks_lock);
    for (int i = 0; i < jump_nslots; i++)
        if (jump_slot_valid(jump_slots[i])) { *(volatile float *)jump_slots[i] = on ? JUMP_FLAG_ON : JUMP_FLAG_OFF; done++; }
    ReleaseSRWLockShared(&hks_lock);
    if (on && !done && scan_event) { scan_wanted = true; SetEvent(scan_event); } /* look for it now */
}

static bool is_player_script(const char *path) {
    static const char tail[] = "action/script/c0000.hks";
    size_t n = strlen(path), m = sizeof tail - 1;
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        char a = (char)tolower((unsigned char)path[n - m + i]);
        if ((a == '\\' ? '/' : a) != tail[i]) return false;
    }
    return true;
}

/* The patched copy of `path` (made once per game start), or NULL to open the original. */
static const char *player_script_redirect(const char *path) {
    if (!is_player_script(path)) return NULL;
    ENSURE();
    if (!enabled || !cfg.jump_key_only) return NULL;
    AcquireSRWLockExclusive(&hks_lock);
    if (!hks_tried) {
        hks_tried = true;
        uint8_t *d = NULL, *o = NULL;
        size_t n = 0, on = 0;
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0 && len < (64 << 20) && (d = malloc((size_t)len)) && fread(d, 1, (size_t)len, f) == (size_t)len)
                n = (size_t)len;
            fclose(f);
        }
        if (n) o = hks_patch(d, n, &on);
        if (o) {
            char out[MAX_PATH * 3];
            snprintf(out, sizeof out, "%ls\\out\\kbm-c0000-jump.hks", root_dir);
            FILE *w = fopen(out, "wb");
            bool ok = w && fwrite(o, 1, on, w) == on;
            if (w && fclose(w)) ok = false;
            if (ok) {
                strcpy(hks_patched_path, out);
                jump_flag_script = true;
                scan_event = CreateEventW(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, jump_scan_thread, NULL, 0, NULL));
                logf_("Jump key only: circle after a sprint rolls, the jump key jumps (player script patched)");
            } else {
                logf_("Jump key only: could not write %s", out);
            }
        } else {
            logf_("Jump key only: the player script was not recognised (%s), circle jumps as in the game", path);
        }
        free(d);
        free(o);
    }
    ReleaseSRWLockExclusive(&hks_lock);
    return hks_patched_path[0] ? hks_patched_path : NULL;
}

static int __cdecl open_hook(const char *path, int flags, int mode) {
    const char *to = path && (flags & 3) == 0 /* _O_RDONLY */ ? player_script_redirect(path) : NULL;
    return real_open(to ? to : path, flags, mode);
}

static int __cdecl stat64_hook(const char *path, void *st) {
    const char *to = path ? player_script_redirect(path) : NULL;
    return real_stat64(to ? to : path, st);
}

/* Replaces bb-probe.exe's import of `name`: *orig gets the original function first. */
static bool hook_import(const char *name, void *hook, void **orig) {
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *by = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)by->Name, name)) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return false;
            *orig = (void *)slots->u1.Function;
            slots->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            return true;
        }
    }
    return false;
}

/* Installed while the runtime loads (DllMain): the game may read its scripts before the first SDL
 * call this layer sees. The hooks change only c0000.hks and only with jump_key_only = 1. */
static void hook_file_opens(void) {
    hook_import("_open", (void *)open_hook, (void **)&real_open);
    hook_import("_stat64", (void *)stat64_hook, (void **)&real_stat64);
}

static void find_runtime(void) {
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    if (!base) return;
    uint32_t menu = 0, prompt = 0, pc_input = 0, touch = 0, in_menu_fn = 0, dir_held = 0;
    SymWant want[] = {
        {"_ZN9BbOverlay12_GLOBAL__N_19menu_openE", &menu},
        {"_ZN9BbOverlay12_GLOBAL__N_113prompt_activeE", &prompt},
        {"pad_read_state", &rva_pad_read_state},
        {"bbgpu_pc_input", &pc_input},
        {"touch_ids", &touch},
        {"_Z20menu_pointer_in_menuv", &in_menu_fn},
        {"_ZN12_GLOBAL__N_114direction_heldEv", &dir_held},
    };
    scan_symbols(want, sizeof want / sizeof *want);
    runtime_has_pc_controls = pc_input != 0;
    runtime_has_touch_ids = touch != 0;
    if (in_menu_fn) rt_menu_pointer_in_menu = (bool (*)(void))(void *)(base + in_menu_fn);
    if (in_menu_fn) find_linger_patch(base + in_menu_fn);
    if (dir_held) find_direction_patch(base + dir_held);
    if (menu) {
        menu_open_flag = base + menu;
        if (prompt) prompt_flag = base + prompt;
        logf_("Runtime symbols: overlay menu flag%s, pad_read_state %s, PC controls %s, touch ids %s",
              prompt ? " + text prompt flag" : "", rva_pad_read_state ? "found" : "missing",
              runtime_has_pc_controls ? "yes" : "no", runtime_has_touch_ids ? "fixed" : "missing");
        return;
    }
    /* bb-probe.exe of Bloodborne PC Offline v0.1 without symbols: pad_read_state checks
     * BbOverlay::menu_open with `movzx eax, byte ptr [rip+disp32]` at RVA 0x40c4e. */
    static const uint8_t sig[] = {0x0f, 0xb6, 0x05, 0x2e, 0xb1, 0x1f, 0x03};
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uint8_t *ins = base + 0x40c4e;
    if (nt->OptionalHeader.SizeOfImage > 0x40c4e + 16 && !memcmp(ins, sig, sizeof sig)) {
        menu_open_flag = ins + 7 + *(int32_t *)(ins + 3);
        rva_pad_read_state = 0x40b20;
        logf_("Overlay menu flag found by signature (v0.1)");
    } else {
        logf_("Runtime not recognised: Insert toggles the mouse release, no touchpad fix");
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
    /* v0.1 pad_read_state: push rbp/r15/r14/r13/r12/rbx; sub rsp, 0xe8 (17 bytes, no RIP-relative
     * operands). Only hooked when the runtime lacks touch_ids and these bytes match. */
    static const uint8_t prologue[17] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                         0x53, 0x48, 0x81, 0xec, 0xe8, 0x00, 0x00, 0x00};
    if (runtime_has_touch_ids) return; /* fixed in the runtime itself (bbport 0.4+) */
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    uint8_t *fn = base + rva_pad_read_state;
    if (!rva_pad_read_state || memcmp(fn, prologue, sizeof prologue)) {
        logf_("pad_read_state not recognised: touchpad ids left as they are");
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
    if (GetEnvironmentVariableW(L"BB_KBM_DEBUG", env, 8) && env[0] == L'1') debug_log = true;
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
    LOAD(r_GetWindowRelativeMouseMode, "SDL_GetWindowRelativeMouseMode");
    LOAD(r_GetGamepadNameForID, "SDL_GetGamepadNameForID");
    LOAD(r_GetGamepadGUIDForID, "SDL_GetGamepadGUIDForID");
    LOAD(r_GetGamepadID, "SDL_GetGamepadID");
    LOAD(r_malloc, "SDL_malloc");
    LOAD(r_free, "SDL_free");

    logf_("bbport kbm: %s", enabled ? "keyboard & mouse layer on" : "disabled (BB_KBM=0)");
    logf_("Data folder: %ls", root_dir);
    if (enabled) {
        load_config();
        find_runtime();
        detect_pc_controls();
        hook_pad_read_state();
        apply_direction_patch(cfg.menu_mouse_while_moving);
        apply_linger_patch(cfg.menu_pointer_linger_ms);
    }
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        self_module = inst;
        hook_file_opens();
    }
    return TRUE;
}
