// ============================================================================
//  HwVRGlassLite —— 华为 VR Glass 轻量客户端（替代原版 HwPCVRAssistant）
//
//  功能：
//    1. 设备状态：6DOF 设备 / 头显显示 / 驱动注册 / SteamVR 运行 / 当前模式
//    2. 一键启动·关闭 SteamVR
//    3. 震动补丁面板（模式 + 时长上限 + 最小间隔）
//    4. 原厂 ⇄ 补丁 一键切换（带 SHA256 校验，绝不破坏原厂文件）
//
//  设计原则：不修改厂商任何原始文件；原厂 DLL 只是"改名保存"，
//            切回原厂时逐字节还原（哈希与厂商清单一致）。
//
//  编译：见 build.sh（mingw-w64 g++）
// ============================================================================
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include "openvr.h"          // 只为接口定义（运行时用 GetProcAddress 拿 openvr_api.dll 的 C 导出）
#include <shlobj.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cwctype>
#include <commctrl.h>
#include "sha256.h"

// ---------------------------------------------------------------- 常量
static const wchar_t* kAppName    = L"HwVRGlassLite";
static const wchar_t* kTitle      = L"华为 VR Glass 客户端 (Lite) v1.4.5";
static const wchar_t* kOriginalSha=
    L"A4D70633ACB659C4A404533E9EA178457F06176AE08F47B4ADB6FDEAED5B3D07"; // 原厂 driver_huaweiVR.dll
static const wchar_t* kProxyName  = L"driver_huaweiVR.dll";
static const wchar_t* kRealName   = L"driver_huaweiVR_real.dll";
static const wchar_t* kIniName    = L"haptic_patch.ini";
static const wchar_t* kLogName    = L"haptic_patch.log";
static const wchar_t* kDefaultBase= L"C:\\Program\\HwPCVR Assistant";
static const wchar_t* kDefaultSv  = L"C:\\Program\\Steam\\steamapps\\common\\SteamVR";

// ---------------------------------------------------------------- 基础工具
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring s(buf);
    size_t p = s.find_last_of(L'\\');
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}
static std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048]; va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 2048 - 1, fmt, ap); buf[2047] = 0; va_end(ap);
    return buf;
}
static std::wstring ReadRegStr(HKEY root, const wchar_t* sub, const wchar_t* val, const std::wstring& def) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return def;
    wchar_t buf[1024] = {0}; DWORD sz = sizeof(buf), type = 0;
    std::wstring out = def;
    if (RegQueryValueExW(k, val, NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ)
        out = buf;
    RegCloseKey(k);
    return out;
}
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring Sha(const std::wstring& p, bool* ok = nullptr) {
    bool o = false;
    std::string h = sha256::ofFile(p, &o);
    if (ok) *ok = o;
    if (!o) return L"(读取失败)";
    std::wstring w(h.begin(), h.end());
    return w;
}
static bool CopyTo(const std::wstring& s, const std::wstring& d) {
    return CopyFileW(s.c_str(), d.c_str(), FALSE) != 0;
}
static bool DelFile(const std::wstring& p) {
    if (!FileExists(p)) return true;
    return DeleteFileW(p.c_str()) != 0;
}
static bool Rename(const std::wstring& a, const std::wstring& b) {
    return MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// ---------------------------------------------------------------- 环境路径
static std::wstring BaseDir() {
    // 优先用"我们自己所在的目录"：安装后驱动就在 <安装目录>\steamVRDriver\huaweiVR\bin\win64
    if (FileExists(ExeDir() + L"\\steamVRDriver\\huaweiVR\\bin\\win64\\driver_huaweiVR_real.dll") ||
        FileExists(ExeDir() + L"\\steamVRDriver\\huaweiVR\\bin\\win64\\driver_huaweiVR.dll"))
        return ExeDir();
    // 退路：原版助手留下的注册表 / 默认路径
    std::wstring p = ReadRegStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"Path", L"");
    return p.empty() ? kDefaultBase : p;
}
static std::wstring DriverDir()  { return BaseDir() + L"\\steamVRDriver\\huaweiVR\\bin\\win64"; }
static std::wstring OpenVrPaths();      // 定义见后面
static std::wstring SteamVRDir() {
    // ⓪ 最权威：%LOCALAPPDATA%\openvr\openvrpaths.vrpath 里 SteamVR 自己记的 runtime 路径
    {
        FILE* f = _wfopen(OpenVrPaths().c_str(), L"rb");
        if (f) {
            std::string s; char b[8192]; size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
            fclose(f);
            const int wl = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
            std::wstring w(wl, 0);
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wl);
            const size_t k = w.find(L"\"runtime\"");
            if (k != std::wstring::npos) {
                const size_t lb = w.find(L'[', k);
                const size_t q0 = (lb == std::wstring::npos) ? std::wstring::npos : w.find(L'"', lb + 1);
                if (q0 != std::wstring::npos) {
                    const size_t q1 = w.find(L'"', q0 + 1);
                    if (q1 != std::wstring::npos) {
                        const std::wstring raw = w.substr(q0 + 1, q1 - q0 - 1);
                        std::wstring o;
                        for (size_t i = 0; i < raw.size(); i++) {
                            if (raw[i] == L'\\' && i + 1 < raw.size() && raw[i + 1] == L'\\') { o += L'\\'; i++; }
                            else o += raw[i];
                        }
                        if (FileExists(o + L"\\bin\\win64\\vrmonitor.exe")) return o;
                    }
                }
            }
        }
    }
    // ① 原版助手留下的（若装过）
    std::wstring p = ReadRegStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"SteamVRInstalledPath", L"");
    if (!p.empty() && FileExists(p + L"\\bin\\win64\\vrmonitor.exe")) return p;
    // ② 从 Steam 自己的注册表推（任何电脑都适用）
    const wchar_t* keys[3] = { L"SteamPath", L"InstallPath", L"SteamExe" };
    const HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 3; i++) {
            std::wstring v = ReadRegStr(roots[r], L"SOFTWARE\\Valve\\Steam", keys[i], L"");
            if (v.empty()) continue;
            if (v.size() > 4 && _wcsicmp(v.c_str() + v.size() - 4, L".exe") == 0) {   // SteamExe 是完整 exe 路径
                size_t s = v.find_last_of(L"\\/"); if (s != std::wstring::npos) v = v.substr(0, s);
            }
            std::wstring cand = v + L"\\steamapps\\common\\SteamVR";
            if (FileExists(cand + L"\\bin\\win64\\vrmonitor.exe")) return cand;
        }
    }
    // ③ 常见路径兜底
    const wchar_t* cands[5] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR",
        L"C:\\Program Files\\Steam\\steamapps\\common\\SteamVR",
        L"D:\\Steam\\steamapps\\common\\SteamVR",
        L"D:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR",
        kDefaultSv };
    for (int i = 0; i < 5; i++)
        if (cands[i] && FileExists(std::wstring(cands[i]) + L"\\bin\\win64\\vrmonitor.exe")) return cands[i];
    return kDefaultSv;
}
static std::wstring PathIni() { return DriverDir() + L"\\" + kIniName; }
static std::wstring PathLog() { return DriverDir() + L"\\" + kLogName; }
static std::wstring PathProxySrc() { return ExeDir() + L"\\patch\\" + kProxyName; }
static std::wstring PathIniSrc()   { return ExeDir() + L"\\patch\\" + kIniName; }
static std::wstring OpenVrPaths() {
    wchar_t buf[MAX_PATH] = {0};
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf))) return L"";
    return std::wstring(buf) + L"\\openvr\\openvrpaths.vrpath";
}

// ---------------------------------------------------------------- 驱动注册 / 注销（原生实现）
// 旧版是靠 register.ps1 + "1-注册驱动(管理员).cmd"：里面有个 pause，安装器又是隐藏等待，
// 于是"装到写注册表就卡住"（Win10 用户反馈）；还顺手 reg import 了一个 NVIDIA 白名单 .reg。
// 现在全部改成这里原生做：不依赖 PowerShell、不弹子进程、不写任何与本机环境绑定的东西。
static bool RunHiddenWait(const std::wstring& exe, const std::wstring& args, DWORD* code) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 60000);
    if (code) GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}
static bool RegWriteStr(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY k = NULL;
    if (RegCreateKeyExW(root, sub.c_str(), 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                                  (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
// 把驱动目录写进 SteamVR 的登记表：优先用它自带的 vrpathreg.exe（官方做法），
// 找不到就退回直接改 %LOCALAPPDATA%\openvr\openvrpaths.vrpath 的 external_drivers。
static bool VrPathRegRun(const std::wstring& steamvr, const std::wstring& drv, const wchar_t* verb, std::wstring& out) {
    const std::wstring exe = steamvr + L"\\bin\\win64\\vrpathreg.exe";
    if (!FileExists(exe)) return false;
    DWORD code = 0;
    if (!RunHiddenWait(exe, Fmt(L"%s \"%s\"", verb, drv.c_str()), &code)) return false;
    out += Fmt(L"  vrpathreg %s → exit=%lu\r\n", verb, code);
    return true;
}
static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring o; for (wchar_t c : s) { if (c == L'\\') o += L"\\\\"; else o += c; } return o;
}
static void RegisterDriver(bool unregister, std::wstring& msg) {
    const std::wstring drv = BaseDir() + L"\\steamVRDriver\\huaweiVR";
    const std::wstring steamvr = SteamVRDir();
    const bool hasVr = FileExists(steamvr + L"\\bin\\win64\\vrmonitor.exe");
    std::wstring r;
    bool done = false;
    if (hasVr) {
        if (unregister) {
            done = VrPathRegRun(steamvr, drv, L"removedriver", r);
        } else {
            VrPathRegRun(steamvr, drv, L"removedriver", r);          // 先删掉旧的，避免重复登记
            done = VrPathRegRun(steamvr, drv, L"adddriver", r);
        }
    }
    // 兜底：直接改 openvrpaths.vrpath（SteamVR 还没装/没跑过时也能登记上）
    if (!done) {
        const std::wstring vp = OpenVrPaths();
        FILE* f = _wfopen(vp.c_str(), L"rb");
        if (f) {
            std::string s; char b[8192]; size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
            fclose(f);
            // 转成宽字符做简单字符串处理
            int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
            std::wstring w(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wlen);
            const std::wstring quoted = L"\"" + JsonEscape(drv) + L"\"";
            if (w.find(quoted) != std::wstring::npos) {
                r += L"  openvrpaths.vrpath 里已经有本驱动登记\r\n"; done = true;
            } else {
                const size_t k = w.find(L"\"external_drivers\"");
                const size_t lb = (k == std::wstring::npos) ? std::wstring::npos : w.find(L'[', k);
                if (lb != std::wstring::npos) {
                    w.insert(lb + 1, L"\r\n\t\t" + quoted + L",");
                    FILE* o = _wfopen(vp.c_str(), L"wb");
                    if (o) {
                        int u8 = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
                        std::string o8(u8, 0);
                        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &o8[0], u8, NULL, NULL);
                        fwrite(o8.data(), 1, o8.size(), o); fclose(o);
                        r += L"  已写入 openvrpaths.vrpath 的 external_drivers\r\n"; done = true;
                    }
                }
            }
        }
    }
    if (!unregister) {
        // 厂商驱动靠 HKCU\SOFTWARE\HuaweiPCVRAssistant 找 SteamVR 路径 —— 保留这一步（官方助手共用同一份配置）
        const std::wstring k = L"SOFTWARE\\HuaweiPCVRAssistant";
        RegWriteStr(HKEY_CURRENT_USER, k, L"Path", BaseDir());
        if (hasVr) RegWriteStr(HKEY_CURRENT_USER, k, L"SteamVRInstalledPath", steamvr);
        RegWriteStr(HKEY_CURRENT_USER, k, L"SteamVrFirstRunningStatus", L"TRUE");
        RegWriteStr(HKEY_CURRENT_USER, k, L"XmlStatus",  L"true");
        RegWriteStr(HKEY_CURRENT_USER, k, L"GetXmlData", L"true");
        RegWriteStr(HKEY_CURRENT_USER, k, L"DirectMode", L"true");
        r += Fmt(L"  已写入 HKCU\\SOFTWARE\\HuaweiPCVRAssistant（厂商驱动据此定位 SteamVR：%s）\r\n",
                 hasVr ? steamvr.c_str() : L"未找到，跳过");
    }
    if (!done) r += L"  ★没能登记（SteamVR 还没装/没启动过一次）—— 装好 SteamVR 启动一次后，再执行一次本操作即可。\r\n";
    msg = (unregister ? L"驱动已从 SteamVR 注销\r\n" : L"驱动已登记进 SteamVR\r\n") + r;
}

// ---------------------------------------------------------------- 设备检测
static bool Has6DofDevice() {
    // HID 枚举，找 vid_12d1&pid_3a09（VR Glass 的 6DOF/手柄 HID 接口）
    HMODULE hid = LoadLibraryW(L"hid.dll");
    if (!hid) return false;
    typedef void (WINAPI *PFN_GUID)(GUID*);
    PFN_GUID pGet = (PFN_GUID)GetProcAddress(hid, "HidD_GetHidGuid");
    if (!pGet) { FreeLibrary(hid); return false; }
    GUID g; pGet(&g);
    HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    bool found = false;
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
        for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(di, NULL, &g, i, &ifd); i++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
            if (!need) continue;
            std::vector<BYTE> bufv(need);
            SP_DEVICE_INTERFACE_DETAIL_DATA_W* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)bufv.data();
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, NULL, NULL)) {
                std::wstring p = det->DevicePath;
                for (auto& c : p) c = towlower(c);
                if (p.find(L"vid_12d1&pid_3a09") != std::wstring::npos) found = true;
            }
        }
        SetupDiDestroyDeviceInfoList(di);
    }
    FreeLibrary(hid);
    return found;
}
static bool HasHmdDisplay() {
    // 用 SetupAPI 枚举“当前存在的监视器设备”，头显在直连模式下也能识别到
    static const GUID kMonitorClass = { 0x4d36e96e, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    HDEVINFO di = SetupDiGetClassDevsW(&kMonitorClass, NULL, NULL, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA d; ZeroMemory(&d, sizeof(d)); d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &d); i++) {
        wchar_t id[512] = {0};
        DWORD need = 0;
        if (SetupDiGetDeviceInstanceIdW(di, &d, id, 511, &need)) {
            std::wstring s = id;
            for (auto& c : s) c = towupper(c);
            if (s.find(L"DISPLAY\\HWV") != std::wstring::npos || s.find(L"HUAWEI") != std::wstring::npos) { found = true; break; }
        }
        d.cbSize = sizeof(d);
    }
    SetupDiDestroyDeviceInfoList(di);
    return found;
}
static bool DriverRegistered() {
    std::wstring p = OpenVrPaths();
    if (p.empty() || !FileExists(p)) return false;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    std::string all; char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, r);
    fclose(f);
    return all.find("huaweiVR") != std::string::npos;
}

// ---------------------------------------------------------------- 进程
static const wchar_t* kSteamVrProcs[] = {
    L"vrserver.exe", L"vrmonitor.exe", L"vrcompositor.exe", L"vrdashboard.exe", L"vrwebhelper.exe"
};
static std::vector<DWORD> FindProcs(const wchar_t* name) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) do {
        if (_wcsicmp(pe.szExeFile, name) == 0) out.push_back(pe.th32ProcessID);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return out;
}
static bool SteamVrRunning(bool* elevatedFail = nullptr) {
    bool any = false, fail = false;
    for (auto* n : kSteamVrProcs) {
        auto v = FindProcs(n);
        if (!v.empty()) any = true;
    }
    if (elevatedFail) *elevatedFail = fail;
    return any;
}
static bool KillSteamVr(std::wstring& report) {
    bool ok = true;
    for (auto* n : kSteamVrProcs) {
        for (DWORD pid : FindProcs(n)) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (!h) { report += Fmt(L"  无法结束 %s (PID %lu)：权限不足（它由管理员/助手启动）\n", n, pid); ok = false; continue; }
            if (TerminateProcess(h, 0)) report += Fmt(L"  已结束 %s (PID %lu)\n", n, pid);
            else { report += Fmt(L"  结束 %s 失败\n", n); ok = false; }
            CloseHandle(h);
        }
    }
    return ok;
}
static bool SetAssistantDisabled(bool dis, std::wstring& msg);   // 定义在后面
static void HoldAssistantMutex(bool hold);
static bool StartSteamVr(std::wstring& report) {
    if (!FileExists(DriverDir() + L"\\driver_huaweiVR_real.dll")) {
        report = L"当前不是补丁模式：请用官方助手启动，或到「高级设置」切换到补丁模式后再启动。";
        return false;
    }
    std::wstring exe = SteamVRDir() + L"\\bin\\win64\\vrmonitor.exe";
    if (!FileExists(exe)) { report = L"找不到 SteamVR (vrmonitor.exe)：\n" + exe; return false; }
    HINSTANCE r = ShellExecuteW(NULL, L"open", exe.c_str(), NULL, SteamVRDir().c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) { report = Fmt(L"启动 SteamVR 失败（err=%lld）", (long long)(INT_PTR)r); return false; }
    report = L"已请求启动 SteamVR（约 20 秒后就绪）";
    return true;
}

// ---------------------------------------------------------------- 管理员权限
// 结束 SteamVR、切换驱动 DLL 都需要管理员权限（SteamVR 常被提权启动，普通权限杀不掉）
static bool IsElevated() {
    HANDLE h = NULL;
    TOKEN_ELEVATION te = { 0 }; DWORD sz = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)) return false;
    bool ok = GetTokenInformation(h, TokenElevation, &te, sizeof(te), &sz) && te.TokenIsElevated != 0;
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------- 阻止原版助手自动启动
// 原理：驱动 driver_huaweiVR.dll 用命名互斥体 "HwPCVRAssistant.exe" 判断助手是否在运行，
//       不存在就 ShellExecute 把它拉起来。我们占住同名互斥体 = "假装助手已在运行"，
//       驱动就不会再启动它。不修改任何厂商文件、不需要管理员、随时可逆。
static HANDLE g_assistantMutex = NULL;
static void HoldAssistantMutex(bool hold) {
    if (hold) { if (!g_assistantMutex) g_assistantMutex = CreateMutexW(NULL, FALSE, L"HwPCVRAssistant.exe"); }
    else { if (g_assistantMutex) { CloseHandle(g_assistantMutex); g_assistantMutex = NULL; } }
}
static bool AssistantRunning() { return !FindProcs(L"HwPCVRAssistant.exe").empty(); }
// 判断某个进程是否加载了指定模块（用于提示“Steam 占着驱动 DLL”）
static bool ProcessHasModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me; ZeroMemory(&me, sizeof(me)); me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}
static bool SteamHoldsDriver() {
    std::vector<DWORD> pids = FindProcs(L"steam.exe");
    for (size_t i = 0; i < pids.size(); i++)
        if (ProcessHasModule(pids[i], L"driver_huaweiVR.dll") ||
            ProcessHasModule(pids[i], L"driver_huaweiVR_real.dll")) return true;
    return false;
}
// 切换需要 Steam 释放驱动 DLL：提示后自动退出，切完再拉起来
static bool EnsureDriverUnlocked(std::wstring& msg, bool& restartSteam) {
    restartSteam = false;
    if (!SteamHoldsDriver()) return true;
    int r = MessageBoxW(NULL,
        L"切换驱动需要先完全退出 Steam（它正占用驱动 DLL）。\n\n现在自动退出 Steam 并继续切换吗？",
        L"HwVRGlassLite", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) { msg = L"已取消：请完全退出 Steam 后再切换。"; return false; }
    std::vector<DWORD> pids = FindProcs(L"steam.exe");
    for (size_t i = 0; i < pids.size(); i++) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pids[i]);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    for (int i = 0; i < 10; i++) { Sleep(500); if (!SteamHoldsDriver()) break; }
    restartSteam = true;
    return true;
}
struct SteamRestarter {
    bool on;
    SteamRestarter() : on(false) {}
    ~SteamRestarter() {
        if (!on) return;
        std::wstring exe = SteamVRDir() + L"\\..\\..\\..\\..\\steam.exe";
        if (FileExists(exe)) ShellExecuteW(NULL, L"open", exe.c_str(), L"-silent", NULL, SW_SHOWNORMAL);
    }
};
// 切换驱动前：SteamVR 在跑就先询问并自动关掉
static bool EnsureSteamVrClosed(std::wstring& msg, std::wstring& report) {
    if (!SteamVrRunning()) return true;
    int r = MessageBoxW(NULL,
        L"切换驱动需要先关闭 SteamVR。\n\n现在自动关闭 SteamVR 并继续切换吗？",
        L"HwVRGlassLite", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) { msg = L"已取消：请先关闭 SteamVR 再切换。"; return false; }
    KillSteamVr(report);
    Sleep(1500);
    return true;
}

// ---------------------------------------------------------------- 禁用原版助手（改名法）
// 驱动是用 ShellExecute 按“文件名”去拉起 HwPCVRAssistant.exe 的。
// 把 exe 改个名 → 驱动拉不起来（它只是执行失败，不影响驱动和 SteamVR 正常工作）。
// 完全可逆：需要原版助手时改回来即可。
// 从 openvrpaths.vrpath 读出当前登记的所有 huaweiVR 驱动目录
static std::wstring ReadOpenVrPathsText() {
    FILE* f = _wfopen(OpenVrPaths().c_str(), L"rb");
    if (!f) return L"";
    std::string s; char b[8192]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    int wl = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(wl, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wl);
    return w;
}
static void RegisteredDriverDirs(std::vector<std::wstring>& out) {
    out.clear();
    std::wstring w = ReadOpenVrPathsText();
    const std::wstring tag = L"huaweiVR";
    size_t pos = 0;
    while ((pos = w.find(tag, pos)) != std::wstring::npos) {
        size_t q0 = pos;
        while (q0 > 0 && w[q0] != L'"') q0--;
        size_t q1 = pos + tag.size();
        while (q1 < w.size() && w[q1] != L'"') q1++;
        if (q0 < pos && q1 < w.size()) {
            std::wstring raw = w.substr(q0 + 1, q1 - q0 - 1), un;
            for (size_t i = 0; i < raw.size(); i++) {
                wchar_t c = raw[i];
                if (c == L'\\' && i + 1 < raw.size() && raw[i + 1] == L'\\') i++;
                un += c;
            }
            if (un.find(L"\\steamVRDriver\\huaweiVR") != std::wstring::npos) out.push_back(un);
        }
        pos = q1 + 1;
    }
}
static int RemoveForeignDriverRegs(const std::wstring& keepDir) {
    std::wstring w = ReadOpenVrPathsText();
    if (w.empty()) return 0;
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= w.size()) {
        size_t e = w.find(L'\n', start);
        lines.push_back(w.substr(start, (e == std::wstring::npos ? w.size() : e) - start));
        if (e == std::wstring::npos) break;
        start = e + 1;
    }
    std::vector<std::wstring> keep;
    int removed = 0;
    std::wstring keepEsc;
    for (size_t k = 0; k < keepDir.size(); k++) {
        if (keepDir[k] == L'\\') keepEsc += L"\\\\"; else keepEsc += keepDir[k];
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const std::wstring& L = lines[i];
        bool isOur = L.find(keepEsc) != std::wstring::npos || L.find(keepDir) != std::wstring::npos;
        if (L.find(L"huaweiVR") != std::wstring::npos && !isOur) { removed++; continue; }
        keep.push_back(L);
    }
    if (!removed) return 0;
    size_t a = std::wstring::npos, b = std::wstring::npos;
    for (size_t i = 0; i < keep.size(); i++) {
        if (a == std::wstring::npos && keep[i].find(L"\"external_drivers\"") != std::wstring::npos) a = i;
        if (a != std::wstring::npos && keep[i].find(L']') != std::wstring::npos) { b = i; break; }
    }
    if (a != std::wstring::npos && b != std::wstring::npos) {
        std::vector<size_t> elems;
        for (size_t i = a + 1; i < b; i++)
            if (keep[i].find(L'"') != std::wstring::npos) elems.push_back(i);
        for (size_t k = 0; k < elems.size(); k++) {
            std::wstring& s = keep[elems[k]];
            while (!s.empty() && (s.back() == L',' || s.back() == L'\r')) s.pop_back();
            if (k + 1 < elems.size()) s += L',';
        }
    }
    std::wstring outw;
    for (size_t i = 0; i < keep.size(); i++) { outw += keep[i]; if (i + 1 < keep.size()) outw += L'\n'; }
    FILE* o = _wfopen(OpenVrPaths().c_str(), L"wb");
    if (!o) return 0;
    int u8 = WideCharToMultiByte(CP_UTF8, 0, outw.c_str(), (int)outw.size(), NULL, 0, NULL, NULL);
    std::string o8(u8, 0);
    WideCharToMultiByte(CP_UTF8, 0, outw.c_str(), (int)outw.size(), &o8[0], u8, NULL, NULL);
    fwrite(o8.data(), 1, o8.size(), o);
    fclose(o);
    return removed;
}
static void SaveOfficialDir(const std::wstring& d) {
    if (d.empty()) return;
    FILE* f = _wfopen((ExeDir() + L"\\official_dir.txt").c_str(), L"wb");
    if (!f) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, d.c_str(), -1, NULL, 0, NULL, NULL);
    std::string u8(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, d.c_str(), -1, &u8[0], n, NULL, NULL);
    fwrite(u8.c_str(), 1, u8.size() - 1, f);
    fclose(f);
}
static std::wstring LoadOfficialDir() {
    FILE* f = _wfopen((ExeDir() + L"\\official_dir.txt").c_str(), L"rb");
    if (!f) return L"";
    std::string s; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    if (s.empty()) return L"";
    int wl = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(wl, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wl);
    return w;
}
static std::wstring OfficialDir() {
    std::wstring memo = LoadOfficialDir();
    if (!memo.empty() && _wcsicmp(memo.c_str(), BaseDir().c_str()) != 0) return memo;
    std::wstring p = ReadRegStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"Path", L"");
    if (!p.empty() && _wcsicmp(p.c_str(), BaseDir().c_str()) != 0) return p;
    std::vector<std::wstring> dirs; RegisteredDriverDirs(dirs);
    for (size_t i = 0; i < dirs.size(); i++) {
        size_t k = dirs[i].rfind(L"\\steamVRDriver\\huaweiVR");
        if (k == std::wstring::npos) continue;
        std::wstring root = dirs[i].substr(0, k);
        if (_wcsicmp(root.c_str(), BaseDir().c_str()) != 0) return root;
    }
    return L"";
}
static std::wstring AssistantDir() {
    std::wstring p = ReadRegStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"Path", L"");
    if (!p.empty() && (FileExists(p + L"\\HwPCVRAssistant.exe") || FileExists(p + L"\\HwPCVRAssistant.exe.disabled")))
        return p;
    std::wstring o = OfficialDir();
    if (!o.empty() && (FileExists(o + L"\\HwPCVRAssistant.exe") || FileExists(o + L"\\HwPCVRAssistant.exe.disabled")))
        return o;
    return BaseDir();
}
static std::wstring AssistantExePath()      { return AssistantDir() + L"\\HwPCVRAssistant.exe"; }
static std::wstring AssistantExeDisabled()  { return AssistantDir() + L"\\HwPCVRAssistant.exe.disabled"; }
static bool AssistantDisabled()             { return FileExists(AssistantExeDisabled()); }
static bool SetAssistantDisabled(bool dis, std::wstring& msg) {
    if (dis) {
        std::vector<DWORD> pids = FindProcs(L"HwPCVRAssistant.exe");
        for (size_t i = 0; i < pids.size(); i++) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pids[i]);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); }
        }
        if (!pids.empty()) Sleep(1000);
        if (!FileExists(AssistantExePath())) {
            msg = AssistantDisabled() ? L"原版助手已处于禁用状态（文件已是 .disabled）。"
                                      : L"没找到 HwPCVRAssistant.exe，无法禁用。";
            return AssistantDisabled();
        }
        if (!MoveFileExW(AssistantExePath().c_str(), AssistantExeDisabled().c_str(), MOVEFILE_REPLACE_EXISTING)) {
            msg = Fmt(L"改名失败（错误码 %lu），文件可能被占用。", GetLastError());
            return false;
        }
        msg = L"已禁用原版助手：HwPCVRAssistant.exe → .disabled，驱动再也拉不起它（可随时恢复）。";
        return true;
    }
    if (!FileExists(AssistantExeDisabled())) { msg = L"没有被改名的助手文件，无需恢复。"; return true; }
    if (!MoveFileExW(AssistantExeDisabled().c_str(), AssistantExePath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        msg = Fmt(L"恢复失败（错误码 %lu）。", GetLastError());
        return false;
    }
    msg = L"已恢复原版助手：文件名已改回 HwPCVRAssistant.exe。";
    return true;
}

// ---------------------------------------------------------------- 震动配置
// ★ 只有三个模式：0=透传(原厂行为) 2=翻译(震动补丁) 3=静音(完全不震)
//   参数（单包时长/续包周期等）已经全部收进 patch\haptic_patch.ini，客户端不再暴露调参。
static const int kModeVal[3] = { 0, 2, 3 };
struct HapticCfg { int mode = 2; };
static HapticCfg ReadIni() {
    HapticCfg c;
    FILE* f = _wfopen(PathIni().c_str(), L"rb");
    if (!f) return c;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '[' || line[0] == '\n' || line[0] == '\r') continue;
        int v;
        if (sscanf(line, "mode=%d", &v) == 1 && v >= 0 && v <= 3) c.mode = v;
    }
    fclose(f);
    return c;
}
// 只改 mode 行：其余参数以 包内 patch\haptic_patch.ini 为模板（保证与代理版本一致）
static bool WriteIni(int mode, std::wstring& err) {
    std::string text;
    FILE* ft = _wfopen(PathIniSrc().c_str(), L"rb");
    if (ft) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), ft)) > 0) text.append(buf, n); fclose(ft); }
    if (text.empty()) text = "[patch]\nmode=2\n";
    std::string out; size_t i = 0; bool done = false;
    while (i <= text.size()) {
        size_t e = text.find('\n', i);
        std::string line = text.substr(i, (e == std::string::npos ? text.size() : e) - i);
        if (!done && line.rfind("mode=", 0) == 0) {
            char mb[32]; snprintf(mb, sizeof(mb), "mode=%d", mode);
            line = mb; done = true;
        }
        out += line;
        if (e == std::string::npos) break;
        out += '\n'; i = e + 1;
    }
    if (!done) { char mb[32]; snprintf(mb, sizeof(mb), "mode=%d\n", mode); out += mb; }
    FILE* f = _wfopen(PathIni().c_str(), L"wb");
    if (!f) { err = L"写入失败：目录只读或路径不存在\n" + PathIni(); return false; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- 模式判断 / 切换
enum DrvState { DS_FACTORY, DS_PATCHED, DS_MIXED, DS_BROKEN };
// 姿态修正的两个辅助函数（实现放在 #include "pose.h" 之后，避免依赖顺序）
static void ApplyPoseFix(std::wstring& r);
static void RestorePoseFactory(std::wstring& r);
static std::wstring ModeName4(int m);
static std::wstring DrvPath()   { return DriverDir() + L"\\" + kProxyName; }
static std::wstring RealPath()  { return DriverDir() + L"\\" + kRealName; }

static DrvState GetDrvState(std::wstring* detail = nullptr) {
    bool hasMain = FileExists(DrvPath()), hasReal = FileExists(RealPath());
    std::wstring mainSha = hasMain ? Sha(DrvPath()) : L"";
    bool mainIsOrig = (mainSha == kOriginalSha);
    if (detail) *detail = Fmt(L"main=%s(%s) real=%s", hasMain ? L"有" : L"无", mainSha.c_str(), hasReal ? L"有" : L"无");
    if (hasMain && mainIsOrig && !hasReal) return DS_FACTORY;      // 纯原厂
    if (hasMain && !mainIsOrig && hasReal) return DS_PATCHED;      // 代理 + 原厂备份
    if (!hasMain && hasReal)               return DS_MIXED;        // 缺主体（不完整）
    if (!hasMain && !hasReal)              return DS_BROKEN;       // 都没有
    return DS_MIXED;
}
static std::wstring DrvStateName(DrvState s) {
    switch (s) {
    case DS_FACTORY: return L"原厂模式（未打补丁）";
    case DS_PATCHED: return L"补丁模式（震动翻译生效）";
    case DS_MIXED:   return L"文件不完整（建议切回原厂后重打）";
    default:         return L"驱动文件缺失！";
    }
}

static bool SwitchToPatch(std::wstring& msg) {
    std::wstring r;
    { std::wstring kr; if (!EnsureSteamVrClosed(msg, kr)) return false; if (!kr.empty()) r += L"已关闭 SteamVR\n" + kr; }
    SteamRestarter sr;
    if (!EnsureDriverUnlocked(msg, sr.on)) return false;
    std::wstring src = PathProxySrc();
    if (!FileExists(src)) { msg = L"缺少补丁文件：\n" + src; return false; }
    std::wstring main = DrvPath(), real = RealPath();
    // 1) 确保原厂 DLL 已备份（且哈希正确）
    if (!FileExists(real)) {
        if (!FileExists(main)) { msg = L"找不到驱动 DLL：\n" + main; return false; }
        bool ok = false; std::wstring h = Sha(main, &ok);
        if (!ok || h != kOriginalSha) {
            msg = L"当前 DLL 不是原厂文件，拒绝操作（哈希不一致）：\n" + h +
                  L"\n请先用原厂安装包装回驱动。";
            return false;
        }
        if (!Rename(main, real)) { msg = SteamHoldsDriver() ? L"驱动 DLL 正被 Steam 占用：请完全退出 Steam（托盘右键→退出）后再切换。" : L"备份原厂 DLL 失败（可能被占用，先关 SteamVR）"; return false; }
        r += L"  已备份原厂 DLL → driver_huaweiVR_real.dll\n";
    }
    // 2) 放入代理
    if (!CopyTo(src, main)) { msg = SteamHoldsDriver() ? L"驱动 DLL 正被 Steam 占用：请完全退出 Steam（托盘右键→退出）后再切换。" : L"写入代理 DLL 失败（文件被占用？先关 SteamVR）"; return false; }
    r += L"  已安装代理 DLL\n";
    // 3) 配置（默认：震动=翻译模式；房间设置修复与手柄姿态修正都由 compat_fixes + 渲染模型完成）
    if (FileExists(PathIniSrc())) {
        CopyTo(PathIniSrc(), PathIni());
        std::wstring e2;
        if (ReadIni().mode == 0) WriteIni(2, e2);       // 打补丁默认开震动翻译
        r += L"  已写入 haptic_patch.ini（震动=翻译模式）\n";
    }
    ApplyPoseFix(r);        // 手柄姿态修正（默认启用）
    // 3.2) 打补丁 = 禁用原版助手（改名 + 占住互斥体），卸载时自动恢复
    {
        std::wstring m; SetAssistantDisabled(true, m); HoldAssistantMutex(true);
        r += L"  " + m + L"\n";
    }
    // 3.3) 驱动/助手完全走本客户端自己的目录；其它同名驱动登记清掉
    {
        std::wstring off = OfficialDir();
        SaveOfficialDir(off);
        RegWriteStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"Path", BaseDir());
        RegWriteStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"DirectMode", L"true");
        std::wstring rm; RegisterDriver(false, rm);
        int n = RemoveForeignDriverRegs(BaseDir());
        if (n) r += Fmt(L"  已清理 %d 条非本目录的 huaweiVR 驱动登记\n", n);
        r += L"  驱动登记已指向本客户端目录\n";
    }
    // 4) 校验
    bool ok1 = false, ok2 = false;
    std::wstring hr = Sha(real, &ok1), hm = Sha(main, &ok2);
    if (!ok1 || hr != kOriginalSha) { msg = L"校验失败：备份的原厂 DLL 哈希不对！\n" + r; return false; }
    r += L"  校验通过：原厂备份哈希与厂商清单一致\n";
    msg = L"已切到【补丁模式】：\n" + r + L"\n重启 SteamVR 生效。";
    return true;
}

static bool SwitchToFactory(std::wstring& msg) {
    std::wstring r;
    { std::wstring kr; if (!EnsureSteamVrClosed(msg, kr)) return false; if (!kr.empty()) r += L"已关闭 SteamVR\n" + kr; }
    SteamRestarter sr;
    if (!EnsureDriverUnlocked(msg, sr.on)) return false;
    std::wstring main = DrvPath(), real = RealPath();
    if (!FileExists(real)) {
        if (FileExists(main) && Sha(main) == kOriginalSha) { msg = L"当前已是原厂模式，无需切换"; return true; }
        msg = L"找不到原厂备份 driver_huaweiVR_real.dll，无法还原。\n请用原厂安装包重装驱动。";
        return false;
    }
    // 校验备份确实是原厂
    bool ok = false; std::wstring hr = Sha(real, &ok);
    if (!ok || hr != kOriginalSha) { msg = L"备份文件哈希与厂商清单不一致，拒绝还原：\n" + hr; return false; }
    // 删代理 → 还原原名
    if (FileExists(main)) {
        std::wstring hm = Sha(main);
        if (hm == kOriginalSha) { msg = L"当前已是原厂模式，无需切换"; return true; }
        if (!DelFile(main)) { msg = SteamHoldsDriver() ? L"驱动 DLL 正被 Steam 占用：请完全退出 Steam（托盘右键→退出）后再切换。" : L"删除代理 DLL 失败（被占用？先关 SteamVR）"; return false; }
        r += L"  已删除代理 DLL\n";
    }
    if (!Rename(real, main)) { msg = SteamHoldsDriver() ? L"驱动 DLL 正被 Steam 占用：请完全退出 Steam（托盘右键→退出）后再切换。" : L"还原原厂 DLL 失败"; return false; }
    r += L"  已还原 driver_huaweiVR.dll（原厂）\n";
    DelFile(PathIni());
    r += L"  已移除 haptic_patch.ini\n";
    RestorePoseFactory(r);  // 还原手柄渲染模型（若包内备有工厂原件）
    { std::wstring m; SetAssistantDisabled(false, m); HoldAssistantMutex(false); r += L"  " + m + L"\n"; }
    // 4) 驱动/助手切回官方目录；没有官方安装就用本目录里的原厂文件
    {
        std::wstring off = OfficialDir();
        bool offDrv = !off.empty() && FileExists(off + L"\\steamVRDriver\\huaweiVR\\bin\\win64\\driver_huaweiVR.dll");
        RegWriteStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"Path", offDrv ? off : BaseDir());
        if (offDrv) {
            std::wstring rr;
            VrPathRegRun(SteamVRDir(), off + L"\\steamVRDriver\\huaweiVR", L"adddriver", rr);
            RemoveForeignDriverRegs(off);
            r += L"  驱动登记已切回官方目录（本客户端目录已注销）\n";
        } else {
            std::wstring rm; RegisterDriver(false, rm);
            r += L"  未检测到官方安装，继续使用本目录里的原厂驱动\n";
        }
    }
    if (Sha(main) != kOriginalSha) { msg = L"还原后哈希校验不一致，请重装驱动！\n" + r; return false; }
    r += L"  校验通过：哈希与厂商清单一致\n";
    msg = L"已切回【原厂模式】（原版华为助手可正常使用）：\n" + r;
    return true;
}

// ---------------------------------------------------------------- 状态汇总
static int DriverDirectModeFromLog() {
    std::wstring p = SteamVRDir() + L"\\..\\..\\..\\logs\\vrserver.txt";
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return -1;
    std::string s; char b[8192]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    const std::string tag = "huaweiVR: DirectMode :";
    size_t pos = s.rfind(tag);
    if (pos == std::string::npos) return -1;
    size_t i = pos + tag.size();
    while (i < s.size() && (s[i] < '0' || s[i] > '9')) i++;
    if (i >= s.size()) return -1;
    return s[i] - '0';
}
static std::wstring ModeName4(int m);
static std::wstring HintText() {
    if (GetDrvState() == DS_PATCHED)
        return L"当前为补丁模式：安装时已对原厂助手 EXE 做了重命名。要进入原厂模式，请在「高级设置」里切换。";
    return L"当前为原厂驱动：请关闭 lite，用原厂助手启动。要进入补丁模式，请在「高级设置」里切换。";
}
static std::wstring StatusText() {
    bool svr = SteamVrRunning();
    std::wstring svrPid;
    for (DWORD pid : FindProcs(L"vrserver.exe")) svrPid += Fmt(L"%lu ", pid);
    bool hmd = HasHmdDisplay();
    bool dongle = Has6DofDevice();
    std::wstring s;
    s += Fmt(L"VR 头显              : %s\r\n", hmd ? L"已连接（显示器模式）" : (dongle ? L"已连接（VR 直连）" : L"未连接"));
    s += Fmt(L"6DOF 插件            : %s\r\n", dongle ? L"在线 ✓" : L"未检测到 ✗");
    s += Fmt(L"左手柄               : %s\r\n", dongle ? L"在线 ｜ 电量 —" : L"未连接");
    s += Fmt(L"右手柄               : %s\r\n", dongle ? L"在线 ｜ 电量 —" : L"未连接");
    s += Fmt(L"SteamVR              : %s\r\n", svr ? (L"运行中 ✓" + svrPid).c_str() : L"未运行");
    return s;
}
static std::wstring ModeName4(int m) {
    switch (m) { case 0: return L"透传（等于没补丁）"; case 1: return L"只记录"; case 2: return L"翻译（推荐）"; default: return L"完全不震"; }
}

// ---------------------------------------------------------------- 输出（CLI）
static FILE* g_out = NULL;
static std::string ToUtf8(const std::wstring& s) {
    std::string u; u.reserve(s.size() * 3);
    for (wchar_t wc : s) {           // 转 UTF-8
        unsigned c = (unsigned)wc;
        if (c < 0x80) u += (char)c;
        else if (c < 0x800) { u += (char)(0xC0 | (c >> 6)); u += (char)(0x80 | (c & 0x3F)); }
        else { u += (char)(0xE0 | (c >> 12)); u += (char)(0x80 | ((c >> 6) & 0x3F)); u += (char)(0x80 | (c & 0x3F)); }
    }
    return u;
}
static void Out(const std::wstring& s) {
    if (g_out) { fputs(ToUtf8(s).c_str(), g_out); fflush(g_out); }
}
static void WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    std::string u = ToUtf8(text);
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------- GUI
#define IDC_STATUS   1001
#define IDC_MSG      1002
#define IDC_REFRESH  1010
#define IDC_START    1011
#define IDC_STOP     1012
#define IDC_TOPATCH  1013
#define IDC_TOFACT   1014
#define IDC_OPENLOG  1015
#define IDC_OPENDIR  1016
#define IDC_M0       1020
#define IDC_M1       1021
#define IDC_M2       1022
#define IDC_M3       1023
#define IDC_DUR      1030
#define IDC_GAP      1031
#define IDC_SAVE     1032
#define IDC_BLOCK    1033
#define IDC_CHANGELOG 1034
#define IDC_VRGUARD  1034
#define IDC_ADVANCED 1035
#define IDC_ADV_SWITCH 1036
#define IDC_ADV_POSE   1037
#define IDC_ADV_LOG    1038
#define IDC_ADV_DIR    1039
#define IDC_MAIN_SWITCH 1040
#define IDC_THEME_AUTO 1041
#define IDC_THEME_LIGHT 1042
#define IDC_THEME_DARK 1043
#define IDC_ADV_VRMODE 1044
#define IDC_TIMER    1

static HWND g_hStatus = NULL, g_hMsg = NULL, g_hDur = NULL, g_hGap = NULL, g_hHint = NULL;
static HWND g_hStatusBox = NULL, g_hHapticBox = NULL;
static HWND g_btnRefresh = NULL, g_btnStart = NULL, g_btnStop = NULL, g_btnAdv = NULL, g_btnSave = NULL, g_btnChangeLog = NULL;
static HWND g_mainSwitch = NULL;
static HWND g_hRadio[3] = {0};      // 0=透传 1=翻译 2=静音（对应 kModeVal）
static int  g_themeCfg = 0;         // 0=自动 1=浅色 2=深色
static bool g_dark = false;
static HBRUSH g_brCard = NULL, g_brBg = NULL;

// ---------------- 主题（Swiss minimal / #2563EB）----------------
static COLORREF ThBg()    { return g_dark ? RGB(15, 23, 42)    : RGB(248, 250, 252); }
static COLORREF ThCard()  { return g_dark ? RGB(30, 41, 59)    : RGB(255, 255, 255); }
static COLORREF ThText()  { return g_dark ? RGB(226, 232, 240) : RGB(30, 41, 59); }
static COLORREF ThSub()   { return g_dark ? RGB(148, 163, 184) : RGB(71, 85, 105); }
static COLORREF ThAccent(){ return g_dark ? RGB(59, 130, 246)  : RGB(37, 99, 235); }
static COLORREF ThBtn()   { return g_dark ? RGB(39, 52, 73)    : RGB(255, 255, 255); }
static COLORREF ThBorder(){ return g_dark ? RGB(51, 65, 85)    : RGB(226, 232, 240); }
static bool SystemUsesDark() {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD v = 1, sz = sizeof(v); bool dark = false;
    if (RegQueryValueExW(k, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&v, &sz) == ERROR_SUCCESS) dark = (v == 0);
    RegCloseKey(k);
    return dark;
}
static void LoadTheme() {
    g_themeCfg = _wtoi(ReadRegStr(HKEY_CURRENT_USER, L"Software\\HwVRGlassLite", L"Theme", L"0").c_str());
    if (g_themeCfg < 0 || g_themeCfg > 2) g_themeCfg = 0;
    g_dark = (g_themeCfg == 2) || (g_themeCfg == 0 && SystemUsesDark());
}
static void SaveTheme(int v) {
    RegWriteStr(HKEY_CURRENT_USER, L"Software\\HwVRGlassLite", L"Theme", v == 0 ? L"0" : (v == 1 ? L"1" : L"2"));
}
static void RebuildBrushes() {
    if (g_brCard) DeleteObject(g_brCard);
    if (g_brBg)   DeleteObject(g_brBg);
    g_brCard = CreateSolidBrush(ThCard());
    g_brBg   = CreateSolidBrush(ThBg());
}
static BOOL CALLBACK InvalidateAllProc(HWND h, LPARAM) { InvalidateRect(h, NULL, TRUE); return TRUE; }
static void RepaintAll() { EnumThreadWindows(GetCurrentThreadId(), InvalidateAllProc, 0); }
static LRESULT DrawFlatButton(const DRAWITEMSTRUCT* d) {
    wchar_t txt[256] = { 0 };
    GetWindowTextW(d->hwndItem, txt, 256);
    bool pressed  = (d->itemState & ODS_SELECTED) != 0;
    bool disabled = (d->itemState & ODS_DISABLED) != 0;
    bool primary  = (d->CtlID == IDC_START || d->CtlID == IDC_MAIN_SWITCH || d->CtlID == IDC_ADV_SWITCH || d->CtlID == IDC_ADV_VRMODE);
    COLORREF bg = primary ? (pressed ? RGB(29, 78, 216) : ThAccent()) : ThBtn();
    COLORREF fg = primary ? RGB(255, 255, 255) : ThText();
    if (disabled) fg = ThSub();
    HDC dc = d->hDC;
    SetBkMode(dc, TRANSPARENT);
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? bg : ThBorder());
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, 10, 10);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
    SetTextColor(dc, fg);
    RECT r = d->rcItem;
    DrawTextW(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    return TRUE;
}
static LRESULT OnThemeMsg(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)w, &r, g_brBg ? g_brBg : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        wchar_t cls[32] = {0};
        GetClassNameW((HWND)l, cls, 32);
        bool isEdit = (_wcsicmp(cls, L"Edit") == 0);
        SetTextColor(dc, ThText());
        SetBkColor(dc, isEdit ? ThCard() : ThBg());
        SetBkMode(dc, isEdit ? OPAQUE : TRANSPARENT);
        return (LRESULT)(isEdit ? (g_brCard ? g_brCard : (HBRUSH)(COLOR_WINDOW + 1))
                                : (g_brBg ? g_brBg : (HBRUSH)(COLOR_WINDOW + 1)));
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)w;
        SetTextColor(dc, ThText());
        SetBkColor(dc, ThBg());
        return (LRESULT)(g_brBg ? g_brBg : (HBRUSH)(COLOR_WINDOW + 1));
    }
    case WM_DRAWITEM: return DrawFlatButton((const DRAWITEMSTRUCT*)l);
    }
    return -1;
}
static void CenterWindow(HWND h) {
    RECT r; GetWindowRect(h, &r);
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = r.right - r.left, ht = r.bottom - r.top;
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - ht) / 2;
    SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void LayoutChildren(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    const int pad = 16, gap = 12, btnH = 34, hapticsH = 136, msgH = 44, hintH = 40;
    int W = rc.right, H = rc.bottom;
    int msgY = H - pad - msgH;
    int hintY = msgY - gap - hintH;
    int bottomY = hintY - gap - btnH;
    int hapticsY = bottomY - gap - hapticsH;
    int actionsY = hapticsY - gap - btnH;
    int statusH = actionsY - gap - pad;
    if (statusH < 180) statusH = 180;
    if (g_hStatusBox) SetWindowPos(g_hStatusBox, NULL, pad, pad, W - pad * 2, statusH, SWP_NOZORDER);
    if (g_hStatus)    SetWindowPos(g_hStatus, NULL, pad + 12, pad + 28, W - pad * 2 - 24, statusH - 40, SWP_NOZORDER);
    int ax = pad;
    if (g_btnStart)   { SetWindowPos(g_btnStart, NULL, ax, actionsY, 120, btnH, SWP_NOZORDER); ax += 128; }
    if (g_btnStop)    { SetWindowPos(g_btnStop, NULL, ax, actionsY, 104, btnH, SWP_NOZORDER); }
    if (g_hHapticBox) SetWindowPos(g_hHapticBox, NULL, pad, hapticsY, W - pad * 2, hapticsH, SWP_NOZORDER);
    int colW = (W - pad * 2 - 36) / 2;
    for (int i = 0; i < 3; i++) if (g_hRadio[i])
        SetWindowPos(g_hRadio[i], NULL, pad + 12 + (i % 2) * colW, hapticsY + 28 + (i / 2) * 30, colW - 8, 24, SWP_NOZORDER);
    if (g_btnSave) SetWindowPos(g_btnSave, NULL, pad + 12, hapticsY + hapticsH - 46, 130, 32, SWP_NOZORDER);
    if (g_btnChangeLog) SetWindowPos(g_btnChangeLog, NULL, pad, bottomY, 120, btnH, SWP_NOZORDER);
    if (g_btnAdv) SetWindowPos(g_btnAdv, NULL, pad + 128, bottomY, 130, btnH, SWP_NOZORDER);
    if (g_mainSwitch) SetWindowPos(g_mainSwitch, NULL, W - pad - 250, bottomY, 250, btnH, SWP_NOZORDER);
    if (g_hMsg) SetWindowPos(g_hMsg, NULL, pad, msgY, W - pad * 2, msgH, SWP_NOZORDER);
    if (g_hHint) SetWindowPos(g_hHint, NULL, pad, hintY, W - pad * 2, hintH, SWP_NOZORDER);
    InvalidateRect(h, NULL, TRUE);
}

// ---------------- 显示模式（官方 hwvr_DisplayManager 协议）----------------
static const GUID kVirtualDisplayIface =
    { 0xB85B7C50, 0x6A01, 0x11D2, { 0xB8, 0x41, 0xFF, 0x22, 0x10, 0x91, 0x51, 0x71 } };
static bool UnplugVirtualDisplays() {
    HDEVINFO di = SetupDiGetClassDevsW(&kVirtualDisplayIface, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    SP_DEVICE_INTERFACE_DATA ifd; ZeroMemory(&ifd, sizeof(ifd)); ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &kVirtualDisplayIface, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, NULL, NULL)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD in[4] = { 0x10, 0, 0, 0 }, ret = 0;
        if (DeviceIoControl(h, 0x2A4004, in, sizeof(in), NULL, 0, &ret, NULL)) ok = true;
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(di);
    return ok;
}
static bool EnsureDirectModeEntry(std::wstring& note) {
    std::wstring p = SteamVRDir() + L"\\..\\..\\..\\config\\steamvr.vrsettings";
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) { note = L"找不到 steamvr.vrsettings"; return false; }
    std::string text; char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    size_t k = text.find("\"direct_mode\"");
    size_t b = (k == std::string::npos) ? std::string::npos : text.find('{', k);
    size_t e = (b == std::string::npos) ? std::string::npos : text.find('}', b);
    std::vector<std::pair<int, int> > pairs;
    bool found = false;
    if (b != std::string::npos && e != std::string::npos) {
        std::string block = text.substr(b, e - b + 1);
        int count = 0;
        size_t c = block.find("\"count\"");
        if (c != std::string::npos) count = atoi(block.c_str() + block.find(':', c) + 1);
        for (int i = 0; i < count; i++) {
            char kv[32], kp[32];
            snprintf(kv, sizeof(kv), "\"edidVid%d\"", i);
            snprintf(kp, sizeof(kp), "\"edidPid%d\"", i);
            size_t iv = block.find(kv), ip = block.find(kp);
            if (iv == std::string::npos || ip == std::string::npos) continue;
            int vid = atoi(block.c_str() + block.find(':', iv) + 1);
            int pid = atoi(block.c_str() + block.find(':', ip) + 1);
            pairs.push_back(std::make_pair(vid, pid));
            if (vid == 63010 && pid == 2) found = true;
        }
    }
    if (found) { note = L"SteamVR 配置里已包含本机头显直连项"; return false; }
    pairs.push_back(std::make_pair(63010, 2));
    std::string nb = "\"direct_mode\" : {\n      \"count\" : " + std::to_string((int)pairs.size());
    for (size_t i = 0; i < pairs.size(); i++) {
        nb += ",\n      \"edidPid" + std::to_string(i) + "\" : " + std::to_string(pairs[i].second);
        nb += ",\n      \"edidVid" + std::to_string(i) + "\" : " + std::to_string(pairs[i].first);
    }
    nb += "\n   }";
    if (k != std::string::npos && b != std::string::npos && e != std::string::npos) {
        text.replace(k, e - k + 1, nb);
    } else {
        size_t first = text.find('{');
        if (first == std::string::npos) { note = L"配置文件格式不认识"; return false; }
        text.insert(first + 1, "\n   " + nb + ",");
    }
    FILE* o = _wfopen(p.c_str(), L"wb");
    if (!o) { note = L"写入 steamvr.vrsettings 失败"; return false; }
    fwrite(text.data(), 1, text.size(), o);
    fclose(o);
    note = L"已在 SteamVR 配置里启用本机头显直连（direct_mode）";
    return true;
}
static void FixVrDisplayMode(std::wstring& msg) {
    std::wstring r;
    if (SteamVrRunning()) { std::wstring kr; KillSteamVr(kr); r += L"已关闭 SteamVR\n"; Sleep(1500); }
    std::wstring note; EnsureDirectModeEntry(note);
    r += note + L"\n";
    RegWriteStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"DirectMode", L"true");
    RegWriteStr(HKEY_CURRENT_USER, L"SOFTWARE\\HuaweiPCVRAssistant", L"Path", BaseDir());
    RemoveForeignDriverRegs(BaseDir());
    r += UnplugVirtualDisplays() ? L"已发送“断开虚拟显示器”命令\n"
                                 : L"没找到虚拟显示设备（可能没装官方投屏驱动）\n";
    std::wstring m; StartSteamVr(m);
    r += m + L"\n";
    msg = r;
}

static void SetMsg(const std::wstring& s) { if (g_hMsg) SetWindowTextW(g_hMsg, s.c_str()); }
static void RefreshMainSwitch() {
    if (!g_mainSwitch) return;
    ShowWindow(g_mainSwitch, (GetDrvState() == DS_PATCHED) ? SW_HIDE : SW_SHOW);
}
static void RefreshStatus() {
    if (g_hStatus) {
        std::wstring t = StatusText();
        wchar_t cur[8192] = {0};
        GetWindowTextW(g_hStatus, cur, 8191);
        if (t != cur) {
            int first = (int)SendMessageW(g_hStatus, EM_GETFIRSTVISIBLELINE, 0, 0);
            SetWindowTextW(g_hStatus, t.c_str());
            int after = (int)SendMessageW(g_hStatus, EM_GETFIRSTVISIBLELINE, 0, 0);
            if (after != first) SendMessageW(g_hStatus, EM_LINESCROLL, 0, first - after);
        }
    }
    HapticCfg c = ReadIni();
    for (int i = 0; i < 3; i++) if (g_hRadio[i])
        SendMessageW(g_hRadio[i], BM_SETCHECK, (c.mode == kModeVal[i]) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hHint) SetWindowTextW(g_hHint, HintText().c_str());
    RefreshMainSwitch();
}
static HWND Mk(HWND p, const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, int id, HFONT f) {
    DWORD s = st;
    if (cls && wcscmp(cls, L"BUTTON") == 0 && (st & BS_TYPEMASK) == BS_PUSHBUTTON) s |= BS_OWNERDRAW;
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | s, x, y, w, h, p, (HMENU)(INT_PTR)id, NULL, NULL);
    if (f && cls != L"EDIT") SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}
static void SaveHaptic(HWND h) {
    int mode = ReadIni().mode;
    for (int i = 0; i < 3; i++) if (SendMessageW(g_hRadio[i], BM_GETCHECK, 0, 0) == BST_CHECKED) mode = kModeVal[i];
    std::wstring err;
    if (WriteIni(mode, err)) SetMsg(Fmt(L"已保存：模式=%s  —— 重启 SteamVR 生效", ModeName4(mode).c_str()));
    else SetMsg(err);
    RefreshStatus();
}


#include "pose.h"        // 手柄姿态微调（旋转, 二级窗口）：必须放在 ExeDir/Fmt/DriverDir/SetMsg/KillSteamVr 之后

// ================= 版本号 + 更新日志（二级页面） =================
static const wchar_t* kClientVer = L"1.4.5";
static const wchar_t* kChangelog =
    L"HwVRGlassLite 更新日志\r\n"
    L"\r\n"
    L"1.4.5  优化与官方助手的共存；优化客户端 UI\r\n"
    L"1.4.1  适配 SteamVR 亮度控制\r\n"
    L"1.4.0  增加 SteamVR 原生透视功能\r\n"
    L"1.3.0  修复手柄震动 BUG\r\n"
    L"1.2.0  修复 SteamVR 关掉后自动重启的 BUG，修复手柄偏移\r\n"
    L"1.1.0  修复每次都拉取房间设置的 bug\r\n"
    L"1.0    重构客户端\r\n";

static BOOL CALLBACK CloseSecondaryProc(HWND w, LPARAM) {
    wchar_t cls[64] = {0};
    GetClassNameW(w, cls, 64);
    if (_wcsicmp(cls, L"HwVRGlassLiteAdv") == 0 ||
        _wcsicmp(cls, L"HwVRGlassLiteLog") == 0 ||
        _wcsicmp(cls, L"HwVRGlassLitePoseWnd") == 0) DestroyWindow(w);
    return TRUE;
}
static void CloseSecondaryWindows() { EnumThreadWindows(GetCurrentThreadId(), CloseSecondaryProc, 0); }
static LRESULT CALLBACK LogWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT tr = OnThemeMsg(h, m, w, l);
    if (tr != -1) return tr;
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (m == WM_COMMAND && LOWORD(w) == IDOK) { DestroyWindow(h); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void OpenChangelogWindow(HWND owner) {
    CloseSecondaryWindows();
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = LogWndProc; wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = g_brBg;
        wc.lpszClassName = L"HwVRGlassLiteLog";
        RegisterClassW(&wc); reg = true;
    }
    const int W = 660, H = 560;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"HwVRGlassLiteLog",
                             Fmt(L"更新日志 — HwVRGlassLite %s", kClientVer).c_str(),
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner, NULL, GetModuleHandleW(NULL), NULL);
    HFONT f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", kChangelog,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                             12, 12, W - 36, H - 92, h, NULL, NULL, NULL);
    SendMessageW(e, WM_SETFONT, (WPARAM)f, TRUE);
    HWND b = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                             W - 120, H - 68, 92, 28, h, (HMENU)IDOK, NULL, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)f, TRUE);
    ShowWindow(h, SW_SHOW);
    CenterWindow(h);
}

// ================= 高级设置（二级窗口） =================
static HWND g_advSwitch = NULL, g_advMsg = NULL;
static HWND g_themeRadio[3] = {0};
static void ThemeRefresh() {
    for (int i = 0; i < 3; i++) if (g_themeRadio[i])
        SendMessageW(g_themeRadio[i], BM_SETCHECK, (g_themeCfg == i) ? BST_CHECKED : BST_UNCHECKED, 0);
}
static void AdvRefresh(HWND h) {
    DrvState s = GetDrvState();
    bool patched = (s == DS_PATCHED);
    if (g_advSwitch) {
        ShowWindow(g_advSwitch, SW_SHOW);
        SetWindowTextW(g_advSwitch, patched ? L"切换到原厂模式" : L"切换到补丁模式");
    }
    if (g_advMsg) SetWindowTextW(g_advMsg, patched
        ? L"当前状态：补丁模式（震动翻译已启用）\r\n\r\n切回原厂：点上面的「切换到原厂模式」"
        : L"当前状态：原厂驱动（未启用震动修复）\r\n\r\n切到补丁：点上面的「切换到补丁模式」");
}
static LRESULT CALLBACK AdvWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT tr = OnThemeMsg(h, m, w, l);
    if (tr != -1) return tr;
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_THEME_AUTO:
        case IDC_THEME_LIGHT:
        case IDC_THEME_DARK: {
            int v = (LOWORD(w) == IDC_THEME_AUTO) ? 0 : (LOWORD(w) == IDC_THEME_LIGHT ? 1 : 2);
            SaveTheme(v); LoadTheme(); RebuildBrushes(); ThemeRefresh(); RepaintAll();
            return 0;
        }
        case IDC_ADV_SWITCH: {
            std::wstring msg;
            if (GetDrvState() == DS_PATCHED) SwitchToFactory(msg); else SwitchToPatch(msg);
            if (g_advMsg) SetWindowTextW(g_advMsg, msg.c_str());
            AdvRefresh(h);
            return 0;
        }
        case IDC_ADV_VRMODE: {
            std::wstring msg2; FixVrDisplayMode(msg2);
            if (g_advMsg) SetWindowTextW(g_advMsg, msg2.c_str());
            AdvRefresh(h);
            return 0;
        }
        case IDC_ADV_POSE: DestroyWindow(h); CloseSecondaryWindows(); OpenPoseWindow(NULL); return 0;
        case IDC_ADV_LOG:  ShellExecuteW(NULL, L"open", L"notepad.exe", PathLog().c_str(), NULL, SW_SHOWNORMAL); return 0;
        case IDC_ADV_DIR:  ShellExecuteW(NULL, L"open", DriverDir().c_str(), NULL, NULL, SW_SHOWNORMAL); return 0;
        case IDOK: DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void OpenAdvancedWindow(HWND owner) {
    CloseSecondaryWindows();
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = AdvWndProc; wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = g_brBg;
        wc.lpszClassName = L"HwVRGlassLiteAdv";
        RegisterClassW(&wc); reg = true;
    }
    const int W = 600, H = 460;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"HwVRGlassLiteAdv", L"高级设置",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner, NULL, GetModuleHandleW(NULL), NULL);
    HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    struct { const wchar_t* t; int id; int x; int y; int w; } btns[] = {
        { L"",               IDC_ADV_SWITCH, 20, 20,  330 },
        { L"手柄姿态微调（旋转）…", IDC_ADV_POSE,   20, 62,  200 },
        { L"打开日志",        IDC_ADV_LOG,    232, 62,  120 },
        { L"打开驱动目录",     IDC_ADV_DIR,    364, 62,  150 },
        { L"切回 VR 模式（修复显示屏模式）", IDC_ADV_VRMODE, 20, 150, 330 },
    };
    for (int i = 0; i < 5; i++) {
        HWND b = CreateWindowExW(0, L"BUTTON", btns[i].t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                                 btns[i].x, btns[i].y, btns[i].w, 30, h, (HMENU)(INT_PTR)btns[i].id, NULL, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)f, TRUE);
        if (btns[i].id == IDC_ADV_SWITCH) g_advSwitch = b;
    }
    const wchar_t* tn[3] = { L"自动（跟随系统）", L"浅色", L"深色" };
    for (int i = 0; i < 3; i++) {
        HWND b = CreateWindowExW(0, L"BUTTON", tn[i], WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0),
                                 20 + i * 150, 110, 140, 24, h, (HMENU)(INT_PTR)(IDC_THEME_AUTO + i), NULL, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)f, TRUE);
        g_themeRadio[i] = b;
    }
    g_advMsg = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               20, 190, W - 56, 170, h, NULL, NULL, NULL);
    SendMessageW(g_advMsg, WM_SETFONT, (WPARAM)f, TRUE);
    HWND c = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                             W - 120, H - 62, 92, 28, h, (HMENU)IDOK, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    AdvRefresh(h);
    ThemeRefresh();
    ShowWindow(h, SW_SHOW);
    CenterWindow(h);
}

// 打补丁默认启用：按 pose_baseline + pose.ini 重新生成两只手的渲染模型（即"手柄姿态修正"）
static void ApplyPoseFix(std::wstring& r) {
    std::wstring e1, e2;
    bool okL = WriteHandJson(true,  ReadPoseOff(true),  e1);
    bool okR = WriteHandJson(false, ReadPoseOff(false), e2);
    r += (okL && okR) ? L"  已应用手柄姿态修正（左右手）\n"
                      : (L"  手柄姿态修正失败：" + (okL ? e2 : e1) + L"\n");
}
// 切回原厂：把渲染模型还原成工厂原件（如果包内备了 pose_factory）
static void RestorePoseFactory(std::wstring& r) {
    std::wstring a = ExeDir() + L"\\pose_factory\\huawei_controller_left.json";
    std::wstring b = ExeDir() + L"\\pose_factory\\huawei_controller_right.json";
    if (FileExists(a) && FileExists(b) && CopyTo(a, PoseJsonPath(true)) && CopyTo(b, PoseJsonPath(false)))
        r += L"  已还原创手柄渲染模型（工厂原件）\n";
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT tr = OnThemeMsg(h, m, w, l);
    if (tr != -1) return tr;
    switch (m) {
    case WM_CREATE: {
        HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_hStatusBox = Mk(h, L"BUTTON", L"设备与运行状态", BS_GROUPBOX, 0, 0, 10, 10, 0, NULL);
        g_hStatus = Mk(h, L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                       0, 0, 10, 10, IDC_STATUS, f);
        g_btnStart   = Mk(h, L"BUTTON", L"启动 VR", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_START, f);
        g_btnStop    = Mk(h, L"BUTTON", L"关闭 VR", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_STOP, f);
        g_hHapticBox = Mk(h, L"BUTTON", L"手柄震动设置", BS_GROUPBOX, 0, 0, 10, 10, 0, NULL);
        const wchar_t* rn[3] = { L"透传（原厂驱动行为）", L"翻译（震动补丁·推荐）", L"静音（完全不震）" };
        for (int i = 0; i < 3; i++)
            g_hRadio[i] = Mk(h, L"BUTTON", rn[i], BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0),
                             0, 0, 10, 10, IDC_M0 + i, f);
        g_btnSave = Mk(h, L"BUTTON", L"保存震动模式", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_SAVE, f);
        g_mainSwitch = Mk(h, L"BUTTON", L"切换到补丁（启用震动修复）", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_MAIN_SWITCH, f);
        g_btnChangeLog = Mk(h, L"BUTTON", L"更新日志", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_CHANGELOG, f);
        g_btnAdv = Mk(h, L"BUTTON", L"高级设置…", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_ADVANCED, f);
        g_hMsg = Mk(h, L"STATIC", L"就绪", SS_LEFT, 0, 0, 10, 10, IDC_MSG, f);
        g_hHint = Mk(h, L"STATIC", HintText().c_str(), SS_LEFT, 0, 0, 10, 10, 0, f);
        LayoutChildren(h);
        RefreshStatus();
        SetTimer(h, IDC_TIMER, 3000, NULL);
        return 0;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) LayoutChildren(h);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = 680;
        mm->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_TIMER: if (w == IDC_TIMER) RefreshStatus(); return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        std::wstring msg;
        switch (id) {
        case IDC_REFRESH: RefreshStatus(); SetMsg(L"状态已刷新"); return 0;
        case IDC_START:
            if (GetDrvState() != DS_PATCHED) {
                MessageBoxW(h, L"当前不是补丁模式：请用官方助手启动，或到「高级设置」切换到补丁模式后再启动。",
                            L"HwVRGlassLite", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            StartSteamVr(msg); SetMsg(msg); RefreshStatus(); return 0;
        case IDC_STOP: {
            std::wstring r;
            if (!SteamVrRunning()) { SetMsg(L"SteamVR 未在运行"); return 0; }
            if (KillSteamVr(r)) SetMsg(L"已关闭 SteamVR\n" + r); else SetMsg(L"部分进程无法结束（权限不足）：\n" + r);
            Sleep(1200); RefreshStatus(); return 0;
        }
        case IDC_MAIN_SWITCH: SwitchToPatch(msg); SetMsg(msg); RefreshStatus(); return 0;
        case IDC_TOPATCH: SwitchToPatch(msg); SetMsg(msg); RefreshStatus(); return 0;
        case IDC_TOFACT:  SwitchToFactory(msg); SetMsg(msg); RefreshStatus(); return 0;
        case IDC_SAVE:    SaveHaptic(h); return 0;
        case IDC_POSE_BTN: CloseSecondaryWindows(); OpenPoseWindow(h); return 0;
        case IDC_CHANGELOG: OpenChangelogWindow(h); return 0;
        case IDC_ADVANCED:  OpenAdvancedWindow(h); return 0;
        // 原「禁用原版助手」勾选框已移除：改由补丁状态自动控制（见 SwitchToPatch / SwitchToFactory）
        case IDC_OPENLOG: ShellExecuteW(NULL, L"open", L"notepad.exe", PathLog().c_str(), NULL, SW_SHOWNORMAL); return 0;
        case IDC_OPENDIR: ShellExecuteW(NULL, L"open", DriverDir().c_str(), NULL, NULL, SW_SHOWNORMAL); return 0;
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- CLI
// ================= 调光器（--dimmer）：SteamVR 亮度滑条 -> 全屏黑色 overlay 的 alpha =================
// 官方 PC 端在投屏模式里用的就是"把画面乘一个系数"（D3D11 亮度 shader）；这块屏没有可用的背光命令。
// 这里用公开的 overlay API 做等价的事：一个铺满视野的黑色 overlay，alpha = 1 - gain，
// 合成时 out = scene*(1-a) + 0*a —— 纯乘法压暗，效果与官方那个 shader 一致。
static void DimLog(const char* fmt, ...) {
    static FILE* f = NULL;
    if (!f) {
        std::wstring p = ExeDir() + L"\\dimmer.log";
        f = _wfopen(p.c_str(), L"ab");
        if (f) { SYSTEMTIME st; GetLocalTime(&st);
                 fprintf(f, "=== %04d-%02d-%02d %02d:%02d:%02d ===\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond); }
    }
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fflush(f);
}
static int RunDimmer(void) {
    HANDLE mtx = CreateMutexW(NULL, FALSE, L"HwVRGlassLiteDimmer_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) { DimLog("已有实例在跑，退出"); return 0; }
    std::wstring dll = SteamVRDir() + L"\\bin\\win64\\openvr_api.dll";
    if (!FileExists(dll)) dll = SteamVRDir() + L"\\bin\\win32\\openvr_api.dll";
    HMODULE hv = LoadLibraryW(dll.c_str());
    if (!hv) { DimLog("载入 openvr_api.dll 失败: %S", dll.c_str()); return 1; }
    // openvr.h 里的真实声明：uint32 VR_InitInternal2(EVRInitError*, type, startupinfo)  —— 返回的是 token，不是错误码！
    typedef uint32_t (*PFN_INIT)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    typedef void* (*PFN_IFACE)(const char*, vr::EVRInitError*);
    typedef void  (*PFN_SHUT)();
    PFN_INIT  pInit  = (PFN_INIT)GetProcAddress(hv, "VR_InitInternal2");
    PFN_IFACE pIface = (PFN_IFACE)GetProcAddress(hv, "VR_GetGenericInterface");
    PFN_SHUT  pShut  = (PFN_SHUT)GetProcAddress(hv, "VR_ShutdownInternal");
    if (!pInit || !pIface) { DimLog("openvr_api 缺少导出"); return 1; }
    DimLog("init... %S", dll.c_str());
    // VR 可能还没起来：给 30 秒重试
    vr::EVRInitError e = vr::VRInitError_None;
    for (int i = 0; i < 60; i++) {
        e = vr::VRInitError_None;
        pInit(&e, vr::VRApplication_Overlay, NULL);
        if (e == vr::VRInitError_None) break;
        DimLog("init 失败 err=%d（第 %d 次）", (int)e, i + 1);
        Sleep(500);
    }
    if (e != vr::VRInitError_None) { DimLog("放弃：VR 起不来"); return 1; }
    vr::EVRInitError e2 = vr::VRInitError_None;
    vr::IVROverlay*  ov = (vr::IVROverlay*)pIface(vr::IVROverlay_Version, &e2);
    vr::IVRSettings* st = (vr::IVRSettings*)pIface(vr::IVRSettings_Version, &e2);
    vr::IVRSystem*   sy = (vr::IVRSystem*)pIface(vr::IVRSystem_Version, &e2);
    if (!ov || !st) { DimLog("取不到 IVROverlay/IVRSettings"); if (pShut) pShut(); return 1; }
    DimLog("overlay 接口已拿到（亮度能力见驱动日志里的 camprod 行）");
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError oe = ov->CreateOverlay("hwvrglasslite.dimmer", "HwVRGlassLite 亮度", &h);
    if (oe != vr::VROverlayError_None) { DimLog("CreateOverlay 失败 err=%d", (int)oe); if (pShut) pShut(); return 1; }
    unsigned char black[4] = { 0, 0, 0, 255 };                  // BGRA 纯黑
    ov->SetOverlayRaw(h, black, 1, 1, 4);
    ov->SetOverlayWidthInMeters(h, 200.0f);                     // 巨大 -> 铺满整个视野
    ov->SetOverlayInputMethod(h, vr::VROverlayInputMethod_None);
    ov->SetOverlayFlag(h, vr::VROverlayFlags_NoDashboardTab, true);
    ov->SetOverlayFlag(h, vr::VROverlayFlags_VisibleInDashboard, false);
    // ★ 不要用 SortWithNonSceneOverlays：那会把 overlay 排到"非场景"那一摞里去，
    //   可能被游戏画面盖住。默认（普通 overlay）就是画在场景之上。
    ov->SetOverlaySortOrder(h, 0);                              // 排在系统/仪表盘 overlay 之下
    {   // 跟着头显，放在正前方 1 米
        vr::HmdMatrix34_t m; memset(&m, 0, sizeof(m));
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
        m.m[2][3] = -1.0f;
        vr::EVROverlayError te = ov->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &m);
        DimLog("设置头显相对位姿(前方 1m) err=%d", (int)te);
    }
    ov->ShowOverlay(h);
    DimLog("overlay 建好，开始跟随 steamvr/analogGain");
    DimLog("overlay 可见=%d（1=合成器认为它可见）", (int)ov->IsOverlayVisible(h));
    float lastA = -1.0f;
    for (;;) {
        if (sy) {
            vr::VREvent_t ev;
            while (sy->PollNextEvent(&ev, sizeof(ev))) {
                if (ev.eventType == vr::VREvent_Quit || ev.eventType == vr::VREvent_ProcessQuit) {
                    DimLog("收到退出事件，收工");
                    ov->DestroyOverlay(h);
                    if (pShut) pShut();
                    return 0;
                }
            }
        }
        vr::EVRSettingsError se = vr::VRSettingsError_None;
        float g = st->GetFloat("steamvr", "analogGain", &se);
        if (se != vr::VRSettingsError_None) g = 1.0f;           // 没设过 = 不压暗
        if (!(g > 0.0f) || g > 1.0f) g = 1.0f;
        if (g < 0.05f) g = 0.05f;
        float a = 1.0f - g;
        if (a < 0.0f) a = 0.0f;
        if (a > 0.95f) a = 0.95f;                               // 别压成全黑，至少留 5%
        if (a != lastA) {
            ov->SetOverlayAlpha(h, a);
            DimLog("analogGain=%.3f -> 压暗 alpha=%.3f", g, a);
            lastA = a;
        }
        Sleep(300);
    }
}

static int RunCli(int argc, wchar_t** argv) {
    // 尝试附着到父控制台；同时把输出写进文件，便于脚本/终端读取
    AttachConsole(ATTACH_PARENT_PROCESS);
    g_out = freopen("CONOUT$", "w", stdout);

    std::wstring exeLog = ExeDir() + L"\\cli_output.txt";
    FILE* fe = _wfopen(exeLog.c_str(), L"wb");
    std::wstring result;

    std::wstring cmd = argv[1];
    if (cmd == L"--status" || cmd == L"-s") {
        result = StatusText();
    } else if (cmd == L"--start-vr") {
        std::wstring m; StartSteamVr(m); result = m + L"\r\n";
    } else if (cmd == L"--stop-vr") {
        std::wstring r; bool ok = KillSteamVr(r); result = (ok ? L"已关闭 SteamVR\r\n" : L"部分未能关闭\r\n") + r;
    } else if (cmd == L"--to-patch") {
        std::wstring m; SwitchToPatch(m); result = m + L"\r\n";
    } else if (cmd == L"--to-factory") {
        std::wstring m; SwitchToFactory(m); result = m + L"\r\n";
    } else if (cmd == L"--register") {
        std::wstring m; RegisterDriver(false, m); result = m;
    } else if (cmd == L"--unregister") {
        std::wstring m; RegisterDriver(true, m); result = m;
    } else if (cmd == L"--haptic") {
        int mode = ReadIni().mode;
        for (int i = 2; i < argc; i++) {
            std::wstring a = argv[i];
            if (a.rfind(L"mode=", 0) == 0) mode = _wtoi(a.c_str() + 5);
        }
        std::wstring err;
        if (WriteIni(mode, err)) result = Fmt(L"已写入：mode=%d（%s）\r\n", mode, ModeName4(mode).c_str());
        else result = err + L"\r\n";
    } else if (cmd == L"--disable-assistant") {
        bool on = !(argc > 2 && _wcsicmp(argv[2], L"off") == 0);
        std::wstring m; SetAssistantDisabled(on, m);
        result = m + L"\r\n";
    } else if (cmd == L"--daemon") {
        bool block = true;
        for (int i = 2; i < argc; i++) if (_wcsicmp(argv[i], L"--no-block") == 0) block = false;
        if (block) HoldAssistantMutex(true);
        { std::wstring m; SetAssistantDisabled(true, m); }
        if (fe) { fclose(fe); fe = NULL; }
        Out(Fmt(L"[daemon] 常驻中：封堵原版助手=%s；状态每 5 秒写入 %s\r\n", block ? L"是" : L"否", exeLog.c_str()));
        while (true) {
            SYSTEMTIME st; GetLocalTime(&st);
            std::wstring s = Fmt(L"[%02d:%02d:%02d] 封堵助手=%s ｜ 助手进程=%s ｜ SteamVR=%s\r\n\r\n",
                                 st.wHour, st.wMinute, st.wSecond,
                                 g_assistantMutex ? L"是" : L"否",
                                 AssistantRunning() ? L"运行中" : L"未运行",
                                 SteamVrRunning() ? L"运行中" : L"未运行");
            s += StatusText();
            WriteFileUtf8(exeLog, s);
            Sleep(5000);
        }
        return 0;
    } else if (cmd == L"--verify") {
        std::wstring main = DrvPath(), real = RealPath();
        result += Fmt(L"目录: %s\r\n", DriverDir().c_str());
        result += Fmt(L"driver_huaweiVR.dll      : %s\r\n", FileExists(main) ? Sha(main).c_str() : L"(无)");
        result += Fmt(L"driver_huaweiVR_real.dll : %s\r\n", FileExists(real) ? Sha(real).c_str() : L"(无)");
        result += Fmt(L"厂商清单要求的原厂哈希    : %s\r\n", kOriginalSha);
        result += Fmt(L"本地补丁 DLL(应与之相同) : %s\r\n", FileExists(PathProxySrc()) ? Sha(PathProxySrc()).c_str() : L"(缺 patch\\driver_huaweiVR.dll)");
    } else {
        result = L"HwVRGlassLite 命令行用法：\r\n"
                 L"  --status                     查看设备/驱动/SteamVR/模式状态\r\n"
                 L"  --start-vr / --stop-vr       启动 / 关闭 SteamVR\r\n"
                 L"  --to-patch / --to-factory    切换到 补丁模式 / 原厂模式（需先关 SteamVR）\r\n"
                 L"  --register / --unregister    把驱动登记进 SteamVR / 从 SteamVR 注销\r\n"
                 L"  --dimmer                     常驻调光器（跟 SteamVR 亮度滑条，一般由驱动自动拉起）\r\n"
                 L"  --disable-assistant [off]    禁用（改名）/ 恢复 原版华为助手\r\n"
                 L"  --haptic mode=2 dur=40 gap=120 inten=10   写震动参数\r\n"
                 L"  --verify                     显示 DLL 哈希校验信息\r\n"
                 L"  --daemon [--no-block]        常驻后台（禁用助手，状态每 5 秒写 cli_output.txt）\r\n"
                 L"（双击不带参数 = 打开窗口界面）\r\n";
    }
    Out(result);
    if (fe) fclose(fe);
    WriteFileUtf8(exeLog, result);
    return 0;
}

// ---------------------------------------------------------------- 入口
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdline, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool noElev = (argc == 2 && _wcsicmp(argv[1], L"--no-elevate") == 0);
    // --dimmer：常驻调光器（不弹窗、不提权），由驱动在 SteamVR 起来时自动拉起
    if (argc > 1 && _wcsicmp(argv[1], L"--dimmer") == 0) {
        int rc = RunDimmer();
        LocalFree(argv);
        return rc;
    }
    // 双击启动（无参数）时先请求管理员权限：UAC 弹窗点一次“是”，之后关 SteamVR/切驱动才生效
    if (argc == 1 && !IsElevated()) {
        wchar_t self[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, self, MAX_PATH);
        SHELLEXECUTEINFOW sei; ZeroMemory(&sei, sizeof(sei)); sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas"; sei.lpFile = self; sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) { LocalFree(argv); return 0; }   // 提权后的新实例接管
    }
    if (argc > 1 && !noElev) {
        // --register / --unregister 需要管理员（写 SteamVR 登记表 / 系统位置）；没提权就自己提一次
        if ((_wcsicmp(argv[1], L"--register") == 0 || _wcsicmp(argv[1], L"--unregister") == 0) && !IsElevated()) {
            wchar_t self[MAX_PATH] = { 0 };
            GetModuleFileNameW(NULL, self, MAX_PATH);
            SHELLEXECUTEINFOW sei; ZeroMemory(&sei, sizeof(sei)); sei.cbSize = sizeof(sei);
            sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = argv[1]; sei.nShow = SW_HIDE;
            if (ShellExecuteExW(&sei)) { LocalFree(argv); return 0; }
        }
        int rc = RunCli(argc, argv);
        LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);

    // 单实例：已经开着就把已有窗口激活，不再开第二个
    HANDLE instMutex = CreateMutexW(NULL, FALSE, L"HwVRGlassLite_UI_SingleInstance");
    if (instMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND ex = FindWindowW(L"HwVRGlassLiteWnd", NULL);
        if (ex) { ShowWindow(ex, SW_RESTORE); SetForegroundWindow(ex); }
        return 0;
    }

    LoadTheme();
    RebuildBrushes();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"HwVRGlassLiteWnd";
    RegisterClassExW(&wc);
    { INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES }; InitCommonControlsEx(&icc); }   // 滑条控件

    HWND h = CreateWindowExW(0, wc.lpszClassName, kTitle,
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 780, 640, NULL, NULL, hInst, NULL);
    if (!h) return 1;
    CenterWindow(h);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
