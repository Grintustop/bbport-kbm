/* Keyboard test: prints every key press/release with a timestamp, to find keys that the keyboard
 * drops when several are held (ghosting), e.g. W + Space + A. Close the window to exit. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>

static LARGE_INTEGER freq, start;
static bool down[512];

static void print_held(void) {
    char name[64];
    printf("   held:");
    for (int i = 0; i < 512; i++)
        if (down[i] && GetKeyNameTextA((LONG)((i & 0xFF) << 16 | (i & 0x100) << 16), name, sizeof name)) printf(" [%s]", name);
    printf("\n");
}

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) {
        int sc = (int)((lp >> 16) & 0xFF) | (((lp >> 24) & 1) ? 0x100 : 0);
        bool press = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
        if (press && (lp >> 30) & 1) return 0; /* auto-repeat */
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        char name[64] = "?";
        GetKeyNameTextA((LONG)(lp & 0x01FF0000), name, sizeof name);
        down[sc] = press;
        printf("%8.0f ms  %s %-12s", (double)(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart,
               press ? "НАЖАТА  " : "отпущена", name);
        print_held();
        return 0;
    }
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(w, m, wp, lp);
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    printf("Кликните в окно \"Key test\" и повторите: зажмите W, потом Space, потом A (и то же с D).\n"
           "Если при нажатии A сама собой \"отпускается\" Space или W - это ghosting клавиатуры.\n\n");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"keytest";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"keytest", L"Key test - нажимайте клавиши здесь", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 480, 200, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(w);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
