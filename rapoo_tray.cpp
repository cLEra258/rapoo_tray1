#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <hidapi/hidapi.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <cstdio>

// ==================== 协议常量（已确认） ====================
constexpr unsigned short RAPOO_VID         = 0x24AE;
constexpr unsigned short VENDOR_USAGE_PAGE = 0xFF00;
constexpr unsigned char  BATTERY_REPORT_ID = 0x07;
constexpr int            BATTERY_BYTE_INDEX = 7;

// ==================== 托盘常量 ====================
constexpr UINT WM_TRAYICON     = WM_APP + 1;
constexpr UINT_PTR TRAY_UID    = 1;
constexpr UINT ID_MENU_EXIT    = 1001;
constexpr UINT ID_MENU_RECONNECT = 1002;

// ==================== 全局状态 ====================
std::atomic<int>  g_battery{ -1 };
std::atomic<bool> g_running{ true };
std::atomic<bool> g_connected{ false };
hid_device*       g_hid = nullptr;
HWND              g_hwnd = nullptr;
HICON             g_currentIcon = nullptr;
std::mutex        g_iconMutex;

// ==================== 电池配色 ====================
static COLORREF BatteryFillColor(int pct) {
    if (pct < 0)  return RGB(170, 170, 170);
    if (pct < 20) return RGB(239, 68, 68);   // 红
    if (pct < 50) return RGB(245, 158, 11);  // 黄
    return RGB(16, 185, 129);                // 绿
}

// ==================== 绘制 64x64 电池图标 ====================
static HICON CreateBatteryIcon(int pct) {
    const int SZ = 64;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = SZ;
    bi.bV5Height = -SZ;              // 自上而下
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP hbm = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits) { DeleteDC(hdc); return nullptr; }
    HGDIOBJ oldBmp = SelectObject(hdc, hbm);
    memset(bits, 0, SZ * SZ * 4);
    SetBkMode(hdc, TRANSPARENT);

    // 几何：电池主体 + 顶部凸起
    const int bodyL = 6,  bodyT = 18, bodyR = 52, bodyB = 48;
    const int capL  = 52, capT  = 26, capR  = 58, capB  = 40;
    const int innerL = bodyL + 4, innerT = bodyT + 4;
    const int innerR = bodyR - 4, innerB = bodyB - 4;
    const int innerW = innerR - innerL;

    // 1. 外框（深灰，实心圆角矩形）
    {
        HBRUSH br = CreateSolidBrush(RGB(64, 64, 64));
        HPEN   pn = CreatePen(PS_SOLID, 0, RGB(64, 64, 64));
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pn);
        RoundRect(hdc, bodyL, bodyT, bodyR, bodyB, 10, 10);
        RoundRect(hdc, capL,  capT,  capR,  capB,  3,  3);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pn);
    }

    // 2. 内衬（白色）
    {
        HBRUSH br = CreateSolidBrush(RGB(245, 245, 245));
        HPEN   pn = CreatePen(PS_SOLID, 0, RGB(245, 245, 245));
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pn);
        RoundRect(hdc, innerL, innerT, innerR, innerB, 7, 7);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pn);
    }

    // 3. 电量填充
    if (pct > 0) {
        int fillW = (innerW * pct + 50) / 100;
        if (fillW < 3) fillW = 3;   // 确保极小电量也可见
        if (fillW > innerW) fillW = innerW;
        COLORREF col = BatteryFillColor(pct);
        HBRUSH br = CreateSolidBrush(col);
        HPEN   pn = CreatePen(PS_SOLID, 0, col);
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pn);
        RoundRect(hdc, innerL, innerT, innerL + fillW, innerB, 6, 6);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pn);
    }

    // 4. 电量数字（动态字号：1 位最大，3 位最小）
    wchar_t txt[8];
    if (pct >= 0) swprintf_s(txt, L"%d", pct);
    else          wcscpy_s(txt, L"?");

    int fontSize;
    if (pct >= 100)      fontSize = 20;
    else if (pct >= 10)  fontSize = 24;
    else                 fontSize = 28;

    HFONT font = CreateFontW(
        fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"
    );
    HGDIOBJ oldFont = SelectObject(hdc, font);

    RECT tr{ bodyL, bodyT, bodyR, bodyB };

    // 阴影（黑色，偏移 1px）
    SetTextColor(hdc, RGB(0, 0, 0));
    RECT sh = tr;
    OffsetRect(&sh, 1, 1);
    DrawTextW(hdc, txt, -1, &sh, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 正文（白色）
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
    DeleteObject(font);
    SelectObject(hdc, oldBmp);

    // 5. 转 HICON
    HBITMAP mask = CreateBitmap(SZ, SZ, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hbm;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(hbm);
    DeleteObject(mask);
    DeleteDC(hdc);
    return icon;
}

// ==================== 托盘更新 ====================
static void UpdateTray(int pct) {
    if (!g_hwnd) return;

    HICON newIcon = CreateBatteryIcon(pct);
    {
        std::lock_guard<std::mutex> lk(g_iconMutex);
        if (g_currentIcon) DestroyIcon(g_currentIcon);
        g_currentIcon = newIcon;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = newIcon;

    if (pct < 0) wcscpy_s(nid.szTip, L"Rapoo 鼠标 — 等待数据…");
    else         swprintf_s(nid.szTip, L"Rapoo 鼠标 — 电量 %d%%", pct);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ==================== 打开 0xFF00 厂商接口 ====================
static hid_device* OpenRapooBatteryInterface() {
    hid_device_info* list = hid_enumerate(RAPOO_VID, 0);
    hid_device* dev = nullptr;
    for (auto* p = list; p; p = p->next) {
        if (p->usage_page == VENDOR_USAGE_PAGE) {
            dev = hid_open_path(p->path);
            if (dev) break;
        }
    }
    hid_free_enumeration(list);
    return dev;
}

// ==================== 后台读取线程 ====================
static void ReaderThread() {
    while (g_running.load()) {
        hid_device* dev = OpenRapooBatteryInterface();
        if (!dev) {
            g_connected = false;
            if (g_battery.load() != -1) { g_battery = -1; UpdateTray(-1); }
            for (int i = 0; i < 30 && g_running.load(); ++i) Sleep(100);
            continue;
        }

        g_hid = dev;
        g_connected = true;

        unsigned char buf[64];
        while (g_running.load()) {
            int n = hid_read_timeout(dev, buf, sizeof(buf), 500);
            if (n < 0) break;                       // 设备断开
            if (n == 0) continue;                   // 超时
            if (n > 1 + BATTERY_BYTE_INDEX && buf[0] == BATTERY_REPORT_ID) {
                int pct = buf[1 + BATTERY_BYTE_INDEX];
                if (pct >= 0 && pct <= 100 && pct != g_battery.load()) {
                    g_battery = pct;
                    UpdateTray(pct);
                }
            }
        }

        hid_close(dev);
        g_hid = nullptr;
        g_connected = false;
        if (g_battery.load() != -1) { g_battery = -1; UpdateTray(-1); }

        if (!g_running.load()) break;
        for (int i = 0; i < 20 && g_running.load(); ++i) Sleep(100);
    }
}

// ==================== 窗口过程 ====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();

            // 电量信息项（禁用状态，仅显示）
            wchar_t info[64];
            if (g_battery.load() >= 0) swprintf_s(info, L"电量：%d%%", g_battery.load());
            else                       wcscpy_s(info, L"电量：未知");
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, info);

            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_MENU_RECONNECT, L"重新连接");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"退出");

            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessage(hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_MENU_EXIT) {
            g_running = false;
            if (g_hid) hid_close(g_hid);
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        } else if (LOWORD(wp) == ID_MENU_RECONNECT) {
            g_running = false;
            if (g_hid) hid_close(g_hid);
            g_running = true;
            std::thread(ReaderThread).detach();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== 入口 ====================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    if (hid_init() != 0) {
        MessageBoxW(nullptr, L"hidapi 初始化失败", L"错误", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"RapooTrayWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"RapooTrayWnd", L"RapooTray", 0,
                             0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hwnd) { hid_exit(); return 1; }

    HICON initIcon = CreateBatteryIcon(-1);
    g_currentIcon = initIcon;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = initIcon;
    wcscpy_s(nid.szTip, L"Rapoo 鼠标 — 等待数据…");
    Shell_NotifyIconW(NIM_ADD, &nid);

    std::thread reader(ReaderThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    if (reader.joinable()) reader.join();
    if (g_hid) { hid_close(g_hid); g_hid = nullptr; }

    Shell_NotifyIconW(NIM_DELETE, &nid);
    {
        std::lock_guard<std::mutex> lk(g_iconMutex);
        if (g_currentIcon) DestroyIcon(g_currentIcon);
    }

    hid_exit();
    return 0;
}
