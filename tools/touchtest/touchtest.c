/*
 * touchtest.c - WM_TOUCH / WM_POINTER verification harness for GameNative
 * native multitouch mode.
 *
 * Build (mingw-w64):
 *   x86_64-w64-mingw32-gcc -O2 -D_WIN32_WINNT=0x0602 touchtest.c -o touchtest.exe -lgdi32 -luser32
 *
 * What it does:
 *  - Prints digitizer capabilities at startup (SM_DIGITIZER / SM_MAXIMUMTOUCHES).
 *    NOTE: the bundled proton-wine does not implement SM_DIGITIZER, so 0 here
 *    does NOT mean touch is broken - it only matters for games that gate touch
 *    handling on this metric.
 *  - Calls RegisterTouchWindow() so win32u synthesizes WM_TOUCH.
 *  - Logs every WM_TOUCH contact (id, position, down/move/up flags) and every
 *    WM_POINTERDOWN/UPDATE/UP (pointer id, type, position) to stdout.
 *  - Draws a colored circle per active contact so you can see 10-point
 *    tracking without reading logs.
 *
 * Expected result with native touch mode working: each finger gets its own
 * stable id from DOWN through MOVEs to UP, and simultaneous fingers appear as
 * simultaneous distinct contacts.
 *
 * Run inside the container (e.g. add as a custom executable, or
 * `wine touchtest.exe` from a shell). Logs go to the wine console/log.
 */

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>

#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#define WM_POINTERDOWN   0x0246
#define WM_POINTERUP     0x0247
#endif
#ifndef GET_POINTERID_WPARAM
#define GET_POINTERID_WPARAM(w) (LOWORD(w))
#endif

#define MAX_CONTACTS 32

typedef struct {
    BOOL  active;
    DWORD id;
    LONG  x, y;   /* client coords */
} Contact;

static Contact g_contacts[MAX_CONTACTS];

/* Dynamically resolved so the binary also runs on wine builds without the
 * pointer API exported. */
typedef BOOL (WINAPI *GetPointerType_t)(UINT32, /*POINTER_INPUT_TYPE*/ DWORD *);
static GetPointerType_t pGetPointerType;

static const COLORREF g_colors[] = {
    RGB(231, 76, 60),  RGB(46, 204, 113), RGB(52, 152, 219), RGB(241, 196, 15),
    RGB(155, 89, 182), RGB(230, 126, 34), RGB(26, 188, 156), RGB(236, 64, 122),
    RGB(149, 165, 166), RGB(255, 255, 255),
};

static void upsert_contact(DWORD id, LONG x, LONG y, BOOL remove)
{
    int i, freeSlot = -1;
    for (i = 0; i < MAX_CONTACTS; i++) {
        if (g_contacts[i].active && g_contacts[i].id == id) {
            if (remove) g_contacts[i].active = FALSE;
            else { g_contacts[i].x = x; g_contacts[i].y = y; }
            return;
        }
        if (!g_contacts[i].active && freeSlot < 0) freeSlot = i;
    }
    if (!remove && freeSlot >= 0) {
        g_contacts[freeSlot].active = TRUE;
        g_contacts[freeSlot].id = id;
        g_contacts[freeSlot].x = x;
        g_contacts[freeSlot].y = y;
    }
}

static int active_contacts(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_CONTACTS; i++) if (g_contacts[i].active) n++;
    return n;
}

static void handle_wm_touch(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    UINT count = LOWORD(wParam);
    TOUCHINPUT inputs[MAX_CONTACTS];
    UINT i;

    if (count > MAX_CONTACTS) count = MAX_CONTACTS;
    if (!GetTouchInputInfo((HTOUCHINPUT)lParam, count, inputs, sizeof(TOUCHINPUT))) {
        printf("WM_TOUCH: GetTouchInputInfo failed (err=%lu)\n", GetLastError());
        return;
    }

    for (i = 0; i < count; i++) {
        POINT pt = { inputs[i].x / 100, inputs[i].y / 100 }; /* centi-pixels -> px */
        ScreenToClient(hwnd, &pt);

        const char *what = (inputs[i].dwFlags & TOUCHEVENTF_DOWN) ? "DOWN"
                         : (inputs[i].dwFlags & TOUCHEVENTF_UP)   ? "UP  "
                         : (inputs[i].dwFlags & TOUCHEVENTF_MOVE) ? "MOVE" : "??? ";

        printf("WM_TOUCH   %s id=%lu pos=(%ld,%ld) flags=0x%lx primary=%d\n",
               what, inputs[i].dwID, pt.x, pt.y, inputs[i].dwFlags,
               (inputs[i].dwFlags & TOUCHEVENTF_PRIMARY) ? 1 : 0);

        upsert_contact(inputs[i].dwID + 0x10000 /* keep separate from pointer ids */,
                       pt.x, pt.y, (inputs[i].dwFlags & TOUCHEVENTF_UP) != 0);
    }
    fflush(stdout);
    CloseTouchInputHandle((HTOUCHINPUT)lParam);
    InvalidateRect(hwnd, NULL, TRUE);
}

static void handle_wm_pointer(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UINT32 id = GET_POINTERID_WPARAM(wParam);
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; /* screen coords */
    DWORD type = 0;

    ScreenToClient(hwnd, &pt);
    if (pGetPointerType && !pGetPointerType(id, &type)) type = 0;

    const char *what = msg == WM_POINTERDOWN ? "DOWN" : msg == WM_POINTERUP ? "UP  " : "MOVE";
    printf("WM_POINTER %s id=%u type=%lu pos=(%ld,%ld)\n", what, id, type, pt.x, pt.y);
    fflush(stdout);

    upsert_contact(id, pt.x, pt.y, msg == WM_POINTERUP);
    InvalidateRect(hwnd, NULL, TRUE);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    char buf[128];
    int i, ci = 0;

    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(200, 200, 200));
    snprintf(buf, sizeof(buf), "touchtest: active contacts = %d  (touch/drag with multiple fingers)",
             active_contacts());
    TextOutA(hdc, 10, 10, buf, (int)strlen(buf));

    for (i = 0; i < MAX_CONTACTS; i++) {
        if (!g_contacts[i].active) continue;
        HBRUSH brush = CreateSolidBrush(g_colors[ci++ % (sizeof(g_colors) / sizeof(g_colors[0]))]);
        HGDIOBJ old = SelectObject(hdc, brush);
        Ellipse(hdc, g_contacts[i].x - 40, g_contacts[i].y - 40,
                     g_contacts[i].x + 40, g_contacts[i].y + 40);
        SelectObject(hdc, old);
        DeleteObject(brush);

        snprintf(buf, sizeof(buf), "%lu", g_contacts[i].id & 0xFFFF);
        SetTextColor(hdc, RGB(0, 0, 0));
        TextOutA(hdc, g_contacts[i].x - 8, g_contacts[i].y - 8, buf, (int)strlen(buf));
    }

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_TOUCH:
            handle_wm_touch(hwnd, wParam, lParam);
            return 0;
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
            handle_wm_pointer(hwnd, msg, wParam, lParam);
            return 0;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int main(void)
{
    int digitizer = GetSystemMetrics(SM_DIGITIZER);
    int maxTouches = GetSystemMetrics(95 /* SM_MAXIMUMTOUCHES */);

    printf("touchtest starting\n");
    printf("SM_DIGITIZER = 0x%x  SM_MAXIMUMTOUCHES = %d\n", digitizer, maxTouches);
    printf("(0 values are expected on this wine build; they do not indicate a broken pipeline)\n");
    fflush(stdout);

    pGetPointerType = (GetPointerType_t)(void *)GetProcAddress(
            GetModuleHandleA("user32.dll"), "GetPointerType");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "TouchTestWnd";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "touchtest - WM_TOUCH/WM_POINTER logger",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("CreateWindow failed (err=%lu)\n", GetLastError());
        return 1;
    }

    if (RegisterTouchWindow(hwnd, 0))
        printf("RegisterTouchWindow OK - WM_TOUCH will be synthesized from WM_POINTER\n");
    else
        printf("RegisterTouchWindow FAILED (err=%lu) - will still log WM_POINTER\n", GetLastError());
    fflush(stdout);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
