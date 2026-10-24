// ================= 手柄位置/姿态微调 —— 二级窗口 =================
// 主界面只放一个入口按钮；6 个滑条（3 平移 + 3 旋转）放在独立的小窗口里。
//   · 旋转（度）：写渲染模型的 rotate_xyz，作用于 grip / openxr_grip / openxr_aim
//     （这三项是"抓取/瞄准位姿"，运行时按它摆放整个手模，所以转它们 = 整只手模转）
//   · 平移（毫米）：写所有零件的 origin（JSON 里单位是【米】，所以这里 mm/1000）
//     —— 之前强制写 0 是因为"实测没效果"，很可能当时单位和零件都搞错了；现在按毫米给回来。
// 用途：透视（房间视角）下真手柄和虚拟手模对不上时，用它把模型对齐到真手柄上。
//
// 原理：左手 / 右手各存一份偏移量(pose.ini)，客户端把 <安装目录>\pose_baseline\ 里的
//       基准渲染模型 + 偏移量，重新写成 SteamVR 实际读取的那份 JSON —— 永远从基准重算，不会越调越飘。
// 生效：渲染模型只在 SteamVR 启动时读一次，改完必须重启 SteamVR（可勾自动重启，由客户端代劳）。
#pragma once
#include <string>
#include <cstdio>

#define IDC_POSE_BTN   1060   // 主窗口：打开二级窗口

// ---- 二级窗口内的控件 ----
#define IDC_PW_L       1061
#define IDC_PW_R       1062
#define IDC_PW_SX      1063   // 滑条：0/1/2=旋转XYZ（半度）  3/4/5=平移XYZ（毫米）
#define IDC_PW_LBX     1100   // 数值标签（连续 6 个，避开滑条 id）
#define IDC_PW_APPLY   1069
#define IDC_PW_ZERO    1070
#define IDC_PW_RSTVR   1071
#define IDC_PW_CLOSE   1072
#define IDC_PW_MSG     1073

struct PoseOff { int x, y, z, rx, ry, rz; };
static const int kPoseMaxHalfDeg = 90;    // 旋转 ±45°（单位：半个度，步长 0.5°）
static const int kPoseMaxMm      = 150;   // 平移 ±150 毫米（步长 1mm）

static std::wstring PoseIniPath()     { return ExeDir() + L"\\pose.ini"; }
static std::wstring PoseBaselineDir() { return ExeDir() + L"\\pose_baseline"; }

static std::wstring PoseJsonPath(bool left) {
    std::wstring side = left ? L"left" : L"right";
    return DriverDir() + L"\\..\\..\\resources\\rendermodels\\huawei_controller_" + side +
           L"\\huawei_controller_" + side + L".json";
}

// ---- 偏移量读写（ini）----
static PoseOff ReadPoseOff(bool left) {
    const wchar_t* sec = left ? L"left" : L"right";
    std::wstring ini = PoseIniPath(); PoseOff o;
    o.x  = (int)GetPrivateProfileIntW(sec, L"x",  0, ini.c_str());
    o.y  = (int)GetPrivateProfileIntW(sec, L"y",  0, ini.c_str());
    o.z  = (int)GetPrivateProfileIntW(sec, L"z",  0, ini.c_str());
    o.rx = (int)GetPrivateProfileIntW(sec, L"rx", 0, ini.c_str());
    o.ry = (int)GetPrivateProfileIntW(sec, L"ry", 0, ini.c_str());
    o.rz = (int)GetPrivateProfileIntW(sec, L"rz", 0, ini.c_str());
    return o;
}
static void WritePoseOff(bool left, const PoseOff& o) {
    const wchar_t* sec = left ? L"left" : L"right";
    std::wstring ini = PoseIniPath();
    // 平移：单位【毫米】（JSON 里再换算成米）
    WritePrivateProfileStringW(sec, L"x",  Fmt(L"%d", o.x).c_str(), ini.c_str());
    WritePrivateProfileStringW(sec, L"y",  Fmt(L"%d", o.y).c_str(), ini.c_str());
    WritePrivateProfileStringW(sec, L"z",  Fmt(L"%d", o.z).c_str(), ini.c_str());
    WritePrivateProfileStringW(sec, L"rx", Fmt(L"%d", o.rx).c_str(), ini.c_str());
    WritePrivateProfileStringW(sec, L"ry", Fmt(L"%d", o.ry).c_str(), ini.c_str());
    WritePrivateProfileStringW(sec, L"rz", Fmt(L"%d", o.rz).c_str(), ini.c_str());
}

// ---- 在 JSON 文本里，把某个零件的某个字段整体平移 ----
// 找的是带引号的 "grip"，不会误伤 "handgrip" / "button_grip" / "openxr_grip"。
static bool ShiftFieldInJson(std::string& s, const char* comp, const char* field,
                             double d0, double d1, double d2) {
    std::string key = std::string("\"") + comp + std::string("\"");
    size_t k = s.find(key);
    if (k == std::string::npos) return false;
    std::string fld = std::string("\"") + field + std::string("\"");
    size_t o = s.find(fld, k);
    if (o == std::string::npos) return false;
    size_t lb = s.find('[', o), rb = s.find(']', lb);
    if (lb == std::string::npos || rb == std::string::npos) return false;
    std::string inner = s.substr(lb + 1, rb - lb - 1);
    double v[3];
    if (sscanf(inner.c_str(), " %lf , %lf , %lf", &v[0], &v[1], &v[2]) != 3) return false;
    v[0] += d0; v[1] += d1; v[2] += d2;
    char buf[240];
    snprintf(buf, sizeof(buf), "[%.8g, %.8g, %.8g]", v[0], v[1], v[2]);
    s.replace(lb, rb - lb + 1, buf);
    return true;
}

// ---- 用「基准 + 偏移」重写某个手的渲染模型 ----
// 把 JSON 里【每一个】 "origin" 都平移 (dx,dy,dz)（单位=米）。
// 每个零件的位置都是相对模型根的，所以整体平移它们 = 整个手模平移到你要的位置。
static int ShiftAllOrigins(std::string& s, double dx, double dy, double dz) {
    if (dx == 0.0 && dy == 0.0 && dz == 0.0) return 0;
    int n = 0;
    size_t pos = 0;
    const std::string fld = "\"origin\"";
    for (;;) {
        size_t o = s.find(fld, pos);
        if (o == std::string::npos) break;
        size_t lb = s.find('[', o), rb = s.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) break;
        std::string inner = s.substr(lb + 1, rb - lb - 1);
        double v[3];
        if (sscanf(inner.c_str(), " %lf , %lf , %lf", &v[0], &v[1], &v[2]) == 3) {
            v[0] += dx; v[1] += dy; v[2] += dz;
            char buf[240];
            snprintf(buf, sizeof(buf), "[%.8g, %.8g, %.8g]", v[0], v[1], v[2]);
            s.replace(lb, rb - lb + 1, buf);
            n++;
            pos = lb + strlen(buf);
        } else {
            pos = rb + 1;
        }
    }
    return n;
}

static bool WriteHandJson(bool left, const PoseOff& o, std::wstring& err) {
    std::wstring side = left ? L"left" : L"right";
    std::wstring base = PoseBaselineDir() + L"\\huawei_controller_" + side + L".json";
    FILE* f = _wfopen(base.c_str(), L"rb");
    if (!f) { err = L"缺少基准文件：" + base; return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::string s; if (n > 0) { s.resize((size_t)n); size_t rd = fread(&s[0], 1, (size_t)n, f); s.resize(rd); }
    fclose(f);
    bool bom = s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF;
    std::string body = bom ? s.substr(3) : s;

    double ax = o.rx / 2.0, ay = o.ry / 2.0, az = o.rz / 2.0;      // 半个度 -> 度
    static const char* comps[3] = { "grip", "openxr_grip", "openxr_aim" };
    int hitR = 0;
    for (int i = 0; i < 3; i++)
        if (ShiftFieldInJson(body, comps[i], "rotate_xyz", ax, ay, az)) hitR++;
    if (hitR == 0) { err = L"基准文件里找不到姿态零件（grip / openxr_grip / openxr_aim）"; return false; }
    // ★ 平移（毫米 -> 米）：所有零件的 origin 一起移
    const int hitT = ShiftAllOrigins(body, o.x / 1000.0, o.y / 1000.0, o.z / 1000.0);
    if ((o.x || o.y || o.z) && hitT == 0) { err = L"基准文件里找不到 origin 字段，平移没写进去"; return false; }

    std::wstring dst = PoseJsonPath(left);
    FILE* g = _wfopen(dst.c_str(), L"wb");
    if (!g) { err = L"写入失败（SteamVR 正在运行会占用文件，先关掉 VR 再试）：\n" + dst; return false; }
    if (bom) fwrite("\xEF\xBB\xBF", 1, 3, g);
    fwrite(body.data(), 1, body.size(), g);
    fclose(g);
    return true;
}

// ================= 二级窗口 =================
static HWND g_pwSlider[3] = { 0 };
static HWND g_pwVal[3] = { 0 };
static HWND g_pwMsg = NULL;

static bool PwLeft(HWND h) {
    return SendMessageW(GetDlgItem(h, IDC_PW_L), BM_GETCHECK, 0, 0) == BST_CHECKED;
}
static void PwSync(HWND h) {
    (void)h;
    for (int i = 0; i < 3; i++) {
        int v = (int)SendMessageW(g_pwSlider[i], TBM_GETPOS, 0, 0);
        SetWindowTextW(g_pwVal[i], Fmt(L"%+.1f°", v / 2.0).c_str());
    }
}
static void PwLoad(HWND h) {
    PoseOff o = ReadPoseOff(PwLeft(h));
    int v[3] = { o.rx, o.ry, o.rz };
    for (int i = 0; i < 3; i++) SendMessageW(g_pwSlider[i], TBM_SETPOS, TRUE, v[i]);
    PwSync(h);
}
static void PwApply(HWND h) {
    PoseOff o; o.x = o.y = o.z = 0;
    o.rx = (int)SendMessageW(g_pwSlider[0], TBM_GETPOS, 0, 0);
    o.ry = (int)SendMessageW(g_pwSlider[1], TBM_GETPOS, 0, 0);
    o.rz = (int)SendMessageW(g_pwSlider[2], TBM_GETPOS, 0, 0);
    WritePoseOff(PwLeft(h), o);

    std::wstring err;
    if (!WriteHandJson(true,  ReadPoseOff(true),  err)) { SetWindowTextW(g_pwMsg, (L"左手写入失败 ✗\n" + err).c_str()); return; }
    if (!WriteHandJson(false, ReadPoseOff(false), err)) { SetWindowTextW(g_pwMsg, (L"右手写入失败 ✗\n" + err).c_str()); return; }

    PoseOff L = ReadPoseOff(true), R = ReadPoseOff(false);
    std::wstring msg = Fmt(L"已写入 ✓   左手 %+.1f/%+.1f/%+.1f°   右手 %+.1f/%+.1f/%+.1f°",
                           L.rx / 2.0, L.ry / 2.0, L.rz / 2.0, R.rx / 2.0, R.ry / 2.0, R.rz / 2.0);
    if (SendMessageW(GetDlgItem(h, IDC_PW_RSTVR), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        std::wstring r1, r2;
        if (SteamVrRunning()) { KillSteamVr(r1); Sleep(1500); }
        StartSteamVr(r2);
        msg += L"\nSteamVR 已重启，戴上就能看到效果";
    } else {
        msg += L"\n（未自动重启：要手动关掉再打开 SteamVR 才生效）";
    }
    SetWindowTextW(g_pwMsg, msg.c_str());
}
static LRESULT CALLBACK PoseWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HFONT f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Microsoft YaHei UI");
        Mk(h, L"BUTTON", L"○ 左手", BS_AUTORADIOBUTTON | WS_GROUP, 20, 14, 84, 22, IDC_PW_L, f);
        Mk(h, L"BUTTON", L"○ 右手", BS_AUTORADIOBUTTON, 112, 14, 84, 22, IDC_PW_R, f);
        SendMessageW(GetDlgItem(h, IDC_PW_R), BM_SETCHECK, BST_CHECKED, 0);
        const wchar_t* nm[3] = { L"俯仰 X", L"偏航 Y", L"滚转 Z" };
        for (int i = 0; i < 3; i++) {
            int y = 50 + i * 34;
            Mk(h, L"STATIC", nm[i], SS_LEFT, 20, y + 6, 62, 20, 0, f);
            g_pwSlider[i] = Mk(h, L"msctls_trackbar32", L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
                               86, y, 230, 26, IDC_PW_SX + i, NULL);
            SendMessageW(g_pwSlider[i], TBM_SETRANGE, TRUE, (LPARAM)MAKELONG(-kPoseMaxHalfDeg, kPoseMaxHalfDeg));
            SendMessageW(g_pwSlider[i], TBM_SETTICFREQ, 20, 0);
            SendMessageW(g_pwSlider[i], TBM_SETPOS, TRUE, 0);
            g_pwVal[i] = Mk(h, L"STATIC", L"+0.0°", SS_LEFT, 322, y + 6, 110, 20, IDC_PW_LBX + i, f);
        }
        Mk(h, L"STATIC", L"左右手各存一套；改完需重启 SteamVR（下方可勾自动）。", SS_LEFT, 20, 156, 420, 20, 0, f);
        Mk(h, L"BUTTON", L"应用偏移", BS_PUSHBUTTON, 20, 182, 110, 28, IDC_PW_APPLY, f);
        Mk(h, L"BUTTON", L"全部归零", BS_PUSHBUTTON, 140, 182, 96, 28, IDC_PW_ZERO, f);
        Mk(h, L"BUTTON", L"关闭",    BS_PUSHBUTTON, 356, 182, 88, 28, IDC_PW_CLOSE, f);
        Mk(h, L"BUTTON", L"应用后自动重启 SteamVR", BS_AUTOCHECKBOX, 20, 218, 250, 22, IDC_PW_RSTVR, f);
        SendMessageW(GetDlgItem(h, IDC_PW_RSTVR), BM_SETCHECK, BST_CHECKED, 0);
        g_pwMsg = Mk(h, L"STATIC", L"就绪（先选左手/右手，再拖滑条）", SS_LEFT, 20, 246, 424, 44, IDC_PW_MSG, f);
        PwLoad(h);
        return 0;
    }
    case WM_HSCROLL: PwSync(h); return 0;
    case WM_COMMAND: {
        switch (LOWORD(w)) {
        case IDC_PW_L: case IDC_PW_R: PwLoad(h); return 0;
        case IDC_PW_APPLY: PwApply(h); return 0;
        case IDC_PW_ZERO:
            for (int i = 0; i < 3; i++) SendMessageW(g_pwSlider[i], TBM_SETPOS, TRUE, 0);
            PwSync(h);
            SetWindowTextW(g_pwMsg, L"三个角度已归零（还没写文件 —— 点「应用偏移」才生效）");
            return 0;
        case IDC_PW_CLOSE: DestroyWindow(h); return 0;
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void OpenPoseWindow(HWND owner) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = PoseWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HwVRGlassLitePoseWnd";
        RegisterClassExW(&wc);
        reg = true;
    }
    HWND exist = FindWindowW(L"HwVRGlassLitePoseWnd", NULL);
    if (exist) { SetForegroundWindow(exist); return; }
    HWND h = CreateWindowExW(0, L"HwVRGlassLitePoseWnd", L"手柄姿态微调（旋转）",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 470, 350,
                             owner, NULL, GetModuleHandleW(NULL), NULL);
    if (h) { ShowWindow(h, SW_SHOW); UpdateWindow(h); }
}
