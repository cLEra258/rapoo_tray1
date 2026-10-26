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
constexpr unsigned short RAPOO_VID       = 0x24AE;
constexpr unsigned short VENDOR_USAGE_PAGE = 0xFF00;
constexpr unsigned char  BATTERY_REPORT_ID = 0x07;
constexpr int            BATTERY_BYTE_INDEX = 7;   // 0-based，即第 8 个字节

// ==================== 托盘常量 ====================
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR TRAY_UID = 1;
constexpr UINT ID_MENU_EXIT = 1001;
constexpr UINT ID_MENU_RECONNECT = 1002;

// ==================== 全局状态 ====================
std::atomic<int>  g_battery{ -1 };          // -1 = 未知
std::atomic<bool> g_running{ true };
std::atomic<bool> g_connected{ false };
hid_device*       g_hid = nullptr;
HWND              g_hwnd = nullptr;
HICON             g_currentIcon = nullptr;
std::mutex        g_iconMutex;

// ==================== GDI 绘制电池图标 ====================
static COLORREF BatteryColor(int pct) {
    if (pct < 0)  return RGB(180, 180, 180);
    if (pct < 20) return RGB(239, 68, 68);    // 红
    if (pct < 50) return RGB(245, 158, 11);   // 黄
    return RGB(16, 185, 129);                 // 绿
}

// 在 32x32 的 DIB 上绘制电池轮廓 + 填充 + 百分数字，返回 HICON
static HICON CreateBatteryIcon(int pct) {
    const int SZ = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = SZ;
    bi.bV5Height = -SZ;          // 自上而下
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP hbm = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits) { DeleteDC(hdc); return nullptr; }
    HGDIOBJ old = SelectObject(hdc, hbm);

    // 清空为全透明
    memset(bits, 0, SZ * SZ * 4);

    // 电池外壳：圆角矩形轮廓（手工绘制，避免 GDI+ 依赖）
    const int bodyL = 5, bodyR = 26, bodyT = 10, bodyB = 24;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(220, 220, 220));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, bodyL, bodyT, bodyR, bodyB, 4, 4);
    // 电池头部凸起
    Rectangle(hdc, bodyR, bodyT + 4, bodyR + 3, bodyB - 4);

    // 填充电量
    if (pct > 0) {
        int innerW = (bodyR - bodyL - 4);
        int fillW = std::max(0, innerW * pct / 100);
        if (fillW > 0) {
            HBRUSH fillBrush = CreateSolidBrush(BatteryColor(pct));
            RECT fr{ bodyL + 2, bodyT + 2, bodyL + 2 + fillW, bodyB - 2 };
            FillRect(hdc, &fr, fillBrush);
            DeleteObject(fillBrush);
        }
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);

    // 绘制百分数字（尽量在 32px 内塞下）
    if (pct >= 0) {
        wchar_t txt[8];
        swprintf_s(txt, L"%d", pct);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        HFONT font = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, font);
        RECT tr{ 0, 0, SZ, SZ };
        DrawTextW(hdc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(font);
    }

    // 从 DIB 生成 HICON（带 alpha）
    HBITMAP hbmMask = CreateBitmap(SZ, SZ, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hbm;
    ii.hbmMask = hbmMask;
    HICON icon = CreateIconIndirect(&ii);

    SelectObject(hdc, old);
    DeleteObject(hbm);
    DeleteObject(hbmMask);
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

    if (pct < 0) wcscpy_s(nid.szTip, L"Rapoo 鼠标：等待数据…");
    else         swprintf_s(nid.szTip, L"Rapoo 鼠标电量：%d%%", pct);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ==================== 设备枚举与打开 ====================
static hid_device* OpenRapooBatteryInterface() {
    hid_device_info* list = hid_enumerate(RAPOO_VID, 0);
    hid_device* dev = nullptr;

    for (auto* p = list; p; p = p->next) {
        // 精确匹配 Usage Page 0xFF00 的厂商接口
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
            g_battery = -1;
            UpdateTray(-1);
            // 设备未连接：每 3 秒重试一次
            for (int i = 0; i < 30 && g_running.load(); ++i)
                Sleep(100);
            continue;
        }

        g_hid = dev;
        g_connected = true;

        unsigned char buf[64];
        while (g_running.load()) {
            // hid_read_timeout 在独立线程中阻塞等待是安全的；
            // 超时 500ms 用于定期检查退出标志。
            int n = hid_read_timeout(dev, buf, sizeof(buf), 500);
            if (n < 0) {
                // 设备断开或读取错误
                break;
            }
            if (n == 0) continue; // 超时，没有数据

            // 解析：hid_read 在有编号报告时，buf[0] 是 Report ID
            // 注意：BATTERY_BYTE_INDEX 是相对于“数据域”的索引，
            // 数据域从 buf[1] 开始，所以实际取 buf[1 + BATTERY_BYTE_INDEX]。
            if (n > 1 + BATTERY_BYTE_INDEX &&
                buf[0] == BATTERY_REPORT_ID) {
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
        g_battery = -1;
        UpdateTray(-1);

        if (!g_running.load()) break;
        // 断开后稍等再重连
        for (int i = 0; i < 20 && g_running.load(); ++i)
            Sleep(100);
    }
}

// ==================== 窗口过程（仅处理托盘消息和退出） ====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_MENU_RECONNECT, L"重新连接");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"退出");
            // 必须 SetForegroundWindow，否则菜单点外面不消失
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessage(hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_MENU_EXIT) {
            g_running = false;
            if (g_hid) hid_close(g_hid);   // 唤醒可能阻塞的读取
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        } else if (LOWORD(wp) == ID_MENU_RECONNECT) {
            // 简单粗暴：让读取线程重新枚举打开
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
    // hidapi 初始化
    if (hid_init() != 0) {
        MessageBoxW(nullptr, L"hidapi 初始化失败", L"错误", MB_ICONERROR);
        return 1;
    }

    // 注册一个隐藏窗口，只用于接收托盘回调
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"RapooTrayWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"RapooTrayWnd", L"RapooTray", 0,
                             0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hwnd) { hid_exit(); return 1; }

    // 添加托盘图标（先显示“等待数据”状态）
    HICON initIcon = CreateBatteryIcon(-1);
    g_currentIcon = initIcon;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = initIcon;
    wcscpy_s(nid.szTip, L"Rapoo 鼠标：等待数据…");
    Shell_NotifyIconW(NIM_ADD, &nid);

    // 启动读取线程
    std::thread reader(ReaderThread);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
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