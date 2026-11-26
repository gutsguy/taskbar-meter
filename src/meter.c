// TaskbarMeter - a tiny CPU / RAM / GPU / disk / network monitor for the Windows taskbar.
//
// Draws plain text straight onto the taskbar (as a layered child window of Shell_TrayWnd),
// refreshed once per second. Double-click opens Task Manager, right-click opens the menu,
// hovering shows a tooltip with details. Meters that are switched off are not sampled at all.
//
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <uiautomation.h>

enum { S_CPU = 1, S_RAM = 2, S_GPU = 4, S_DISK = 8, S_NET = 16, S_ALL = 31, S_DEFAULT = S_CPU | S_RAM };

// ---------- strings (Korean UI language -> Korean, otherwise English) ----------
enum { T_CPU, T_RAM, T_GPU, T_DISK, T_NET, T_LEFT, T_AUTO, T_EXIT,
       T_TIP_RAM, T_TIP_NET, T_TIP_UP, T_DHM, T_HM, T_COUNT };
static const wchar_t *const STR_EN[T_COUNT] = {
    L"CPU", L"RAM", L"GPU", L"Disk", L"Network", L"Show on left", L"Start with Windows", L"Exit",
    L"RAM %s / %s GB", L"Total \x2193 %s  \x2191 %s", L"Uptime %s", L"%dd %dh %dm", L"%dh %dm",
};
static const wchar_t *const STR_KO[T_COUNT] = {
    L"CPU", L"RAM", L"GPU", L"디스크", L"네트워크", L"왼쪽에 표시", L"Windows 시작 시 실행", L"종료",
    L"RAM %s / %s GB", L"누적 \x2193 %s  \x2191 %s", L"가동 시간 %s", L"%dd %dh %dm", L"%dh %dm",
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
static BOOL g_left = FALSE; // left end of the taskbar instead of next to the clock (centered taskbar only)

static int g_cpu, g_ram, g_gpu, g_disk;
static double g_up, g_down;
static ULONG64 g_totIn, g_totOut;     // bytes received / sent since start (while the network meter is on)

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
// Deltas are taken per adapter, so an adapter that just came up doesn't show its lifetime total as a spike.
typedef struct { ULONG64 luid, in, out; } IfStat;
enum { MAX_IF = 64 };
static IfStat g_prevIf[MAX_IF];
static int g_nPrevIf;
static ULONGLONG g_prevNetT;

static void sampleNet(void)
{
    MIB_IF_TABLE2 *t;
    if (GetIfTable2(&t) != NO_ERROR) return;
    IfStat cur[MAX_IF];
    int n = 0;
    ULONG64 din = 0, dout = 0;
    for (ULONG i = 0; i < t->NumEntries && n < MAX_IF; i++) {
        MIB_IF_ROW2 *r = &t->Table[i];
        if (!r->InterfaceAndOperStatusFlags.HardwareInterface || r->InterfaceAndOperStatusFlags.FilterInterface ||
            r->OperStatus != IfOperStatusUp)
            continue;
        IfStat s = { r->InterfaceLuid.Value, r->InOctets, r->OutOctets };
        for (int j = 0; j < g_nPrevIf; j++) {
            if (g_prevIf[j].luid != s.luid) continue;
            if (s.in >= g_prevIf[j].in) din += s.in - g_prevIf[j].in;
            if (s.out >= g_prevIf[j].out) dout += s.out - g_prevIf[j].out;
            break;
        }
        cur[n++] = s;
    }
    FreeMibTable(t);
    g_totIn += din;
    g_totOut += dout;
    ULONGLONG now = GetTickCount64();
    if (g_prevNetT && now > g_prevNetT) {
        double dt = (now - g_prevNetT) / 1000.0;
        g_down = din / dt;
        g_up = dout / dt;
    }
    memcpy(g_prevIf, cur, n * sizeof cur[0]);
    g_nPrevIf = n;
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
    if (turnedOn & S_NET) { g_prevNetT = 0; g_nPrevIf = 0; g_up = g_down = 0; sampleNet(); }
}

static BOOL isLightTheme(void)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v == 1;
    return FALSE;
}

// ---------- left-end placement ----------
// Things Windows draws left of the Start button (the Widgets/weather button, ...) have no window of
// their own, so ask UI Automation where they end. Runs on a worker thread: our window lives inside the
// taskbar, so querying the taskbar from our UI thread could deadlock. Only active in left mode.
static const GUID CLSID_UIA = { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const GUID IID_UIA = { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };
#define WM_LEFTEND (WM_APP + 2)
static volatile LONG g_leftEnd = -1; // right edge of whatever sits left of Start (taskbar-relative), -1 = unknown
static volatile LONG g_wantLeftEnd;  // 1 while left placement is in use
static HANDLE g_uiaThread, g_uiaWake;

static LONG findLeftEnd(IUIAutomation *uia, IUIAutomationCondition *all, IUIAutomationCacheRequest *cache, HWND tray, HWND self)
{
    RECT tr, sr;
    HWND start = FindWindowExW(tray, NULL, L"Start", NULL);
    if (!GetWindowRect(tray, &tr) || !start || !GetWindowRect(start, &sr)) return 0;
    LONG best = 0;
    IUIAutomationElement *root = NULL;
    IUIAutomationElementArray *arr = NULL;
    // One round trip: fetch every element of the taskbar with the three properties we need.
    if (SUCCEEDED(IUIAutomation_ElementFromHandle(uia, tray, &root)) && root &&
        SUCCEEDED(IUIAutomationElement_FindAllBuildCache(root, TreeScope_Descendants, all, cache, &arr)) && arr) {
        int n = 0;
        IUIAutomationElementArray_get_Length(arr, &n);
        for (int i = 0; i < n; i++) {
            IUIAutomationElement *e;
            if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &e)) || !e) continue;
            RECT r;
            UIA_HWND hw = NULL;
            BSTR cls = NULL;
            IUIAutomationElement_get_CachedNativeWindowHandle(e, &hw);
            IUIAutomationElement_get_CachedClassName(e, &cls);
            BOOL skip = (HWND)hw == self || (cls && lstrcmpW(cls, L"Windows.UI.Input.InputSite.WindowClass") == 0);
            if (cls) SysFreeString(cls);
            if (!skip && SUCCEEDED(IUIAutomationElement_get_CachedBoundingRectangle(e, &r)) && r.right > r.left &&
                r.left >= tr.left && r.right <= sr.left && r.right - tr.left > best)
                best = r.right - tr.left;
            IUIAutomationElement_Release(e);
        }
    }
    if (arr) IUIAutomationElementArray_Release(arr);
    if (root) IUIAutomationElement_Release(root);
    return best;
}

static DWORD WINAPI uiaThreadProc(void *arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUIAutomation *uia = NULL;
    IUIAutomationCondition *all = NULL;
    IUIAutomationCacheRequest *cache = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_UIA, NULL, CLSCTX_INPROC_SERVER, &IID_UIA, (void **)&uia)) && uia &&
        SUCCEEDED(IUIAutomation_CreateTrueCondition(uia, &all)) &&
        SUCCEEDED(IUIAutomation_CreateCacheRequest(uia, &cache))) {
        IUIAutomationCacheRequest_AddProperty(cache, UIA_BoundingRectanglePropertyId);
        IUIAutomationCacheRequest_AddProperty(cache, UIA_NativeWindowHandlePropertyId);
        IUIAutomationCacheRequest_AddProperty(cache, UIA_ClassNamePropertyId);
    } else {
        uia = NULL;
    }

    // Taskbar settings (Widgets on/off, alignment, ...) live here; react to changes right away.
    HANDLE wait[2] = { g_uiaWake, CreateEventW(NULL, FALSE, FALSE, NULL) };
    HKEY key = NULL;
    int nwait = 1;
    if (wait[1] && RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", 0,
                                 KEY_NOTIFY, &key) == ERROR_SUCCESS &&
        RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, wait[1], TRUE) == ERROR_SUCCESS)
        nwait = 2;

    // After a change, re-measure a few times while the taskbar animates; otherwise every 5 s.
    static const DWORD FOLLOW[] = { 300, 500, 700, 1500, 2000 };
    int follow = -1;
    for (;;) {
        DWORD r = WaitForMultipleObjects(nwait, wait, FALSE, follow >= 0 ? FOLLOW[follow] : 5000);
        if (r == WAIT_OBJECT_0 + 1) {
            RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, wait[1], TRUE);
            follow = 0;
        } else if (r == WAIT_TIMEOUT && follow >= 0 && ++follow == (int)(sizeof FOLLOW / sizeof FOLLOW[0])) {
            follow = -1;
        }
        if (!g_wantLeftEnd) continue;
        HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
        if (!tray) continue;
        LONG v = uia ? findLeftEnd(uia, all, cache, tray, g_child) : 0;
        if (InterlockedExchange(&g_leftEnd, v) != v) PostMessageW(g_main, WM_LEFTEND, 0, 0);
    }
}
// Returns the left-end x position, or -1 while the first measurement is pending.
static int leftEndX(void)
{
    g_wantLeftEnd = 1;
    if (!g_uiaThread) {
        g_uiaWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_uiaThread = CreateThread(NULL, 0, uiaThreadProc, NULL, 0, NULL);
        if (!g_uiaThread) return 0;
    }
    LONG le = g_leftEnd;
    if (le < 0) SetEvent(g_uiaWake);
    return le;
}

static void resetLeftEnd(void)
{
    InterlockedExchange(&g_leftEnd, -1);
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
    if (k < 1000) { wsprintfW(r->value, L"%d KB/s", k); return; }
    int m10 = (int)(bps / (1024.0 * 1024) * 10 + 0.5); // MB/s x 10
    if (m10 < 100) wsprintfW(r->value, L"%d.%d MB/s", m10 / 10, m10 % 10);
    else if (m10 < 10000) wsprintfW(r->value, L"%d MB/s", (m10 + 5) / 10);
    else wsprintfW(r->value, L"%d.%d GB/s", m10 / 10240, (m10 % 10240) * 10 / 10240);
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
    int pctW = textW(mem, L"100%"), netW = 0;
    static const wchar_t *const WIDEST_RATE[] = { L"999 KB/s", L"999 MB/s", L"9.9 GB/s" };
    for (int i = 0; i < 3; i++) {
        int tw = textW(mem, WIDEST_RATE[i]);
        if (tw > netW) netW = tw;
    }
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
    BOOL useLeft = FALSE;
    if (start && GetWindowRect(start, &sr) && sr.left - tr.left > S(100)) {
        int iconsRight = (tr.right - tr.left) - (sr.left - tr.left);
        useLeft = g_left || x < iconsRight + S(8);
    }
    if (useLeft) {
        int le = leftEndX();
        if (le < 0) { // wait for the first measurement instead of flashing in the wrong place
            SelectObject(mem, oldFont);
            DeleteDC(mem);
            ReleaseDC(NULL, screen);
            return;
        }
        x = le + S(12);
    } else {
        g_wantLeftEnd = 0;
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

// ---------- hover tooltip ----------
// Shows what the taskbar has no room for. Our own small popup, drawn off-screen and copied in one go,
// so it never flickers when a value changes. The text is only built while it is visible.
static HWND g_tip;
static HFONT g_tipFont;
static UINT g_tipDpi;
static wchar_t g_tipText[512];
static int g_tipLen;
static BOOL g_tipOn, g_hoverArmed, g_tipDark, g_tipRounded;
static HRESULT(WINAPI *g_dwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);

static void tipLine(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_tipLen) { lstrcpyW(g_tipText + g_tipLen, L"\r\n"); g_tipLen += 2; }
    g_tipLen += wvsprintfW(g_tipText + g_tipLen, fmt, ap);
    va_end(ap);
}

// 1023 KB, 12.3 MB, 456 MB, 1.2 GB, ...
static void fmtBytes(wchar_t *out, ULONG64 b)
{
    static const wchar_t *const UNIT[] = { L"KB", L"MB", L"GB", L"TB" };
    ULONG64 div = 1024;
    int u = 0;
    while (u < 3 && b >= div * 1000) { div *= 1024; u++; }
    ULONG64 x10 = (b * 10 + div / 2) / div;
    if (x10 < 100) wsprintfW(out, L"%d.%d %s", (int)(x10 / 10), (int)(x10 % 10), UNIT[u]);
    else wsprintfW(out, L"%d %s", (int)((x10 + 5) / 10), UNIT[u]);
}

static void fmtGB(wchar_t *out, ULONG64 b)
{
    ULONG64 x10 = (b * 10 + (1ULL << 29)) >> 30;
    wsprintfW(out, L"%d.%d", (int)(x10 / 10), (int)(x10 % 10));
}

static void buildTip(void)
{
    wchar_t a[24], b[24];
    g_tipLen = 0;
    g_tipText[0] = 0;
    // RAM and uptime are read here, so they cost nothing while the tooltip is hidden.
    MEMORYSTATUSEX ms = { sizeof ms };
    if (GlobalMemoryStatusEx(&ms)) {
        fmtGB(a, ms.ullTotalPhys - ms.ullAvailPhys); // Task Manager's "In use"
        fmtGB(b, ms.ullTotalPhys);
        tipLine(STR[T_TIP_RAM], a, b);
    }
    // Totals need sampling every second, so they only exist while the network meter is on.
    if (g_show & S_NET) {
        fmtBytes(a, g_totIn);
        fmtBytes(b, g_totOut);
        tipLine(STR[T_TIP_NET], a, b);
    }
    ULONGLONG min = GetTickCount64() / 60000;
    int d = (int)(min / 1440), h = (int)(min / 60 % 24), m = (int)(min % 60);
    if (d) wsprintfW(a, STR[T_DHM], d, h, m);
    else wsprintfW(a, STR[T_HM], h, m);
    tipLine(STR[T_TIP_UP], a);
}

#define TIP_S(v) MulDiv((v), g_tipDpi, 96)

static LRESULT CALLBACK tipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HGDIOBJ oldFont = SelectObject(mem, g_tipFont ? (HGDIOBJ)g_tipFont : GetStockObject(DEFAULT_GUI_FONT));
        HBRUSH bg = CreateSolidBrush(g_tipDark ? RGB(43, 43, 43) : RGB(249, 249, 249));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        if (!g_tipRounded) { // Windows 10: no DWM border, draw one
            HBRUSH br = CreateSolidBrush(g_tipDark ? RGB(70, 70, 70) : RGB(200, 200, 200));
            FrameRect(mem, &rc, br);
            DeleteObject(br);
        }
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, g_tipDark ? RGB(255, 255, 255) : RGB(26, 26, 26));
        RECT tr = { TIP_S(8), TIP_S(5), rc.right - TIP_S(8), rc.bottom - TIP_S(5) };
        DrawTextW(mem, g_tipText, -1, &tr, DT_LEFT | DT_NOPREFIX);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL ensureTip(void)
{
    if (g_tip) return TRUE;
    g_tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"TaskbarMeterTip", L"", WS_POPUP,
                            0, 0, 0, 0, g_main, NULL, g_inst, NULL);
    if (!g_tip) return FALSE;
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) g_dwmSetAttr = (void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    DWORD round = 3; // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUNDSMALL; fails before Windows 11
    g_tipRounded = g_dwmSetAttr && SUCCEEDED(g_dwmSetAttr(g_tip, 33, &round, sizeof round));
    return TRUE;
}

// The taskbar's monitor decides the DPI; the font is the one Windows uses for tooltips.
static void tipStyle(void)
{
    UINT dpi = GetDpiForWindow(g_tray);
    if (!dpi) dpi = 96;
    if (!g_tipFont || dpi != g_tipDpi) {
        NONCLIENTMETRICSW ncm = { sizeof ncm };
        if (g_tipFont) DeleteObject(g_tipFont);
        g_tipFont = SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi)
                        ? CreateFontIndirectW(&ncm.lfStatusFont) : NULL;
        g_tipDpi = dpi;
    }
    g_tipDark = !isLightTheme();
    if (g_tipRounded) {
        COLORREF border = g_tipDark ? RGB(70, 70, 70) : RGB(220, 220, 220); // DWMWA_BORDER_COLOR
        g_dwmSetAttr(g_tip, 34, &border, sizeof border);
    }
}

static SIZE tipSize(void)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, g_tipFont ? (HGDIOBJ)g_tipFont : GetStockObject(DEFAULT_GUI_FONT));
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, g_tipText, -1, &r, DT_CALCRECT | DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    SIZE s = { r.right + TIP_S(16), r.bottom + TIP_S(10) };
    return s;
}

// Shown centered above the meter, kept on the taskbar's monitor, and left there:
// following the width as the numbers change would make it wander.
static void showTip(void)
{
    RECT wr, tr;
    if (!g_child || !GetWindowRect(g_child, &wr) || !GetWindowRect(g_tray, &tr) || !ensureTip()) return;
    tipStyle();
    buildTip();
    SIZE s = tipSize();
    int x = (wr.left + wr.right) / 2 - s.cx / 2;
    MONITORINFO mi = { sizeof mi };
    if (GetMonitorInfoW(MonitorFromWindow(g_tray, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (x + s.cx > mi.rcMonitor.right) x = mi.rcMonitor.right - s.cx;
        if (x < mi.rcMonitor.left) x = mi.rcMonitor.left;
    }
    int y = tr.top - s.cy - TIP_S(8);
    InvalidateRect(g_tip, NULL, FALSE);
    SetWindowPos(g_tip, HWND_TOPMOST, x, y, s.cx, s.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_tipOn = TRUE;
}

// Repaints only when the text actually changed; grows or shrinks to the right, never moves.
static void updateTip(void)
{
    if (!g_tipOn) return;
    wchar_t prev[sizeof g_tipText / sizeof g_tipText[0]];
    lstrcpyW(prev, g_tipText);
    buildTip();
    if (lstrcmpW(prev, g_tipText) == 0) return;
    SIZE s = tipSize();
    SetWindowPos(g_tip, NULL, 0, 0, s.cx, s.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_tip, NULL, FALSE);
}
#undef TIP_S

static void hideTip(void)
{
    if (g_tipOn) ShowWindow(g_tip, SW_HIDE);
    g_tipOn = FALSE;
    g_hoverArmed = FALSE; // re-arm hover tracking on the next mouse move
}

static void tick(void)
{
    sampleAll();
    HWND t = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!t) return; // Explorer is restarting

    // (Re)attach to the taskbar - it is recreated whenever Explorer restarts.
    if (t != g_tray || !g_child || !IsWindow(g_child)) {
        hideTip();
        if (g_child && IsWindow(g_child)) DestroyWindow(g_child);
        g_tray = t;
        resetLeftEnd();
        g_child = CreateWindowExW(WS_EX_LAYERED, L"TaskbarMeterChild", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                                  g_tray, NULL, g_inst, NULL);
        if (!g_child) return;
    }
    render();
    updateTip();
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
        resetLeftEnd();
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
    case WM_MOUSEMOVE:
        if (!g_hoverArmed) {
            TRACKMOUSEEVENT t = { sizeof t, TME_HOVER | TME_LEAVE, h, HOVER_DEFAULT };
            g_hoverArmed = TrackMouseEvent(&t);
        }
        return 0;
    case WM_MOUSEHOVER:
        showTip();
        return 0;
    case WM_MOUSELEAVE:
        hideTip();
        return 0;
    case WM_LBUTTONDOWN:
        hideTip();
        return 0;
    case WM_LBUTTONDBLCLK:
        hideTip();
        ShellExecuteW(NULL, L"open", L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL);
        return 0;
    case WM_RBUTTONUP:
        hideTip();
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
    case WM_LEFTEND:
        if (g_child && IsWindow(g_child)) render();
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
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = tipProc;
    wc.lpszClassName = L"TaskbarMeterTip";
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
