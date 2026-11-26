// TaskbarMeter - a tiny CPU / RAM / GPU / disk / network monitor for the Windows taskbar.
//
// Draws plain text straight onto the taskbar (as a layered child window of Shell_TrayWnd),
// refreshed once per second. Double-click opens Task Manager, right-click opens the menu.
// Meters that are switched off are not sampled at all.
//
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wchar.h>

enum { S_CPU = 1, S_RAM = 2, S_GPU = 4, S_DISK = 8, S_NET = 16, S_ALL = 31, S_DEFAULT = S_CPU | S_RAM };

// ---------- strings (Korean UI language -> Korean, otherwise English) ----------
enum { T_CPU, T_RAM, T_GPU, T_DISK, T_NET, T_LEFT, T_AUTO, T_EXIT, T_COUNT };
static const wchar_t *const STR_EN[T_COUNT] = {
    L"CPU", L"RAM", L"GPU", L"Disk", L"Network", L"Show on left", L"Start with Windows", L"Exit",
};
static const wchar_t *const STR_KO[T_COUNT] = {
    L"CPU", L"RAM", L"GPU", L"디스크", L"네트워크", L"왼쪽에 표시", L"Windows 시작 시 실행", L"종료",
};
static const wchar_t *const *STR = STR_EN;

static const DWORD ITEM_BITS[] = { S_CPU, S_RAM, S_GPU, S_DISK, S_NET }; // menu order = T_CPU..T_NET
#define N_ITEMS (sizeof ITEM_BITS / sizeof ITEM_BITS[0])

#define ID_AUTO 1
#define ID_EXIT 2
#define ID_LEFT 3
#define ID_ITEM0 10
#define WM_SHOWMENU (WM_APP + 1)

static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t CFG_KEY[] = L"Software\\TaskbarMeter";
static const wchar_t APP_NAME[] = L"TaskbarMeter";

static HINSTANCE g_inst;
static HWND g_main, g_child, g_tray;
static HFONT g_font;
static UINT g_fontDpi;
static DWORD g_show = S_DEFAULT;
static BOOL g_left = TRUE; // left end of the taskbar (centered taskbar only)

static int g_cpu, g_ram, g_gpu, g_disk;
static double g_up, g_down;

// ---------- sampling ----------
static ULONGLONG g_prevIdle, g_prevTotal;
static ULONGLONG ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void sampleCpu(void)
{
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return;
    ULONGLONG idle = ft64(fi), total = ft64(fk) + ft64(fu); // kernel time includes idle time
    if (g_prevTotal && total > g_prevTotal) {
        ULONGLONG dt = total - g_prevTotal, di = idle - g_prevIdle;
        if (di > dt) di = dt;
        g_cpu = (int)((100 * (dt - di) + dt / 2) / dt);
    }
    g_prevTotal = total;
    g_prevIdle = idle;
}

static void sampleRam(void)
{
    MEMORYSTATUSEX m = { sizeof m };
    if (GlobalMemoryStatusEx(&m)) g_ram = (int)m.dwMemoryLoad;
}

static int clampPct(double v) { return v < 0 ? 0 : v > 100 ? 100 : (int)(v + 0.5); }

// GPU: same method as Task Manager - sum every process per engine, then take the busiest engine.
static PDH_HQUERY g_qGpu;
static PDH_HCOUNTER g_cGpu;

static void sampleGpu(void)
{
    if (!g_qGpu || PdhCollectQueryData(g_qGpu) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(g_cGpu, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, NULL) != PDH_MORE_DATA)
        return;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = HeapAlloc(GetProcessHeap(), 0, size);
    if (!it) return;
    if (PdhGetFormattedCounterArrayW(g_cGpu, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, it) == ERROR_SUCCESS) {
        enum { MAXG = 256 };
        const wchar_t *key[MAXG];
        double sum[MAXG];
        int ng = 0;
        double best = 0;
        for (DWORD i = 0; i < count; i++) {
            if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            // "pid_123_luid_..._phys_0_eng_3_engtype_3D": everything from "luid_" identifies the engine
            const wchar_t *k = wcsstr(it[i].szName, L"luid_");
            if (!k) continue;
            int g = 0;
            while (g < ng && lstrcmpW(key[g], k) != 0) g++;
            if (g == ng) {
                if (ng == MAXG) continue;
                key[ng] = k;
                sum[ng++] = 0;
            }
            sum[g] += it[i].FmtValue.doubleValue;
            if (sum[g] > best) best = sum[g];
        }
        g_gpu = clampPct(best);
    }
    HeapFree(GetProcessHeap(), 0, it);
}

// Disk: active time = 100 - idle time, across all physical disks.
static PDH_HQUERY g_qDisk;
static PDH_HCOUNTER g_cDisk;

static void sampleDisk(void)
{
    PDH_FMT_COUNTERVALUE v;
    if (!g_qDisk || PdhCollectQueryData(g_qDisk) != ERROR_SUCCESS) return;
    if (PdhGetFormattedCounterValue(g_cDisk, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &v) == ERROR_SUCCESS)
        g_disk = clampPct(100.0 - v.doubleValue);
}

// English counter names work on every display language.
static void pdhOpen(PDH_HQUERY *q, PDH_HCOUNTER *c, const wchar_t *path)
{
    if (*q) return;
    if (PdhOpenQueryW(NULL, 0, q) != ERROR_SUCCESS) { *q = NULL; return; }
    if (PdhAddEnglishCounterW(*q, path, 0, c) != ERROR_SUCCESS) { PdhCloseQuery(*q); *q = NULL; return; }
    PdhCollectQueryData(*q);
}

static void pdhClose(PDH_HQUERY *q)
{
    if (*q) PdhCloseQuery(*q);
    *q = NULL;
}

// Network: bytes sent/received by physical adapters (skips loopback, VPN and filter layers).
static ULONG64 g_prevIn, g_prevOut;
static ULONGLONG g_prevNetT;

static void sampleNet(void)
{
    MIB_IF_TABLE2 *t;
    if (GetIfTable2(&t) != NO_ERROR) return;
    ULONG64 in = 0, out = 0;
    for (ULONG i = 0; i < t->NumEntries; i++) {
        MIB_IF_ROW2 *r = &t->Table[i];
        if (!r->InterfaceAndOperStatusFlags.HardwareInterface || r->InterfaceAndOperStatusFlags.FilterInterface ||
            r->OperStatus != IfOperStatusUp)
            continue;
        in += r->InOctets;
        out += r->OutOctets;
    }
    FreeMibTable(t);
    ULONGLONG now = GetTickCount64();
    if (g_prevNetT && now > g_prevNetT && in >= g_prevIn && out >= g_prevOut) {
        double dt = (now - g_prevNetT) / 1000.0;
        g_down = (in - g_prevIn) / dt;
        g_up = (out - g_prevOut) / dt;
    }
    g_prevIn = in;
    g_prevOut = out;
    g_prevNetT = now;
}

static void sampleAll(void)
{
    if (g_show & S_CPU) sampleCpu();
    if (g_show & S_RAM) sampleRam();
    if (g_show & S_GPU) sampleGpu();
    if (g_show & S_DISK) sampleDisk();
    if (g_show & S_NET) sampleNet();
}

// ---------- settings (HKCU\Software\TaskbarMeter) ----------
static void loadSettings(void)
{
    DWORD v, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, CFG_KEY, L"Show", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS && (v & S_ALL))
        g_show = v & S_ALL;
    sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, CFG_KEY, L"Left", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        g_left = v != 0;
}

static void setLeft(BOOL on)
{
    DWORD v = on;
    g_left = on;
    RegSetKeyValueW(HKEY_CURRENT_USER, CFG_KEY, L"Left", REG_DWORD, &v, sizeof v);
}

// Opens/closes the PDH queries so that hidden meters cost nothing.
static void applyShow(DWORD s)
{
    DWORD turnedOn = s & ~g_show;
    g_show = s;
    RegSetKeyValueW(HKEY_CURRENT_USER, CFG_KEY, L"Show", REG_DWORD, &s, sizeof s);

    if (s & S_GPU) pdhOpen(&g_qGpu, &g_cGpu, L"\\GPU Engine(*)\\Utilization Percentage");
    else pdhClose(&g_qGpu);
    if (s & S_DISK) pdhOpen(&g_qDisk, &g_cDisk, L"\\PhysicalDisk(_Total)\\% Idle Time");
    else pdhClose(&g_qDisk);

    if (turnedOn & S_CPU) { g_prevTotal = 0; sampleCpu(); }
    if (turnedOn & S_NET) { g_prevNetT = 0; g_up = g_down = 0; sampleNet(); }
}

static BOOL isLightTheme(void)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v == 1;
    return FALSE;
}

// ---------- drawing ----------
typedef struct { const wchar_t *label; wchar_t value[16]; BOOL hot; } Row;
typedef struct { Row rows[2]; int n; BOOL net; int labelW, w; } Col;

static void fmtPct(Row *r, const wchar_t *label, int v)
{
    r->label = label;
    wsprintfW(r->value, L"%d%%", v);
    r->hot = v >= 90;
}

static void fmtRate(Row *r, const wchar_t *label, double bps)
{
    r->label = label;
    r->hot = FALSE;
    int k = (int)(bps / 1024 + 0.5);
    if (k < 1000) { wsprintfW(r->value, L"%d KB", k); return; }
    int m10 = (int)(bps / (1024.0 * 1024) * 10 + 0.5); // MB/s x 10
    if (m10 < 100) wsprintfW(r->value, L"%d.%d MB", m10 / 10, m10 % 10);
    else if (m10 < 10000) wsprintfW(r->value, L"%d MB", (m10 + 5) / 10);
    else wsprintfW(r->value, L"%d.%d GB", m10 / 10240, (m10 % 10240) * 10 / 10240);
}

static int textW(HDC dc, const wchar_t *s)
{
    SIZE z;
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &z);
    return z.cx;
}

static void render(void)
{
    RECT tr, nr;
    if (!GetWindowRect(g_tray, &tr)) return;
    UINT dpi = GetDpiForWindow(g_tray);
    if (!dpi) dpi = 96;
#define S(v) MulDiv((v), dpi, 96)

    if (!g_font || g_fontDpi != dpi) {
        if (g_font) DeleteObject(g_font);
        g_font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontDpi = dpi;
    }

    // Visible rows, stacked two per column. Network up/down always share one column.
    Row pct[4];
    int np = 0;
    if (g_show & S_CPU) fmtPct(&pct[np++], L"CPU", g_cpu);
    if (g_show & S_RAM) fmtPct(&pct[np++], L"RAM", g_ram);
    if (g_show & S_GPU) fmtPct(&pct[np++], L"GPU", g_gpu);
    if (g_show & S_DISK) fmtPct(&pct[np++], L"DISK", g_disk);

    Col cols[4];
    int nc = 0;
    for (int i = 0; i < np; i += 2) {
        Col *c = &cols[nc++];
        c->net = FALSE;
        c->n = (i + 1 < np) ? 2 : 1;
        c->rows[0] = pct[i];
        if (c->n == 2) c->rows[1] = pct[i + 1];
    }
    if (g_show & S_NET) {
        Col *c = &cols[nc++];
        c->net = TRUE;
        c->n = 2;
        fmtRate(&c->rows[0], L"↑", g_up);
        fmtRate(&c->rows[1], L"↓", g_down);
    }
    if (!nc) return;

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ oldFont = SelectObject(mem, g_font);

    // Column widths are sized for the widest possible value so the layout never jitters.
    int gapIn = S(5), gapCol = S(10), w = 0;
    int pctW = textW(mem, L"100%"), netW = textW(mem, L"999 KB");
    for (int i = 0; i < nc; i++) {
        Col *c = &cols[i];
        c->labelW = 0;
        for (int r = 0; r < c->n; r++) {
            int lw = textW(mem, c->rows[r].label);
            if (lw > c->labelW) c->labelW = lw;
        }
        c->w = c->labelW + gapIn + (c->net ? netW : pctW);
        w += c->w + (i ? gapCol : 0);
    }

    // Default spot: just left of the notification area.
    int h = tr.bottom - tr.top;
    int right = (tr.right - tr.left) - S(260);
    HWND notify = FindWindowExW(g_tray, NULL, L"TrayNotifyWnd", NULL);
    if (notify && GetWindowRect(notify, &nr) && nr.right > nr.left) right = nr.left - tr.left;
    int x = right - w - S(6);

    // Centered taskbar (Windows 11 default): the app icons end where the Start button's mirror image is.
    // Use the empty left end if asked to, or if the right spot would cover the icons.
    HWND start = FindWindowExW(g_tray, NULL, L"Start", NULL);
    RECT sr;
    if (start && GetWindowRect(start, &sr) && sr.left - tr.left > S(100)) {
        int iconsRight = (tr.right - tr.left) - (sr.left - tr.left);
        if (g_left || x < iconsRight + S(8)) x = S(12);
    }

    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD *px = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp) {
        SelectObject(mem, oldFont);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // Draw grayscale-antialiased text on black: normal text in white, warning values in pure red.
    // The coverage then becomes per-pixel alpha, and the red channel marks which color to apply.
    SetBkMode(mem, TRANSPARENT);
    int lh = S(16), top = (h - 2 * lh) / 2, cx = 0;
    for (int i = 0; i < nc; i++) {
        Col *c = &cols[i];
        for (int r = 0; r < c->n; r++) {
            int y = c->n == 2 ? top + r * lh : (h - lh) / 2;
            RECT rc = { cx, y, cx + c->w, y + lh };
            SetTextColor(mem, RGB(255, 255, 255));
            DrawTextW(mem, c->rows[r].label, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(mem, c->rows[r].hot ? RGB(255, 0, 0) : RGB(255, 255, 255));
            DrawTextW(mem, c->rows[r].value, -1, &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        cx += c->w + gapCol;
    }
    GdiFlush();

    BOOL light = isLightTheme();
    COLORREF fg = light ? RGB(25, 25, 25) : RGB(245, 245, 245);
    COLORREF hot = light ? RGB(200, 70, 0) : RGB(255, 150, 60);
    for (int i = 0; i < w * h; i++) {
        DWORD p = px[i];
        BYTE r = (BYTE)(p >> 16), g = (BYTE)(p >> 8);
        if (!r) { px[i] = 0x01000000; continue; } // alpha 1: invisible, but still receives clicks
        COLORREF c = (g < r / 2) ? hot : fg;
        DWORD a = r;
        px[i] = (a << 24) | ((GetRValue(c) * a / 255) << 16) | ((GetGValue(c) * a / 255) << 8) | (GetBValue(c) * a / 255);
    }

    SetWindowPos(g_child, HWND_TOP, x, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SIZE size = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_child, screen, NULL, &size, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
#undef S
}

static void tick(void)
{
    sampleAll();
    HWND t = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!t) return; // Explorer is restarting

    // (Re)attach to the taskbar - it is recreated whenever Explorer restarts.
    if (t != g_tray || !g_child || !IsWindow(g_child)) {
        if (g_child && IsWindow(g_child)) DestroyWindow(g_child);
        g_tray = t;
        g_child = CreateWindowExW(WS_EX_LAYERED, L"TaskbarMeterChild", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                                  g_tray, NULL, g_inst, NULL);
        if (!g_child) return;
    }
    render();
}

// ---------- start with Windows (HKCU Run key) ----------
static BOOL isAuto(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void setAuto(BOOL on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH], val[MAX_PATH + 2];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wsprintfW(val, L"\"%s\"", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)val, (DWORD)((lstrlenW(val) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ---------- menu ----------
static void showMenu(void)
{
    POINT p;
    GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    BOOL onlyOne = (g_show & (g_show - 1)) == 0; // never allow hiding the last meter
    for (UINT i = 0; i < N_ITEMS; i++) {
        BOOL on = (g_show & ITEM_BITS[i]) != 0;
        AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0) | (on && onlyOne ? MF_GRAYED : 0), ID_ITEM0 + i, STR[T_CPU + i]);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_left ? MF_CHECKED : 0), ID_LEFT, STR[T_LEFT]);
    AppendMenuW(m, MF_STRING | (isAuto() ? MF_CHECKED : 0), ID_AUTO, STR[T_AUTO]);
    AppendMenuW(m, MF_STRING, ID_EXIT, STR[T_EXIT]);
    SetForegroundWindow(g_main);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (cmd >= ID_ITEM0 && cmd < ID_ITEM0 + (int)N_ITEMS) {
        applyShow(g_show ^ ITEM_BITS[cmd - ID_ITEM0]);
        if (g_child && IsWindow(g_child)) render();
    } else if (cmd == ID_LEFT) {
        setLeft(!g_left);
        if (g_child && IsWindow(g_child)) render();
    } else if (cmd == ID_AUTO) {
        setAuto(!isAuto());
    } else if (cmd == ID_EXIT) {
        DestroyWindow(g_main);
    }
}

// ---------- windows ----------
static LRESULT CALLBACK childProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDBLCLK:
        ShellExecuteW(NULL, L"open", L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL);
        return 0;
    case WM_RBUTTONUP:
        PostMessageW(g_main, WM_SHOWMENU, 0, 0); // show it from our own top-level window
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        tick();
        return 0;
    case WM_SHOWMENU:
        showMenu();
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        if (g_child && IsWindow(g_child)) DestroyWindow(g_child);
        pdhClose(&g_qGpu);
        pdhClose(&g_qDisk);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    CreateMutexW(NULL, TRUE, L"TaskbarMeter_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_inst = inst;
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN) STR = STR_KO;

    WNDCLASSW wc = { 0 };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpfnWndProc = mainProc;
    wc.lpszClassName = L"TaskbarMeterMain";
    RegisterClassW(&wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = childProc;
    wc.lpszClassName = L"TaskbarMeterChild";
    RegisterClassW(&wc);

    // Hidden top-level window: owns the timer and the menu, and outlives Explorer restarts.
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, L"TaskbarMeterMain", APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_main) return 1;

    loadSettings();
    DWORD s = g_show;
    g_show = 0;
    applyShow(s); // open the needed counters and take the first samples
    tick();
    SetTimer(g_main, 1, 1000, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
