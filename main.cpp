// ============================================================================
//  Klyraudio - a Winamp-style audio player for Windows (Win32 + Media Foundation)
//
//  No external libraries needed. Decoding is done by Windows Media Foundation,
//  so every format Windows can decode works (MP3, AAC/M4A, WMA, WAV, FLAC,
//  ALAC, AIFF, AC3 ... and OGG Vorbis / Opus if the free Microsoft
//  "Web Media Extensions" are installed).
//
//  Playlists are saved as .klyr (UTF-8 text) .m3u/.m3u8 can be imported.
// ============================================================================
#define NOMINMAX
#define _USE_MATH_DEFINES
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propsys.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <winhttp.h>
#include "resource.h"
#include "feedparse.h"
#include "podcatalog.h"
#include "resample.h"
#include "keymap.h"
#include "snap.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <cmath>
#include <complex>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// ----------------------------------------------------------------------------
//  Constants / IDs
// ----------------------------------------------------------------------------
#define WM_APP_TRACKEND (WM_APP + 1)
#define WM_APP_TRACKERR (WM_APP + 2)
#define WM_APP_META     (WM_APP + 3)
#define WM_APP_SCAN     (WM_APP + 4)
#define WM_APP_URL      (WM_APP + 5)
#define WM_APP_POD      (WM_APP + 6)
#define WM_APP_PODSEARCH (WM_APP + 7)
#define WM_APP_TRACKSWITCH (WM_APP + 8)   // player moved on to the next track (gapless / crossfade)

enum { IDC_LIST = 100, IDC_SEEK, IDC_VOL };
enum {
    IDM_ADD_FILES = 200, IDM_ADD_FOLDER, IDM_OPEN_PL, IDM_SAVE_PL, IDM_EXIT,
    IDM_PLAY, IDM_PAUSE, IDM_STOP, IDM_PREV, IDM_NEXT, IDM_SHUFFLE, IDM_REPEAT,
    IDM_SEEK_FWD, IDM_SEEK_BACK, IDM_VOL_UP, IDM_VOL_DN,
    IDM_EQ, IDM_ONTOP, IDM_REMOVE, IDM_CLEAR, IDM_SELALL, IDM_ABOUT,
    // playlist context menu / sorting
    IDM_PLAY_SEL, IDM_SHOW_EXPLORER, IDM_COPY_PATH, IDM_COPY_TITLE, IDM_PROPS,
    IDM_SORT_TITLE, IDM_SORT_PATH, IDM_SORT_DUR, IDM_SORT_REV, IDM_SORT_RAND,
    IDM_ADD_URL, IDM_PODCASTS,
    // Playback menu: gapless playback and crossfade lengths
    IDM_GAPLESS, IDM_FADE_0, IDM_FADE_1, IDM_FADE_2, IDM_FADE_3, IDM_FADE_4, IDM_FADE_5, IDM_FADE_6, IDM_KEYS, IDM_PLAYLIST_WIN
};
static const int ID_EQ_ON = 3200, ID_EQ_RESET = 3201;
static const int kFadeSecs[7] = { 0, 1, 2, 3, 5, 8, 12 };   // crossfade lengths offered in the menu (seconds)

// ---- keyboard shortcuts (1.6) ---------------------------------------------------------------------------
// Every command that has a shortcut. `def` is the default window shortcut ("" = none); global hotkeys
// (system-wide, RegisterHotKey) are off by default. Names are what settings.ini uses (key_<name>, gkey_<name>).
struct KeyAction { int cmd; const wchar_t* name; const wchar_t* title; const wchar_t* def; };
static const KeyAction kKeyActions[] = {
    { IDM_ADD_FILES,  L"add_files",  L"Add files",           L"Ctrl+O" },
    { IDM_ADD_FOLDER, L"add_folder", L"Add folder",          L"Ctrl+D" },
    { IDM_ADD_URL,    L"add_url",    L"Add URL",             L"Ctrl+U" },
    { IDM_PODCASTS,   L"podcasts",   L"Podcasts",            L"Ctrl+P" },
    { IDM_OPEN_PL,    L"open_pl",    L"Open playlist",       L"Ctrl+L" },
    { IDM_SAVE_PL,    L"save_pl",    L"Save playlist",       L"Ctrl+S" },
    { IDM_PLAY,       L"play",       L"Play",                L"X" },
    { IDM_PAUSE,      L"pause",      L"Pause / resume",      L"C" },
    { IDM_STOP,       L"stop",       L"Stop",                L"V" },
    { IDM_PREV,       L"prev",       L"Previous",            L"Z" },
    { IDM_NEXT,       L"next",       L"Next",                L"B" },
    { IDM_SEEK_BACK,  L"seek_back",  L"Back 5 seconds",      L"Ctrl+Left" },
    { IDM_SEEK_FWD,   L"seek_fwd",   L"Forward 5 seconds",   L"Ctrl+Right" },
    { IDM_VOL_UP,     L"vol_up",     L"Volume up",           L"Ctrl+Up" },
    { IDM_VOL_DN,     L"vol_down",   L"Volume down",         L"Ctrl+Down" },
    { IDM_SHUFFLE,    L"shuffle",    L"Shuffle",             L"S" },
    { IDM_REPEAT,     L"repeat",     L"Repeat",              L"R" },
    { IDM_EQ,         L"equalizer",  L"Equalizer",           L"Ctrl+E" },
    { IDM_PLAYLIST_WIN, L"playlist", L"Playlist window",     L"Alt+2" },
    { IDM_ONTOP,      L"ontop",      L"Always on top",       L"" },
    { IDM_SELALL,     L"select_all", L"Select all",          L"Ctrl+A" },
    { IDM_REMOVE,     L"remove",     L"Remove selected",     L"Delete" },
    { IDM_PLAY_SEL,   L"play_sel",   L"Play selected",       L"Enter" },
    { IDM_CLEAR,      L"clear",      L"Clear playlist",      L"" },
};
static const int kNumKeyActions = (int)(sizeof kKeyActions / sizeof kKeyActions[0]);

struct KeyBind { std::wstring window, global; };     // shortcut text, "" = none
static std::vector<KeyBind> DefaultKeys() {
    std::vector<KeyBind> v(kNumKeyActions);
    for (int i = 0; i < kNumKeyActions; i++) v[i].window = kKeyActions[i].def;
    return v;
}
static std::vector<KeyBind> g_keys = DefaultKeys();
static std::vector<int> g_hotkeyFailed;              // actions whose global hotkey another program already owns

static int KeyIndexOf(int cmd) {
    for (int i = 0; i < kNumKeyActions; i++) if (kKeyActions[i].cmd == cmd) return i;
    return -1;
}
// Menu text for a command with its window shortcut: "&Play" -> "&Play\tX"
static std::wstring WithKey(const wchar_t* label, int cmd) {
    int i = KeyIndexOf(cmd);
    if (i < 0 || g_keys[i].window.empty()) return label;
    return std::wstring(label) + L"\t" + g_keys[i].window;
}

static const int PANEL_H  = 96;
static const int FOOTER_H = 26;

static const COLORREF C_BG    = RGB(16, 20, 26);
static const COLORREF C_LCD   = RGB(6, 12, 9);
static const COLORREF C_AMBER = RGB(233, 162, 59);   // accent: same amber as the icon
static const COLORREF C_DIM   = RGB(150, 100, 35);
static const COLORREF C_TEXT  = RGB(200, 210, 220);
static const COLORREF C_SEL   = RGB(36, 64, 96);
static const COLORREF C_ALT   = RGB(20, 25, 32);
static const COLORREF C_MUTED = RGB(110, 125, 140);
static const COLORREF C_BTN   = RGB(30, 38, 48);
static const COLORREF C_BTNHI = RGB(44, 56, 72);
static const COLORREF C_BTNDN = RGB(12, 16, 22);

// ----------------------------------------------------------------------------
//  String helpers
// ----------------------------------------------------------------------------
static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring FmtTime(double sec) {
    if (sec < 0) sec = 0;
    int t = (int)sec;
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else swprintf_s(b, L"%d:%02d", t / 60, t % 60);
    return b;
}
static std::wstring LowerExt(const std::wstring& path) {
    std::wstring e = fs::path(path).extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}
static bool IsAudioExt(const std::wstring& path) {
    static const wchar_t* exts[] = {
        L".mp3", L".mp2", L".aac", L".adts", L".m4a", L".m4b", L".mp4", L".wma", L".wav",
        L".flac", L".ogg", L".oga", L".opus", L".aif", L".aiff", L".ac3", L".ec3",
        L".mka", L".3gp", L".amr", L".wv", L".caf" };
    std::wstring e = LowerExt(path);
    for (auto x : exts) if (e == x) return true;
    return false;
}
// ---- web links -------------------------------------------------------------
static bool IsUrl(const std::wstring& s) {
    return _wcsnicmp(s.c_str(), L"http://", 7) == 0 || _wcsnicmp(s.c_str(), L"https://", 8) == 0;
}
static std::wstring UrlHostW(const std::wstring& url) {
    size_t a = url.find(L"://");
    a = (a == std::wstring::npos) ? 0 : a + 3;
    size_t b = url.find_first_of(L"/?#", a);
    std::wstring h = url.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
    size_t at = h.rfind(L'@');
    if (at != std::wstring::npos) h.erase(0, at + 1);
    return h.empty() ? url : h;
}
static bool IsAudioExt(const std::wstring& path);
// Title for a link that came without one: file name for direct audio files, else the host name.
static std::wstring UrlTitleW(const std::wstring& url) {
    std::wstring p = url;
    size_t q = p.find_first_of(L"?#");
    if (q != std::wstring::npos) p.resize(q);
    size_t se = p.find(L"://");
    size_t ls = p.rfind(L'/');
    if (ls != std::wstring::npos && se != std::wstring::npos && ls > se + 2) {
        std::wstring seg = p.substr(ls + 1);
        if (!seg.empty() && IsAudioExt(seg)) {
            size_t dot = seg.rfind(L'.');
            return dot == std::wstring::npos ? seg : seg.substr(0, dot);
        }
    }
    return UrlHostW(url);
}
static bool IsPlaylistExt(const std::wstring& path) {
    std::wstring e = LowerExt(path);
    return e == L".klyr" || e == L".small" || e == L".cumdrops" || e == L".m3u" || e == L".m3u8";
}
// ---- where settings live ---------------------------------------------------------------------------------------
//  Portable: everything (settings.ini, last.klyr, podcasts.dat) is kept in a "data" folder next to the .exe, so
//  the program can be copied, moved or run from a USB stick with its settings. If that folder can't be written
//  to (the .exe sits somewhere like C:\Program Files) it falls back to %APPDATA%\Klyraudio.
static void RenameIniSection(const std::wstring& ini) {
    // settings.ini keeps its values under a section named after the app; older versions used other names.
    HANDLE h = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, nullptr), got = 0;
    std::string txt(sz, '\0');
    if (sz && ReadFile(h, &txt[0], sz, &got, nullptr)) txt.resize(got); else txt.clear();
    CloseHandle(h);
    static const char* const oldSections[] = { "[SMALLAMP]", "[CUMAMP]" };
    bool changed = false;
    for (const char* sec : oldSections) {
        size_t p0 = txt.find(sec);
        if (p0 != std::string::npos) { txt.replace(p0, strlen(sec), "[Klyraudio]"); changed = true; break; }
    }
    if (!changed) return;
    HANDLE w = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(w, txt.data(), (DWORD)txt.size(), &wr, nullptr); CloseHandle(w); }
}

static bool DirWritable(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring probe = dir + L"\\.write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// First run in a new location: copy the files of an earlier install (%APPDATA%\Klyraudio, \SMALLAMP, \CUMAMP).
// The old folder is left untouched.
static void CopyOldSettings(const std::wstring& dst, const std::wstring& roaming) {
    static const wchar_t* const names[] = { L"\\Klyraudio", L"\\SMALLAMP", L"\\CUMAMP" };
    for (const wchar_t* n : names) {
        std::wstring src = roaming + n;
        if (_wcsicmp(src.c_str(), dst.c_str()) == 0) continue;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((src + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            CopyFileW((src + L"\\" + fd.cFileName).c_str(), (dst + L"\\" + fd.cFileName).c_str(), TRUE);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        RenameIniSection(dst + L"\\settings.ini");
        return;
    }
}

static std::wstring ComputeDataDir() {
    std::wstring roaming;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { roaming = p; CoTaskMemFree(p); }

    wchar_t exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring local;
    if (n > 0 && n < MAX_PATH) {
        local = exe;
        size_t slash = local.find_last_of(L"\\/");
        local = slash == std::wstring::npos ? L"data" : local.substr(0, slash) + L"\\data";
    }
    if (!local.empty()) {
        bool fresh = GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES;
        if (DirWritable(local)) {
            if (fresh) CopyOldSettings(local, roaming);
            return local;
        }
    }
    std::wstring dir = roaming + L"\\Klyraudio";
    bool fresh = GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (fresh) CopyOldSettings(dir, roaming);
    return dir;
}

static std::wstring AppDataDir() {          // computed once; "AppData" is historic - it is the portable data folder
    static const std::wstring dir = ComputeDataDir();
    return dir;
}

// ----------------------------------------------------------------------------
//  Equalizer (10-band, peaking biquads) + shared settings
// ----------------------------------------------------------------------------
struct EqSettings { bool on = false; float preamp = 0; float gain[10] = {0}; };
static EqSettings g_eq;
static std::mutex g_eqMtx;
static std::atomic<int> g_eqVer{1};
static const float kEqFreq[10] = { 60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000 };

struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

class EqProcessor {
public:
    void Init(int sr, int ch) { sr_ = sr; ch_ = ch; memset(z_, 0, sizeof z_); localVer_ = -1; }
    void Reset() { memset(z_, 0, sizeof z_); }
    void Refresh() {
        int v = g_eqVer.load();
        if (v == localVer_) return;
        EqSettings s;
        { std::lock_guard<std::mutex> lk(g_eqMtx); s = g_eq; }
        localVer_ = v;
        on_ = s.on;
        pre_ = pow(10.0, s.preamp / 20.0);
        for (int b = 0; b < 10; b++) {
            double g = s.gain[b];
            act_[b] = fabs(g) > 0.05 && kEqFreq[b] < sr_ * 0.45;
            if (!act_[b]) continue;
            double A = pow(10.0, g / 40.0), w0 = 2.0 * M_PI * kEqFreq[b] / sr_;
            double cw = cos(w0), sw = sin(w0), alpha = sw / (2.0 * 1.4);
            double a0 = 1 + alpha / A;
            Biquad q;
            q.b0 = (1 + alpha * A) / a0; q.b1 = (-2 * cw) / a0; q.b2 = (1 - alpha * A) / a0;
            q.a1 = (-2 * cw) / a0;       q.a2 = (1 - alpha / A) / a0;
            bq_[b] = q;
        }
    }
    void Process(int16_t* d, size_t frames) {
        if (!on_) return;
        for (size_t f = 0; f < frames; f++) {
            for (int c = 0; c < ch_; c++) {
                double x = d[f * ch_ + c] * (1.0 / 32768.0) * pre_;
                if (c < 8) {
                    for (int b = 0; b < 10; b++) {
                        if (!act_[b]) continue;
                        const Biquad& q = bq_[b];
                        double y = q.b0 * x + z_[c][b][0];
                        z_[c][b][0] = q.b1 * x - q.a1 * y + z_[c][b][1];
                        z_[c][b][1] = q.b2 * x - q.a2 * y;
                        x = y;
                    }
                }
                x = std::max(-1.0, std::min(1.0, x));
                d[f * ch_ + c] = (int16_t)(x * 32767.0);
            }
        }
    }
private:
    int sr_ = 44100, ch_ = 2, localVer_ = -1;
    bool on_ = false;
    double pre_ = 1.0;
    bool act_[10] = {};
    Biquad bq_[10];
    double z_[8][10][2];
};

// Ring buffer of recently played mono samples for the spectrum analyzer
static const int kRing = 1 << 16;
static float g_ring[kRing];
static std::atomic<long long> g_ringWritten{0};
static std::atomic<long long> g_played{0};

// ----------------------------------------------------------------------------
//  Player: decoder + waveOut on a worker thread
// ----------------------------------------------------------------------------
static const DWORD kAudioStream = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;

class Player {
    // ---- types used below ------------------------------------------------------------------------------------
    // Control flags live in a per-track Session so an abandoned (stuck) worker can never be
    // revived or confused by the next track's state.
    struct Session {
        std::atomic<bool> quit{false}, paused{false};
        std::atomic<long long> seekReq{-1};
        std::atomic<unsigned long long> seekStamp{0};
        std::atomic<unsigned long long> seekFirst{0};   // when the oldest not-yet-handled seek was requested
        std::atomic<unsigned long long> beat{0};     // last time the worker loop ran (0 = not started)
    };

    // One open decoder. The worker holds the track being played and, shortly before it ends, the next one.
    // Everything a stream hands out is 16-bit PCM in the audio device's format.
    struct Stream {
        ComPtr<IMFSourceReader> reader;
        std::wstring path;
        int id = 0;                         // playlist id of the track (0 = unknown)
        UINT32 sr = 0, ch = 0;              // format the decoder delivers
        UINT32 outSr = 0;                   // format handed out (differs from sr when converted)
        double dur = 0;                     // seconds, 0 = unknown (live radio)
        bool net = false;
        std::vector<BYTE> pending;          // decoded but not yet consumed (device format)
        size_t pendOff = 0;
        bool eos = false;
        // baseSec = media time of the first sample handed out since the last (re)positioning. After a
        // seek the decoder rarely lands exactly on the requested time, so it is corrected from the first
        // decoded sample's own timestamp (needBase); baseDirty tells the worker the correction happened.
        double baseSec = 0;
        bool needBase = true, baseDirty = false;
        int readErrs = 0;                   // consecutive failed reads (a single hiccup must not end the track)
        double recoverPos = -1;             // position to fall back to if a seek lands past the real end
        long long frames = 0;               // frames handed out since the last (re)positioning
        bool convert = false;
        rsmp::Resampler rs;
        std::vector<int16_t> conv;          // scratch for the resampler
        bool Drained() const { return eos && pendOff >= pending.size(); }
    };

    // A next-track open running on a helper thread, so a slow disk or network never starves the audio device.
    struct Prep {
        std::atomic<bool> done{false};
        std::unique_ptr<Stream> s;          // null when opening failed or the format can't follow the device
    };

    // What the playlist says plays after the current track. `after` is the id of the track the playlist
    // believes is playing, so a stale answer (computed before a track change was processed) is never used.
    struct Next { std::wstring path; int id = 0, after = 0; };

public:
    enum State { Stopped, Playing, Paused };
    HWND notify = nullptr;
    std::atomic<double> posSec{0}, durSec{0};
    std::atomic<float> volume{0.8f};
    std::atomic<int> gen{0};
    std::atomic<int> sampleRate{0}, channels{0};
    std::atomic<bool> netStream{false};     // current track is a web link (radio / podcast)
    std::atomic<bool> gapless{true};        // carry on into the next entry without closing the audio device
    std::atomic<int> fadeMs{0};             // crossfade length between tracks in ms (0 = off)
    State state = Stopped;

    void Play(const std::wstring& path, int id = 0) {
        Stop();
        sess_ = std::make_shared<Session>();
        posSec = 0; durSec = 0; sampleRate = 0; channels = 0;
        netStream = IsUrl(path);
        int g = ++gen;
        state = Playing;
        th_ = std::thread(&Player::Run, this, path, id, g, sess_);
    }
    void Stop() {
        if (sess_) sess_->quit = true;
        if (th_.joinable()) {
            // Give the worker a moment to wind down. If it is wedged inside the decoder or the
            // audio driver (it used to happen after rapid seeking), abandon it instead of freezing
            // the whole window; it owns its own Session so it can't disturb the next track.
            if (WaitForSingleObject((HANDLE)th_.native_handle(), 1500) == WAIT_OBJECT_0) th_.join();
            else th_.detach();
        }
        sess_.reset();
        state = Stopped;
        posSec = 0;
        ++gen;
        sampleRate = 0; channels = 0;
    }
    void TogglePause() {
        if (!sess_) return;
        if (state == Playing) { sess_->paused = true; state = Paused; }
        else if (state == Paused) { sess_->paused = false; state = Playing; }
    }
    // True if the worker loop has made no progress for several seconds (wedged in the decoder
    // or the audio driver). The UI uses this to restart the track instead of leaving it dead.
    bool Stalled() const {
        auto ss = sess_;
        if (!ss || state != Playing) return false;
        unsigned long long b = ss->beat.load();
        // network sources legitimately block for a while when the connection hiccups
        return b != 0 && GetTickCount64() - b > (netStream ? 45000ULL : 5000ULL);
    }
    void Seek(double s) {
        if (state == Stopped) return;
        if (s < 0) s = 0;
        // Never ask the decoder to seek to (or past) the very end of the file: some
        // containers fail or return nothing there, which used to wedge the player.
        double d = durSec.load();
        if (d > 0.5 && s > d - 0.25) s = d - 0.25;
        auto ss = sess_;
        if (!ss) return;
        unsigned long long nowTick = GetTickCount64();
        if (ss->seekReq.load() < 0) ss->seekFirst = nowTick;
        ss->seekStamp = nowTick;
        ss->seekReq = (long long)(s * 1e7);
        posSec = s;
    }
    // The playlist tells the worker which track follows the current one (see Next).
    void SetNext(const std::wstring& path, int id, int after) {
        std::lock_guard<std::mutex> lk(nextMtx_);
        next_.path = path; next_.id = id; next_.after = after;
    }
    void ClearNext() {
        std::lock_guard<std::mutex> lk(nextMtx_);
        next_ = Next();
    }

private:
    std::thread th_;
    std::shared_ptr<Session> sess_;
    std::mutex nextMtx_;
    Next next_;

    Next PeekNext() {
        std::lock_guard<std::mutex> lk(nextMtx_);
        return next_;
    }

    void Run(std::wstring path, int id, int g, std::shared_ptr<Session> sess) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        try { ok = Decode(path, id, g, *sess); } catch (...) { ok = false; }
        if (!ok && !sess->quit) PostMessage(notify, WM_APP_TRACKERR, 0, (LPARAM)g);
        CoUninitialize();
    }

    // Opens `path` for decoding to 16-bit PCM. With devSr == 0 the stream's own rate and channel count
    // define the output format; otherwise the stream must have devCh channels and is resampled to devSr
    // when its own rate differs.
    static bool OpenStream(Stream& s, const std::wstring& path, UINT32 devSr, UINT32 devCh) {
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return false;
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(reader->SetStreamSelection(kAudioStream, TRUE))) return false;

        UINT32 nativeSr = 44100, nativeCh = 2;
        {
            ComPtr<IMFMediaType> nt;
            if (SUCCEEDED(reader->GetNativeMediaType(kAudioStream, 0, &nt))) {
                UINT32 v = 0;
                if (SUCCEEDED(nt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v)) && v) nativeSr = v;
                if (SUCCEEDED(nt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v)) && v) nativeCh = v;
            }
        }
        auto trySet = [&](UINT32 sr, UINT32 ch) -> bool {
            ComPtr<IMFMediaType> mt;
            if (FAILED(MFCreateMediaType(&mt))) return false;
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (sr) mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sr);
            if (ch) {
                mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
                mt->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
                if (sr) mt->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sr * ch * 2);
            }
            return SUCCEEDED(reader->SetCurrentMediaType(kAudioStream, nullptr, mt.Get()));
        };
        if (!(trySet(nativeSr, 2) || trySet(nativeSr, nativeCh) || trySet(0, 0))) return false;

        UINT32 sr = 0, ch = 0, bits = 0;
        {
            ComPtr<IMFMediaType> cur;
            if (FAILED(reader->GetCurrentMediaType(kAudioStream, &cur))) return false;
            cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
            cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            cur->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        }
        if (bits != 16 || ch < 1 || ch > 2 || sr < 8000) return false;
        if (devSr != 0 && ch != devCh) return false;       // the open device can't change channel layout

        double dur = 0;
        {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))
                && pv.vt == VT_UI8)
            {
                // live radio reports no duration, or an absurd one: treat anything over a week as "unknown"
                double d = pv.uhVal.QuadPart / 1e7;
                if (d > 0 && d < 7 * 86400.0) dur = d;
            }
            PropVariantClear(&pv);
        }

        s.reader = reader;
        s.path = path;
        s.net = IsUrl(path);
        s.sr = sr; s.ch = ch;
        s.outSr = devSr ? devSr : sr;
        s.dur = dur;
        if (s.outSr != sr) { s.convert = true; s.rs.Init((int)sr, (int)s.outSr, (int)ch); }
        return true;
    }

    // Hands out up to `frames` frames of decoded audio. Returns fewer when the stream ended, a seek or
    // quit is waiting, or the decoder is stuck - the caller tells those apart through s.eos / the Session.
    static size_t ReadFrames(Stream& s, int16_t* dst, size_t frames, Session& S) {
        const size_t bpf = (size_t)s.ch * 2;
        BYTE* out = (BYTE*)dst;
        const size_t cap = frames * bpf;
        size_t got = 0;
        int idle = 0;
        // Bail out early on quit / a new seek request so Stop and seeking stay responsive.
        while (got < cap && !S.quit && S.seekReq.load() < 0) {
            if (s.pendOff < s.pending.size()) {
                size_t n = std::min(cap - got, s.pending.size() - s.pendOff);
                memcpy(out + got, s.pending.data() + s.pendOff, n);
                s.pendOff += n; got += n;
                continue;
            }
            if (s.eos) break;
            s.pending.clear(); s.pendOff = 0;
            DWORD flags = 0;
            ComPtr<IMFSample> smp;
            HRESULT hr = s.reader->ReadSample(kAudioStream, 0, nullptr, &flags, nullptr, &smp);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                if (++s.readErrs > 4) { s.eos = true; break; }
                Sleep(5);
                continue;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) s.eos = true;
            if (!smp && !s.eos && ++idle > 200) { s.eos = true; break; }   // decoder stuck: give up, don't spin
            if (smp) {
                idle = 0; s.readErrs = 0; s.recoverPos = -1;
                if (s.needBase) {
                    LONGLONG t = 0;
                    if (SUCCEEDED(smp->GetSampleTime(&t)) && t >= 0) s.baseSec = t / 1e7;
                    s.needBase = false;
                    s.baseDirty = true;
                }
                ComPtr<IMFMediaBuffer> buf;
                if (SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))) {
                    BYTE* p = nullptr; DWORD len = 0;
                    if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                        if (s.convert) {
                            s.conv.clear();
                            s.rs.Process((const int16_t*)p, len / bpf, s.conv);
                            const BYTE* c = (const BYTE*)s.conv.data();
                            s.pending.assign(c, c + s.conv.size() * sizeof(int16_t));
                        } else {
                            len -= len % (DWORD)bpf;
                            s.pending.assign(p, p + len);
                        }
                        buf->Unlock();
                    }
                }
            }
        }
        s.frames += (long long)(got / bpf);
        return got / bpf;
    }

    static HRESULT SeekStream(Stream& s, long long hns) {
        PROPVARIANT v; PropVariantInit(&v); v.vt = VT_I8; v.hVal.QuadPart = hns;
        HRESULT hr = s.reader->SetCurrentPosition(GUID_NULL, v);
        PropVariantClear(&v);
        s.pending.clear(); s.pendOff = 0; s.eos = false; s.frames = 0;
        if (s.convert) s.rs.Reset();
        return hr;
    }

    static void PrepMain(std::shared_ptr<Prep> p, std::wstring path, int id, UINT32 sr, UINT32 ch) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {   // everything holding COM objects is released before CoUninitialize, whichever thread ends up owning it
            auto s = std::make_unique<Stream>();
            s->id = id;
            bool ok = false;
            try { ok = OpenStream(*s, path, sr, ch); } catch (...) { ok = false; }
            if (ok) p->s = std::move(s);
            s.reset();
            p->done = true;
            p.reset();
        }
        CoUninitialize();
    }

    bool Decode(const std::wstring& path, int playId, int g, Session& S) {
        auto cur = std::make_unique<Stream>();
        cur->id = playId;
        if (!OpenStream(*cur, path, 0, 0)) return false;
        if (S.quit) return true;          // superseded while the file was opening
        const UINT32 sr = cur->outSr, ch = cur->ch;
        sampleRate = (int)sr; channels = (int)ch;
        if (cur->dur > 0) durSec = cur->dur;

        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)ch; wf.nSamplesPerSec = sr;
        wf.wBitsPerSample = 16; wf.nBlockAlign = (WORD)(ch * 2); wf.nAvgBytesPerSec = sr * wf.nBlockAlign;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HWAVEOUT hwo = nullptr;
        if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            CloseHandle(ev);
            return false;
        }

        const int NB = 4;
        const DWORD blockAlign = wf.nBlockAlign;
        const DWORD bufBytes = (sr / (netStream ? 4 : 10)) * blockAlign;   // ~100 ms per buffer (250 ms for web streams)
        std::vector<std::vector<BYTE>> mem(NB, std::vector<BYTE>(bufBytes));
        WAVEHDR hdr[NB] = {};
        for (int i = 0; i < NB; i++) {
            hdr[i].lpData = (LPSTR)mem[i].data();
            hdr[i].dwBufferLength = bufBytes;
            waveOutPrepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
            hdr[i].dwFlags |= WHDR_DONE;
        }

        // ---- gapless / crossfade state ------------------------------------------------------------------------
        std::unique_ptr<Stream> nxt;      // the next track, open and ready
        std::shared_ptr<Prep> prep;       // the next track, still being opened on a helper thread
        int prepId = 0, failedId = 0;     // which track prep is opening / which one could not be chained
        unsigned long long dryTick = 0;   // when the current track ran dry (bounds the wait for the next one)
        bool fading = false;              // crossfade in progress: audio = cur fading out + nxt fading in
        size_t fadePos = 0, fadeTotal = 0;
        // A track change becomes audible only once the device reaches the sample where the new track begins;
        // that is also when the playlist is told (WM_APP_TRACKSWITCH).
        bool swPending = false;
        long long swSmp = 0;
        int swId = 0; double swDur = 0; bool swNet = false;
        // Position shown to the user = uiBaseSec + (device samples - uiStartSmp) / rate
        double uiBaseSec = 0;
        long long uiStartSmp = 0;
        std::vector<int16_t> mixA, mixB;

        EqProcessor eq; eq.Init((int)sr, (int)ch);
        bool devPaused = false, finished = false;
        long long curSmp = 0;        // samples the device has played since the last reset
        long long submitted = 0;     // samples handed to the device since the last reset
        g_ringWritten = 0; g_played = 0;

        auto chainOn = [&]() { return gapless.load() || fadeMs.load() > 0; };
        auto remainingSec = [&]() -> double {        // decode head to end of the current track; -1 = unknown
            if (cur->dur <= 0) return -1;
            return cur->dur - (cur->baseSec + (double)cur->frames / (double)sr);
        };
        auto beginSwitch = [&](size_t doneFrames) {
            swPending = true;
            swSmp = submitted + (long long)doneFrames;
            swId = nxt->id; swDur = nxt->dur; swNet = nxt->net;
        };

        // Fills one device buffer. Returns bytes written.
        auto fillBuf = [&](BYTE* dstB, DWORD capBytes) -> DWORD {
            int16_t* dst = (int16_t*)dstB;
            const size_t cap = capBytes / blockAlign;
            size_t done = 0;
            while (done < cap && !S.quit && S.seekReq.load() < 0) {
                const size_t want = cap - done;
                if (fading) {
                    const size_t n = std::min(want, fadeTotal - fadePos);
                    mixA.assign(n * ch, 0); mixB.assign(n * ch, 0);
                    const size_t a = ReadFrames(*cur, mixA.data(), n, S);
                    const size_t b = ReadFrames(*nxt, mixB.data(), n, S);
                    const size_t m = std::max(a, b);
                    for (size_t i = 0; i < m; i++) {
                        const double t = (double)(fadePos + i) / (double)fadeTotal;   // equal-power curves
                        const float ga = (float)std::cos(t * 1.5707963267948966);
                        const float gb = (float)std::sin(t * 1.5707963267948966);
                        for (UINT32 c = 0; c < ch; c++) {
                            float v = mixA[i * ch + c] * ga + mixB[i * ch + c] * gb;
                            v = v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v);
                            dst[(done + i) * ch + c] = (int16_t)std::lrintf(v);
                        }
                    }
                    fadePos += m; done += m;
                    // An incoming track that runs out early just fades in silence (mixB stays 0) until the
                    // outgoing track's fade-out is finished; only the outgoing track running dry ends the fade.
                    const bool curEnded = a < n && cur->Drained();
                    if (fadePos >= fadeTotal || curEnded) { cur = std::move(nxt); fading = false; }
                    else if (m < n) break;            // interrupted (seek / quit)
                    continue;
                }
                // crossfade due? (needs the next track open, a known length and a free switch slot)
                if (nxt && !swPending && fadeMs.load() > 0) {
                    const double rem = remainingSec(), fs = fadeMs.load() / 1000.0;
                    if (rem >= 0 && rem <= fs) {
                        const size_t total = (size_t)(std::max(0.0, rem) * sr);
                        if (total >= sr / 20) {       // under 50 ms left: a plain gapless hand-over does it
                            fading = true; fadePos = 0; fadeTotal = total;
                            beginSwitch(done);
                            continue;
                        }
                    }
                }
                const size_t n = ReadFrames(*cur, dst + done * ch, want, S);
                done += n;
                if (n < want) {
                    if (cur->Drained() && nxt && !swPending && chainOn()) {
                        // gapless hand-over: the next track carries on inside this very buffer
                        beginSwitch(done);
                        cur = std::move(nxt);
                        continue;
                    }
                    break;                            // end of track with nothing to follow, or interrupted
                }
            }
            return (DWORD)(done * blockAlign);
        };

        while (!S.quit) {
            S.beat = GetTickCount64();
            // Debounce scrubbing: wait until seek requests stop arriving (~120 ms) before touching
            // the decoder / audio device, so dragging or key-repeat doesn't hammer them.
            if (S.seekReq.load() >= 0) {
                unsigned long long now = GetTickCount64();
                if (now - S.seekStamp.load() < 120 && now - S.seekFirst.load() < 300) { Sleep(10); continue; }
            }
            long long sk = S.seekReq.exchange(-1);
            if (sk >= 0) {
                if (swPending) {
                    // A track change is about to become audible; the seek was meant for the old track.
                    // Drop it and let the change through now.
                    swSmp = std::min(swSmp, curSmp);
                } else {
                    if (fading) { cur = std::move(nxt); fading = false; }   // the playlist already shows the incoming track
                    double lastPos = uiBaseSec + (double)(curSmp - uiStartSmp) / (double)sr;
                    waveOutReset(hwo); devPaused = false;
                    HRESULT shr = SeekStream(*cur, sk);
                    // If the seek was refused the reader keeps its old position, so carry on from
                    // where we were instead of showing a position the audio never reached.
                    cur->baseSec = SUCCEEDED(shr) ? sk / 1e7 : lastPos;
                    cur->needBase = true;
                    cur->recoverPos = SUCCEEDED(shr) ? lastPos : -1;
                    uiBaseSec = cur->baseSec; uiStartSmp = 0;
                    curSmp = 0; submitted = 0;
                    g_ringWritten = 0; g_played = 0;
                    eq.Reset();
                }
            }
            bool wantPause = S.paused;
            if (wantPause != devPaused) {
                if (wantPause) waveOutPause(hwo); else waveOutRestart(hwo);
                devPaused = wantPause;
            }
            eq.Refresh();

            // ---- line up the next track ----------------------------------------------------------------------
            if (prep && prep->done.load()) {
                if (prep->s) nxt = std::move(prep->s); else failedId = prepId;
                prep.reset();
            }
            if (nxt && !fading && !swPending) {
                Next pk = PeekNext();                 // the playlist may have moved on while the file was opening
                if (!(pk.id == nxt->id && pk.after == cur->id)) nxt.reset();
            }
            if (chainOn() && !nxt && !prep && !fading && !swPending) {
                Next pk = PeekNext();
                const double rem = remainingSec();
                const bool soon = cur->eos || (rem >= 0 && rem <= fadeMs.load() / 1000.0 + 6.0);
                if (soon && pk.id != 0 && pk.after == cur->id && pk.id != failedId) {
                    prep = std::make_shared<Prep>();
                    prepId = pk.id;
                    std::thread(&Player::PrepMain, prep, pk.path, pk.id, sr, ch).detach();
                }
            }

            bool wrote = false;
            for (int i = 0; i < NB; i++) {
                if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
                DWORD n = fillBuf(mem[i].data(), bufBytes);
                if (n == 0) continue;

                int16_t* s = (int16_t*)mem[i].data();
                size_t frames = n / blockAlign;
                eq.Process(s, frames);

                long long w = g_ringWritten.load();
                for (size_t f = 0; f < frames; f++) {
                    float m = 0;
                    for (UINT32 c = 0; c < ch; c++) m += s[f * ch + c];
                    g_ring[(w + (long long)f) & (kRing - 1)] = m / (ch * 32768.0f);
                }
                g_ringWritten.store(w + (long long)frames);

                float vol = volume.load(); vol *= vol;
                if (vol < 0.999f)
                    for (size_t k = 0; k < frames * ch; k++) s[k] = (int16_t)(s[k] * vol);

                hdr[i].dwBufferLength = n;
                hdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(hwo, &hdr[i], sizeof(WAVEHDR));
                submitted += (long long)frames;
                wrote = true;
            }

            // Playback position. Prefer the device's own counter; some drivers report in
            // milliseconds or not at all, so fall back to "submitted minus still queued".
            {
                long long smp = -1;
                MMTIME mt{}; mt.wType = TIME_SAMPLES;
                if (waveOutGetPosition(hwo, &mt, sizeof mt) == MMSYSERR_NOERROR) {
                    if (mt.wType == TIME_SAMPLES) smp = mt.u.sample;
                    else if (mt.wType == TIME_BYTES) smp = mt.u.cb / blockAlign;
                    else if (mt.wType == TIME_MS) smp = (long long)mt.u.ms * sr / 1000;
                }
                if (smp < 0) {
                    long long queued = 0;
                    for (int i = 0; i < NB; i++)
                        if (!(hdr[i].dwFlags & WHDR_DONE)) queued += hdr[i].dwBufferLength / blockAlign;
                    smp = std::max(0LL, submitted - queued);
                }
                curSmp = smp;
                g_played = smp;
                if (swPending && smp >= swSmp) {
                    // the new track is now what the speakers play: tell the playlist, adopt its length
                    swPending = false;
                    const Stream& now = fading ? *nxt : *cur;
                    uiBaseSec = now.baseSec; uiStartSmp = swSmp;
                    durSec = swDur; netStream = swNet;
                    PostMessage(notify, WM_APP_TRACKSWITCH, (WPARAM)swId, (LPARAM)g);
                }
                if (!swPending && !fading && cur->baseDirty) { uiBaseSec = cur->baseSec; cur->baseDirty = false; }
                // While a seek is waiting to be processed the UI already shows the new target;
                // don't overwrite it with the old (pre-seek) position.
                if (S.seekReq.load() < 0 && !S.quit)
                    posSec = uiBaseSec + (smp > uiStartSmp ? (double)(smp - uiStartSmp) / (double)sr : 0.0);
            }

            if (!fading && !swPending && cur->eos && cur->needBase && cur->recoverPos >= 0 && cur->pendOff >= cur->pending.size()) {
                // The seek landed at/after the real end (e.g. a VBR file whose length is only an
                // estimate): there was no audio at all after it. Go back instead of ending the track.
                double back = cur->recoverPos; cur->recoverPos = -1;
                waveOutReset(hwo); devPaused = false;
                HRESULT rhr = SeekStream(*cur, (long long)(back * 1e7));
                if (SUCCEEDED(rhr)) {
                    cur->baseSec = back; cur->needBase = true;
                    uiBaseSec = back; uiStartSmp = 0; curSmp = 0; submitted = 0;
                    g_ringWritten = 0; g_played = 0; eq.Reset();
                    if (S.seekReq.load() < 0) posSec = back;
                    continue;
                }
            }

            if (!fading && cur->Drained()) {
                if (!dryTick) dryTick = GetTickCount64();
                // Is another track on its way? Wait for it (at most a few seconds) instead of ending, so the
                // device stays open. If it can't be chained the track simply ends and the playlist moves on.
                bool comingUp = false;
                if (chainOn()) {
                    if (nxt || prep) comingUp = true;
                    else { Next pk = PeekNext(); comingUp = pk.id != 0 && pk.after == cur->id && pk.id != failedId; }
                }
                if (comingUp && GetTickCount64() - dryTick > 6000) comingUp = false;
                if (!comingUp && !swPending) {
                    bool allDone = true;
                    for (int i = 0; i < NB; i++) if (!(hdr[i].dwFlags & WHDR_DONE)) allDone = false;
                    if (allDone) { finished = true; break; }
                }
            } else {
                dryTick = 0;
            }
            if (!wrote) WaitForSingleObject(ev, 30);
        }

        waveOutReset(hwo);
        for (int i = 0; i < NB; i++) waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
        waveOutClose(hwo);
        CloseHandle(ev);

        if (finished && !S.quit) PostMessage(notify, WM_APP_TRACKEND, 0, (LPARAM)g);
        return true;
    }
};

// ----------------------------------------------------------------------------
//  Globals: playlist & UI state
// ----------------------------------------------------------------------------
struct Track { int id; std::wstring path; std::wstring title; double dur; };

static std::vector<Track> g_tracks;
static int  g_cur = -1;
static int  g_nextId = 1;
static bool g_shuffle = false;
static int  g_repeat = 0;                 // 0 off, 1 all, 2 one
static std::vector<int> g_history;
static int  g_shufNextId = 0;            // track id shuffle picked to play next (0 = none yet)
static int  g_failCount = 0;
static bool g_onTop = false;
static bool g_seekDragging = false;
static std::wstring g_curTitle;
static std::atomic<bool> g_quitting{false};
static std::mt19937 g_rng{ std::random_device{}() };

static Player g_player;
static HINSTANCE g_hInst;
static HWND g_hwnd, g_list, g_seek, g_vol;
static HFONT g_fontBig, g_fontMed, g_fontUI, g_fontSmall, g_fontTitle;
static HWND g_hotBtn = nullptr;
static HWND g_eqWnd = nullptr;
static HWND g_plWnd = nullptr, g_podWnd = nullptr;     // playlist window, Podcasts window (see Layout below)
static bool g_plWanted = true, g_eqWanted = false, g_podWanted = false;   // which windows the user has open
static const int kMainClientH = 160;                  // the main window: display panel + transport row only
static int g_mainH = 0;                               // its frame height (set in WM_CREATE)
static HBRUSH g_brBg;
static float g_bars[32];
static float g_peaks[32];
static HACCEL g_accel;

typedef std::vector<std::pair<int, std::wstring>> MetaList;

// ----------------------------------------------------------------------------
//  Metadata loader (title/artist/duration) on a background thread
// ----------------------------------------------------------------------------
struct MetaResult { int id; std::wstring title; double dur; };

static const PROPERTYKEY kKeyTitle  = { { 0xF29F85E0, 0x4FF9, 0x1068, { 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 } }, 2 };
static const PROPERTYKEY kKeyArtist = { { 0x56A3372E, 0xCE9C, 0x11D2, { 0x9F, 0x0E, 0x00, 0x60, 0x97, 0xC6, 0x86, 0xF6 } }, 2 };

static std::wstring PropString(IPropertyStore* st, const PROPERTYKEY& k) {
    PROPVARIANT pv; PropVariantInit(&pv);
    std::wstring r;
    if (SUCCEEDED(st->GetValue(k, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) r = pv.pwszVal;
        else if (pv.vt == (VT_VECTOR | VT_LPWSTR) && pv.calpwstr.cElems > 0 && pv.calpwstr.pElems[0])
            r = pv.calpwstr.pElems[0];
    }
    PropVariantClear(&pv);
    return r;
}

static void MetaThread(MetaList items, HWND hwnd) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (auto& it : items) {
        if (g_quitting) break;
        ComPtr<IMFSourceReader> rd;
        if (FAILED(MFCreateSourceReaderFromURL(it.second.c_str(), nullptr, &rd))) continue;
        MetaResult* r = new MetaResult{ it.first, L"", -1 };
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))
            && pv.vt == VT_UI8)
            r->dur = pv.uhVal.QuadPart / 1e7;
        PropVariantClear(&pv);
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(rd->GetServiceForStream((DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                              MF_PROPERTY_HANDLER_SERVICE, IID_PPV_ARGS(&store)))) {
            std::wstring t = PropString(store.Get(), kKeyTitle);
            std::wstring a = PropString(store.Get(), kKeyArtist);
            if (!t.empty()) r->title = a.empty() ? t : a + L" - " + t;
        }
        if (!PostMessage(hwnd, WM_APP_META, 0, (LPARAM)r)) delete r;
    }
    CoUninitialize();
}

// ----------------------------------------------------------------------------
//  Playlist management
// ----------------------------------------------------------------------------
static void InvalidateFooter() {
    if (!g_plWnd) return;
    RECT c; GetClientRect(g_plWnd, &c);
    RECT r = { 0, c.bottom - FOOTER_H, c.right, c.bottom };
    InvalidateRect(g_plWnd, &r, FALSE);
}

static void RefreshList() {
    SendMessage(g_list, LB_SETCOUNT, (WPARAM)g_tracks.size(), 0);
    InvalidateRect(g_list, nullptr, TRUE);
    InvalidateFooter();
}

static void DarkTitle(HWND h) {
    BOOL v = TRUE;
    DwmSetWindowAttribute(h, 20, &v, sizeof v);   // DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 2004+/11)
    DwmSetWindowAttribute(h, 19, &v, sizeof v);   // same flag on early Win10 builds
}

static void UpdateTitleText() {
    if (g_cur >= 0 && g_cur < (int)g_tracks.size()) {
        g_curTitle = std::to_wstring(g_cur + 1) + L". " + g_tracks[g_cur].title;
        SetWindowTextW(g_hwnd, (L"Klyraudio - " + g_tracks[g_cur].title).c_str());
    } else {
        g_curTitle = L"Klyraudio";
        SetWindowTextW(g_hwnd, L"Klyraudio");
    }
}

static void AddTrack(const std::wstring& path, const std::wstring& title, double dur, MetaList& meta) {
    Track t;
    t.id = g_nextId++;
    t.path = path;
    t.title = !title.empty() ? title : IsUrl(path) ? UrlTitleW(path) : fs::path(path).stem().wstring();
    t.dur = dur;
    g_tracks.push_back(t);
    // web links are not probed in the background (a dead host or a live stream would stall the scan)
    if (dur <= 0 && !IsUrl(path)) meta.push_back({ t.id, path });
}

static void LoadPlaylistFile(const std::wstring& path, MetaList& meta) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    wchar_t dir[MAX_PATH] = {};
    wcsncpy_s(dir, path.c_str(), MAX_PATH - 1);
    PathRemoveFileSpecW(dir);

    bool nativeFmt = false, first = true;
    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first) {
            first = false;
            if (line == "SMALL 1" || line.rfind("CUMDROPS", 0) == 0) { nativeFmt = true; continue; }   // CUMDROPS = pre-1.2.7 header
        }
        if (line[0] == '#') continue;
        std::wstring w = Utf8ToW(line), p = w, title;
        double dur = -1;
        if (nativeFmt) {
            size_t t1 = w.find(L'\t');
            if (t1 != std::wstring::npos) {
                p = w.substr(0, t1);
                size_t t2 = w.find(L'\t', t1 + 1);
                title = w.substr(t1 + 1, t2 == std::wstring::npos ? std::wstring::npos : t2 - t1 - 1);
                if (t2 != std::wstring::npos) dur = _wtof(w.substr(t2 + 1).c_str());
                if (dur <= 0) dur = -1;
            }
        }
        if (!IsUrl(p) && PathIsRelativeW(p.c_str())) {
            wchar_t full[MAX_PATH] = {};
            PathCombineW(full, dir, p.c_str());
            p = full;
        }
        AddTrack(p, title, dur, meta);
    }
}

static bool SavePlaylistFile(const std::wstring& path) {
    std::string out = "SMALL 1\n";
    for (auto& t : g_tracks) {
        std::wstring title = t.title;
        for (auto& c : title) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        out += WToUtf8(t.path) + "\t" + WToUtf8(title) + "\t" +
               std::to_string(t.dur > 0 ? (int)(t.dur + 0.5) : 0) + "\n";
    }
    std::ofstream f(fs::path(path), std::ios::binary);
    if (!f) return false;
    f << out;
    return f.good();
}

static void AddPath(const std::wstring& p, MetaList& meta) {
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        std::vector<std::wstring> found;
        std::error_code ec;
        fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_regular_file(ec2) && IsAudioExt(it->path().wstring()))
                found.push_back(it->path().wstring());
        }
        std::sort(found.begin(), found.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (auto& f : found) AddTrack(f, L"", -1, meta);
    } else if (IsPlaylistExt(p)) {
        LoadPlaylistFile(p, meta);
    } else if (IsAudioExt(p)) {
        AddTrack(p, L"", -1, meta);
    }
}

static void PlayIndex(int idx, bool addHistory = true);
static void PodOnTrackStart(const std::wstring& path);   // marks podcast episodes as played
static void SetTrackPos(HWND tb, int pos);

// Folders (local or network) are scanned on a background thread so a big music
// library on a slow server never freezes the window. The result comes back as WM_APP_SCAN.
static std::atomic<int> g_scanCount{0};
struct ScanResult { std::vector<std::wstring> files; bool autoplay; };

static void ScanThread(std::vector<std::wstring> dirs, bool autoplay, HWND hwnd) {
    auto* r = new ScanResult{ {}, autoplay };
    try {
        for (auto& d : dirs) {
            std::vector<std::wstring> found;
            std::error_code ec;
            fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end && !g_quitting; it.increment(ec)) {
                std::error_code ec2;
                if (it->is_regular_file(ec2) && IsAudioExt(it->path().wstring()))
                    found.push_back(it->path().wstring());
            }
            std::sort(found.begin(), found.end(), [](const std::wstring& a, const std::wstring& b) {
                return _wcsicmp(a.c_str(), b.c_str()) < 0; });
            r->files.insert(r->files.end(), found.begin(), found.end());
        }
    } catch (...) {}
    if (g_quitting || !PostMessage(hwnd, WM_APP_SCAN, 0, (LPARAM)r)) delete r;
}

// ----------------------------------------------------------------------------
//  Web radio / podcast links. A link is inspected on a background thread (WinHTTP):
//    - podcast RSS feed        -> one playlist entry per episode
//    - .pls / .m3u radio list  -> one entry per stream
//    - anything audio-like     -> a single entry (live radio or a direct audio file)
//  The result comes back to the UI thread as WM_APP_URL. Playback itself is done by
//  Media Foundation, which streams http(s) URLs directly.
// ----------------------------------------------------------------------------
static std::atomic<int> g_urlCount{0};
struct UrlItem { std::wstring url, title; double dur; };
struct UrlResult { std::vector<UrlItem> items; std::wstring error; bool autoplay = false; };

struct HttpConn {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    int status = 0;
    DWORD err = 0;
    std::wstring ctype, icyName;
    ~HttpConn() {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
};

static std::wstring HttpHeader(HINTERNET req, DWORD level, LPCWSTR name = WINHTTP_HEADER_NAME_BY_INDEX) {
    DWORD sz = 0;
    WinHttpQueryHeaders(req, level, name, WINHTTP_NO_OUTPUT_BUFFER, &sz, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sz == 0) return L"";
    std::wstring v(sz / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, level, name, &v[0], &sz, WINHTTP_NO_HEADER_INDEX)) return L"";
    v.resize(sz / sizeof(wchar_t));
    return v;
}

// Connects and reads the response headers only (the body of a live stream never ends).
static bool HttpOpen(const std::wstring& url, HttpConn& c) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || !uc.lpszHostName) { c.err = ERROR_INVALID_PARAMETER; return false; }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.lpszUrlPath ? uc.dwUrlPathLength : 0);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    c.ses = WinHttpOpen(L"Klyraudio/1.6.6", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c.ses) c.ses = WinHttpOpen(L"Klyraudio/1.6.6", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c.ses) { c.err = GetLastError(); return false; }
    WinHttpSetTimeouts(c.ses, 8000, 8000, 8000, 12000);
    c.con = WinHttpConnect(c.ses, host.c_str(), uc.nPort, 0);
    if (!c.con) { c.err = GetLastError(); return false; }
    c.req = WinHttpOpenRequest(c.con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               https ? WINHTTP_FLAG_SECURE : 0);
    if (!c.req) { c.err = GetLastError(); return false; }
    DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(c.req, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(c.req, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof dec);   // ignored on old systems
    static const wchar_t hdrs[] = L"Icy-MetaData: 0\r\nAccept: */*\r\n";
    if (!WinHttpSendRequest(c.req, hdrs, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(c.req, nullptr)) { c.err = GetLastError(); return false; }
    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(c.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &st, &sz, WINHTTP_NO_HEADER_INDEX);
    c.status = (int)st;
    c.ctype = HttpHeader(c.req, WINHTTP_QUERY_CONTENT_TYPE);
    for (auto& ch : c.ctype) ch = (wchar_t)towlower(ch);
    c.icyName = HttpHeader(c.req, WINHTTP_QUERY_CUSTOM, L"icy-name");
    return true;
}

static bool HttpReadBody(HttpConn& c, size_t maxBytes, std::string& out) {
    for (;;) {
        if (out.size() >= maxBytes) return true;            // big enough: use what we have
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(c.req, &avail)) return !out.empty();
        if (avail == 0) return true;
        if (out.size() + avail > maxBytes) avail = (DWORD)(maxBytes - out.size());
        size_t old = out.size();
        out.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(c.req, &out[old], avail, &got)) { out.resize(old); return !out.empty(); }
        out.resize(old + got);
        if (got == 0) return true;
    }
}

static std::wstring ResolveUrl(const std::wstring& base, const std::wstring& rel) {
    if (IsUrl(rel)) return rel;
    size_t se = base.find(L"://");
    if (se == std::wstring::npos) return rel;
    size_t hostEnd = base.find(L'/', se + 3);
    std::wstring origin = hostEnd == std::wstring::npos ? base : base.substr(0, hostEnd);
    if (!rel.empty() && rel[0] == L'/') return origin + rel;
    std::wstring b = base;
    size_t q = b.find_first_of(L"?#");
    if (q != std::wstring::npos) b.resize(q);
    size_t ls = b.rfind(L'/');
    if (ls == std::wstring::npos || ls < se + 3) return origin + L"/" + rel;
    return b.substr(0, ls + 1) + rel;
}

static void UrlThread(std::wstring url, bool autoplay, HWND hwnd) {
    auto* r = new UrlResult;
    r->autoplay = autoplay;
    try {
        std::wstring host = UrlHostW(url);

        // file extension of the URL path (query string ignored)
        std::wstring ext, pathOnly = url;
        size_t qpos = pathOnly.find_first_of(L"?#");
        if (qpos != std::wstring::npos) pathOnly.resize(qpos);
        size_t se = pathOnly.find(L"://");
        size_t ps = pathOnly.find(L'/', se == std::wstring::npos ? 0 : se + 3);
        size_t dot = pathOnly.rfind(L'.');
        if (ps != std::wstring::npos && dot != std::wstring::npos && dot > ps) {
            ext = pathOnly.substr(dot + 1);
            for (auto& ch : ext) ch = (wchar_t)towlower(ch);
        }

        HttpConn c;
        if (!HttpOpen(url, c)) {
            if (c.err == ERROR_WINHTTP_INVALID_SERVER_RESPONSE) {
                // old Shoutcast servers answer "ICY 200 OK", which isn't valid HTTP to WinHTTP but
                // is perfectly playable: hand the address straight to the player
                r->items.push_back({ url, L"", -1 });
            } else if (c.err == ERROR_INVALID_PARAMETER) {
                r->error = L"That doesn't look like a valid web address.";
            } else {
                r->error = L"Couldn't connect to " + host + L".\n\nCheck the address and your internet connection.";
            }
        } else if (c.status >= 400) {
            r->error = L"The server at " + host + L" answered with HTTP error " + std::to_wstring(c.status) + L".";
        } else {
            auto has = [&](const wchar_t* t) { return c.ctype.find(t) != std::wstring::npos; };
            auto addItems = [&](const std::vector<feed::Item>& v, const std::wstring& prefix) {
                for (auto& it : v) {
                    std::wstring u = ResolveUrl(url, Utf8ToW(it.url));
                    if (!IsUrl(u)) continue;
                    std::wstring t = Utf8ToW(it.title);
                    if (!prefix.empty() && !t.empty()) t = prefix + L" - " + t;
                    r->items.push_back({ u, t, it.dur });
                }
            };
            auto parseRss = [&](const std::string& body) {
                std::string chan; std::vector<feed::Item> v;
                if (feed::ParseRss(body, chan, v)) addItems(v, Utf8ToW(chan));
                else r->error = L"That feed doesn't contain any audio episodes.";
            };

            if (ext == L"pls" || has(L"scpls") || has(L"x-pls")) {
                std::string body; HttpReadBody(c, 1 << 20, body);
                addItems(feed::ParsePls(body), L"");
            } else if (ext == L"m3u" || ext == L"m3u8" || has(L"mpegurl")) {
                std::string body; HttpReadBody(c, 1 << 20, body);
                if (feed::IsHls(body))
                    r->error = L"That is an HLS (adaptive .m3u8) stream, which isn't supported yet.\n\n"
                               L"Look for the station's plain MP3 / AAC stream address instead.";
                else addItems(feed::ParseM3u(body), L"");
            } else if (ext == L"rss" || ext == L"xml" || has(L"xml") || has(L"rss")) {
                std::string body; HttpReadBody(c, 12 << 20, body);
                parseRss(body);
            } else if (has(L"text/html")) {
                // some hosts serve feeds as text/html; sniff before giving up
                std::string body; HttpReadBody(c, 12 << 20, body);
                std::string head = feed::Lower(body.substr(0, 4096));
                if (head.find("<rss") != std::string::npos) parseRss(body);
                else r->error = L"That link is a web page, not an audio stream, playlist or podcast feed.\n\n"
                                L"For a podcast, use its RSS feed address.";
            } else {
                // audio/* (live radio or a direct file), ogg, octet-stream, ...: play it as is
                r->items.push_back({ url, c.icyName, -1 });
            }
            if (r->items.empty() && r->error.empty()) r->error = L"Found nothing playable at that link.";
        }
    } catch (...) {
        r->items.clear();
        r->error = L"Something went wrong while reading that link.";
    }
    if (g_quitting || !PostMessage(hwnd, WM_APP_URL, 0, (LPARAM)r)) delete r;
}

static void StartUrlFetch(const std::wstring& url, bool autoplay) {
    ++g_urlCount;
    InvalidateFooter();
    std::thread(UrlThread, url, autoplay, g_hwnd).detach();
}

static void AddPaths(const std::vector<std::wstring>& paths, bool autoplayIfStopped) {
    size_t before = g_tracks.size();
    MetaList meta;
    std::vector<std::wstring> dirs;
    for (auto& p : paths) {
        if (IsUrl(p)) { StartUrlFetch(p, autoplayIfStopped); continue; }     // radio / podcast link
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) dirs.push_back(p);
        else AddPath(p, meta);
    }
    RefreshList();
    if (!meta.empty()) std::thread(MetaThread, meta, g_hwnd).detach();
    if (autoplayIfStopped && g_player.state == Player::Stopped && g_tracks.size() > before)
        PlayIndex((int)before);
    if (!dirs.empty()) {
        ++g_scanCount;
        InvalidateFooter();
        std::thread(ScanThread, dirs, autoplayIfStopped, g_hwnd).detach();
    }
}

// ----------------------------------------------------------------------------
//  Transport
// ----------------------------------------------------------------------------
static void UpdateUi();

static void StopPlayback() {
    g_player.Stop();
    g_seekDragging = false;
    SetTrackPos(g_seek, 0);
    UpdateUi();
}

// Makes track `idx` the current one in the playlist (history, title, position). Does not touch the player.
static void CommitTrack(int idx, bool addHistory) {
    if (addHistory && g_cur >= 0 && g_cur != idx) {
        g_history.push_back(g_cur);
        if (g_history.size() > 500) g_history.erase(g_history.begin());
    }
    g_cur = idx;
    g_seekDragging = false;
    PodOnTrackStart(g_tracks[idx].path);
    SetTrackPos(g_seek, 0);
    UpdateTitleText();
    InvalidateRect(g_list, nullptr, FALSE);
}

static void PlayIndex(int idx, bool addHistory) {
    if (idx < 0 || idx >= (int)g_tracks.size()) return;
    CommitTrack(idx, addHistory);
    g_player.Play(g_tracks[idx].path, g_tracks[idx].id);
    UpdateUi();
}

// Index of the track that plays after the current one, or -1 if playback would end here.
// repeatOne: "repeat one" makes the current track follow itself (automatic advance only).
// With shuffle the pick is remembered by track id, so the gapless preparation and a later Next agree.
static int PeekNextIdx(bool repeatOne) {
    int n = (int)g_tracks.size();
    if (n == 0) return -1;
    if (repeatOne && g_repeat == 2 && g_cur >= 0) return g_cur;
    if (g_shuffle) {
        if (n == 1) return 0;
        int idx = -1;
        if (g_shufNextId)
            for (int i = 0; i < n; i++) if (g_tracks[i].id == g_shufNextId) { idx = i; break; }
        if (idx < 0 || idx == g_cur) {
            do { idx = (int)(g_rng() % n); } while (idx == g_cur);
            g_shufNextId = g_tracks[idx].id;
        }
        return idx;
    }
    int nxt = g_cur + 1;
    if (nxt >= n) return g_repeat == 1 ? 0 : -1;
    return nxt;
}

// Tells the player which track follows the current one, so it can be opened ahead of time.
static void UpdateNext() {
    int nxt = PeekNextIdx(true);
    if (g_cur < 0 || g_cur >= (int)g_tracks.size() || nxt < 0) { g_player.ClearNext(); return; }
    g_player.SetNext(g_tracks[nxt].path, g_tracks[nxt].id, g_tracks[g_cur].id);
}

static void NextTrack(bool autoAdvance, bool ignoreRepeatOne = false) {
    if (g_tracks.empty()) return;
    int nxt = PeekNextIdx(autoAdvance && !ignoreRepeatOne);
    if (nxt < 0) { if (autoAdvance) StopPlayback(); return; }
    PlayIndex(nxt);
}

static void PrevTrack() {
    if (g_tracks.empty()) return;
    if (g_player.state != Player::Stopped && g_player.posSec > 3.0) { PlayIndex(g_cur, false); return; }
    if (g_shuffle && !g_history.empty()) {
        int p = g_history.back(); g_history.pop_back();
        if (p < (int)g_tracks.size()) { PlayIndex(p, false); return; }
    }
    int p = g_cur - 1;
    if (p < 0) p = (g_repeat == 1) ? (int)g_tracks.size() - 1 : 0;
    PlayIndex(p, false);
}

static void PlayCaret() {
    int idx = (int)SendMessage(g_list, LB_GETCARETINDEX, 0, 0);
    if (idx >= 0 && idx < (int)g_tracks.size()) PlayIndex(idx);
}

// Length of the current track: what the decoder reported, else what the playlist scan found.
static double CurrentDuration() {
    double d = g_player.durSec;
    if (d <= 0 && g_cur >= 0 && g_cur < (int)g_tracks.size()) d = g_tracks[g_cur].dur;
    return d > 0 ? d : 0;
}

static void SeekRelative(double d) {
    if (g_player.state == Player::Stopped) return;
    if (CurrentDuration() <= 0 && g_cur >= 0 && g_cur < (int)g_tracks.size() && IsUrl(g_tracks[g_cur].path))
        return;                                   // live radio can't be seeked
    double dur = CurrentDuration(), p = g_player.posSec + d;
    if (p < 0) p = 0;
    if (dur > 0 && p > dur - 0.5) p = dur - 0.5;
    g_player.Seek(p);
}

// Setting a trackbar position only repaints the thumb area; our custom-drawn amber fill
// spans the whole channel, so force a full repaint or gaps are left after big jumps.
static void SetTrackPos(HWND tb, int pos) {
    if ((int)SendMessage(tb, TBM_GETPOS, 0, 0) == pos) return;
    SendMessage(tb, TBM_SETPOS, TRUE, pos);
    RedrawWindow(tb, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void SetVolume(int v) {
    v = std::max(0, std::min(100, v));
    SetTrackPos(g_vol, v);
    g_player.volume = v / 100.0f;
}

// ----------------------------------------------------------------------------
//  Spectrum analyzer
// ----------------------------------------------------------------------------
static void FFT(std::complex<float>* a, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        std::complex<float> wl(cosf(ang), sinf(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; j++) {
                std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

static void UpdateSpectrum() {
    const int N = 1024;
    static std::complex<float> buf[N];
    static float win[N];
    static bool init = false;
    if (!init) { for (int i = 0; i < N; i++) win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1)); init = true; }

    float level[32] = {};
    int sr = g_player.sampleRate;
    if (g_player.state == Player::Playing && sr > 0) {
        long long w = g_ringWritten.load(), p = g_played.load();
        if (p > w) p = w;
        long long start = p - N;
        if (start >= 0 && (w - start) < kRing - 64) {
            for (int i = 0; i < N; i++) buf[i] = g_ring[(start + i) & (kRing - 1)] * win[i];
            FFT(buf, N);
            double fLo = 40.0, fHi = std::min(16000.0, sr * 0.5 * 0.95);
            for (int b = 0; b < 32; b++) {
                double f0 = fLo * pow(fHi / fLo, b / 32.0), f1 = fLo * pow(fHi / fLo, (b + 1) / 32.0);
                int k0 = std::max(1, (int)(f0 * N / sr)), k1 = std::max(k0 + 1, (int)(f1 * N / sr));
                k1 = std::min(k1, N / 2);
                float mx = 0;
                for (int k = k0; k < k1; k++) mx = std::max(mx, std::abs(buf[k]));
                float m = mx / (N / 4.0f);
                float db = 20.0f * log10f(m + 1e-6f) + 12.0f * b / 32.0f;
                level[b] = std::max(0.0f, std::min(1.0f, (db + 75.0f) / 75.0f));
            }
        }
    }
    for (int i = 0; i < 32; i++) {
        g_bars[i]  = std::max(level[i], g_bars[i] - 0.05f);
        g_peaks[i] = std::max(g_bars[i], g_peaks[i] - 0.012f);
    }
}

// ----------------------------------------------------------------------------
//  Painting
// ----------------------------------------------------------------------------
static void RoundBox(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int rad) {
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, border);
    SetDCBrushColor(dc, fill);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, op); SelectObject(dc, ob);
}
static void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
    SelectObject(dc, ob);
}

static void PaintPanel(HDC hdc, int W) {
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, PANEL_H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    RECT all = { 0, 0, W, PANEL_H };
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    // LCD time box
    RECT lcd = { 8, 8, 168, 72 };
    RoundBox(dc, lcd, C_LCD, C_DIM, 8);

    double pos = g_player.state == Player::Stopped ? 0.0 : g_player.posSec.load();
    SelectObject(dc, g_fontBig);
    SetTextColor(dc, C_AMBER);
    RECT tr = { lcd.left, lcd.top + 2, lcd.right, lcd.bottom - 16 };
    std::wstring ts = FmtTime(pos);
    DrawTextW(dc, ts.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, g_fontSmall);
    SetTextColor(dc, C_DIM);
    std::wstring st = g_player.state == Player::Playing ? L"PLAY" : g_player.state == Player::Paused ? L"PAUSE" : L"STOP";
    if (g_player.sampleRate > 0) {
        wchar_t b[96];
        swprintf_s(b, L"%s  %.1f kHz  %s", st.c_str(), g_player.sampleRate / 1000.0,
                   g_player.channels == 1 ? L"MONO" : L"STEREO");
        st = b;
    }
    RECT sr = { lcd.left + 6, lcd.bottom - 16, lcd.right - 4, lcd.bottom - 2 };
    DrawTextW(dc, st.c_str(), -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Spectrum
    RECT sp = { 176, 8, W - 8, 72 };
    RoundBox(dc, sp, C_LCD, C_DIM, 8);
    int sw = (sp.right - sp.left) - 8, sh = (sp.bottom - sp.top) - 8;
    if (sw > 64) {
        int bw = sw / 32;
        int x0 = sp.left + 4 + (sw - bw * 32) / 2;
        int nseg = sh / 4;
        for (int i = 0; i < 32; i++) {
            int segs = (int)(g_bars[i] * nseg);
            for (int s = 0; s < segs; s++) {
                float t = s / (float)nseg;
                COLORREF c = t < 0.6f ? RGB(233, 162, 59) : t < 0.85f ? RGB(240, 220, 0) : RGB(255, 60, 40);
                RECT r = { x0 + i * bw, sp.bottom - 5 - (s + 1) * 4 + 1, x0 + i * bw + bw - 1, sp.bottom - 5 - s * 4 };
                FillRectColor(dc, r, c);
            }
            // falling peak cap
            int ps = (int)(g_peaks[i] * nseg);
            if (ps > segs && ps > 0) {
                RECT r = { x0 + i * bw, sp.bottom - 5 - ps * 4 + 1, x0 + i * bw + bw - 1, sp.bottom - 5 - (ps - 1) * 4 };
                FillRectColor(dc, r, RGB(255, 220, 160));
            }
        }
    }

    // Title marquee
    RECT tt = { 8, 76, W - 8, 94 };
    HRGN clip = CreateRectRgn(tt.left, tt.top, tt.right, tt.bottom);
    SelectClipRgn(dc, clip);
    SelectObject(dc, g_fontMed);
    SetTextColor(dc, C_AMBER);
    SIZE ext{};
    GetTextExtentPoint32W(dc, g_curTitle.c_str(), (int)g_curTitle.size(), &ext);
    int avail = tt.right - tt.left - 8;
    int x = tt.left + 4;
    if (ext.cx > avail) {
        int cycle = ext.cx + 60;
        int off = (int)((GetTickCount64() / 25) % cycle);
        TextOutW(dc, x - off, tt.top, g_curTitle.c_str(), (int)g_curTitle.size());
        TextOutW(dc, x - off + cycle, tt.top, g_curTitle.c_str(), (int)g_curTitle.size());
    } else {
        TextOutW(dc, x, tt.top, g_curTitle.c_str(), (int)g_curTitle.size());
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    BitBlt(hdc, 0, 0, W, PANEL_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void DrawListItem(DRAWITEMSTRUCT* d) {
    if ((int)d->itemID < 0 || d->itemID >= g_tracks.size()) return;
    const Track& t = g_tracks[d->itemID];
    bool sel = (d->itemState & ODS_SELECTED) != 0;
    bool cur = (int)d->itemID == g_cur;
    RECT rc = d->rcItem;
    HDC dc = d->hDC;
    FillRectColor(dc, rc, sel ? C_SEL : ((d->itemID & 1) ? C_ALT : C_BG));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fontUI);

    // playing marker: small amber triangle
    if (cur) {
        int cy = (rc.top + rc.bottom) / 2;
        POINT tri[3] = { { rc.left + 6, cy - 4 }, { rc.left + 6, cy + 4 }, { rc.left + 12, cy } };
        HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
        HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCPenColor(dc, C_AMBER); SetDCBrushColor(dc, C_AMBER);
        Polygon(dc, tri, 3);
        SelectObject(dc, op); SelectObject(dc, ob);
    }

    // duration (right), index (right-aligned column), title (ellipsis)
    SetTextColor(dc, cur ? C_AMBER : C_MUTED);
    std::wstring dur = t.dur > 0 ? FmtTime(t.dur) : L"";
    RECT dr = rc; dr.right -= 8;
    DrawTextW(dc, dur.c_str(), -1, &dr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT nr = { rc.left + 16, rc.top, rc.left + 48, rc.bottom };
    std::wstring num = std::to_wstring(d->itemID + 1) + L".";
    DrawTextW(dc, num.c_str(), -1, &nr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, cur ? C_AMBER : C_TEXT);
    RECT tr = rc; tr.left += 56; tr.right -= 64;
    DrawTextW(dc, t.title.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// ----------------------------------------------------------------------------
//  Owner-drawn flat buttons (transport glyphs are drawn with GDI shapes)
// ----------------------------------------------------------------------------
static void DrawBtn(DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    int id = (int)d->CtlID;
    bool down = (d->itemState & ODS_SELECTED) != 0;
    bool hot  = d->hwndItem && d->hwndItem == g_hotBtn;
    bool active =
        (id == IDM_SHUFFLE && g_shuffle) || (id == IDM_REPEAT && g_repeat != 0) ||
        (id == IDM_PLAY && g_player.state == Player::Playing) ||
        (id == IDM_PAUSE && g_player.state == Player::Paused) ||
        (id == IDM_EQ && g_eqWnd && IsWindowVisible(g_eqWnd)) ||
        (id == ID_EQ_ON && g_eq.on);

    FillRect(dc, &rc, g_brBg);
    RECT br = rc; InflateRect(&br, -1, -1);
    RoundBox(dc, br, down ? C_BTNDN : hot ? C_BTNHI : C_BTN, active ? C_DIM : RGB(52, 64, 80), 6);

    COLORREF fg = active ? C_AMBER : C_TEXT;
    int cx = (rc.left + rc.right) / 2 + (down ? 1 : 0), cy = (rc.top + rc.bottom) / 2 + (down ? 1 : 0);
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, fg); SetDCBrushColor(dc, fg);

    auto tri = [&](int x0, int x1, bool left) {   // triangle spanning x0..x1
        POINT p[3];
        if (left) { p[0] = { x1, cy - 6 }; p[1] = { x1, cy + 6 }; p[2] = { x0, cy }; }
        else      { p[0] = { x0, cy - 6 }; p[1] = { x0, cy + 6 }; p[2] = { x1, cy }; }
        Polygon(dc, p, 3);
    };
    switch (id) {
    case IDM_PREV:  Rectangle(dc, cx - 8, cy - 6, cx - 5, cy + 7); tri(cx - 5, cx + 7, true); break;
    case IDM_PLAY:  tri(cx - 5, cx + 7, false); break;
    case IDM_PAUSE: Rectangle(dc, cx - 6, cy - 6, cx - 2, cy + 7); Rectangle(dc, cx + 2, cy - 6, cx + 6, cy + 7); break;
    case IDM_STOP:  Rectangle(dc, cx - 6, cy - 6, cx + 7, cy + 7); break;
    case IDM_NEXT:  tri(cx - 7, cx + 5, false); Rectangle(dc, cx + 5, cy - 6, cx + 8, cy + 7); break;
    default: {
        wchar_t txt[64] = {};
        GetWindowTextW(d->hwndItem, txt, 64);
        SelectObject(dc, g_fontUI);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        RECT tr = rc; if (down) OffsetRect(&tr, 1, 1);
        DrawTextW(dc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    }
    SelectObject(dc, op); SelectObject(dc, ob);

    if (d->itemState & ODS_FOCUS) {
        RECT fr = rc; InflateRect(&fr, -4, -4);
        SetTextColor(dc, C_MUTED);
        DrawFocusRect(dc, &fr);
    }
}

// ----------------------------------------------------------------------------
//  Dark, flat trackbars via NM_CUSTOMDRAW
// ----------------------------------------------------------------------------
static LRESULT TrackbarCD(NMCUSTOMDRAW* cd, bool vertical, bool centerFill) {
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT:
        FillRect(cd->hdc, &cd->rc, g_brBg);
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        if (cd->dwItemSpec == TBCD_TICS) return CDRF_SKIPDEFAULT;
        RECT th{}; SendMessage(cd->hdr.hwndFrom, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        int tcx = (th.left + th.right) / 2, tcy = (th.top + th.bottom) / 2;
        if (!vertical) {
            // The control's cached thumb rectangle can lag behind the value we just set, which
            // left the amber fill (and thumb) stuck. Derive the x position from the live value.
            HWND tb = cd->hdr.hwndFrom;
            RECT chr{}; SendMessage(tb, TBM_GETCHANNELRECT, 0, (LPARAM)&chr);
            LRESULT lo = SendMessage(tb, TBM_GETRANGEMIN, 0, 0), hi = SendMessage(tb, TBM_GETRANGEMAX, 0, 0);
            LRESULT pv = SendMessage(tb, TBM_GETPOS, 0, 0);
            int tw = std::max<int>(1, th.right - th.left);
            int span = (int)(chr.right - chr.left) - tw;
            if (hi > lo && span > 0)
                tcx = (int)chr.left + tw / 2 + (int)((pv - lo) * span / (hi - lo));
        }
        if (cd->dwItemSpec == TBCD_CHANNEL) {
            RECT ch = cd->rc;
            if (!vertical) { int cy = (ch.top + ch.bottom) / 2; ch.top = cy - 3; ch.bottom = cy + 3; }
            else           { int cx = (ch.left + ch.right) / 2; ch.left = cx - 3; ch.right = cx + 3; }
            RoundBox(cd->hdc, ch, C_LCD, C_DIM, 6);
            RECT f = ch; InflateRect(&f, vertical ? -1 : 0, vertical ? 0 : -1);
            if (!vertical) { f.left += 1; f.right = tcx; }
            else if (centerFill) {
                int mid = (cd->rc.top + cd->rc.bottom) / 2;
                f.top = std::min<int>(mid, tcy); f.bottom = std::max<int>(mid, tcy);
            } else { f.top = tcy; f.bottom -= 1; }
            if (f.right > f.left && f.bottom > f.top) FillRectColor(cd->hdc, f, C_AMBER);
            return CDRF_SKIPDEFAULT;
        }
        if (cd->dwItemSpec == TBCD_THUMB) {
            bool hot = (cd->uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
            RECT tr = vertical ? RECT{ tcx - 8, tcy - 5, tcx + 9, tcy + 6 } : RECT{ tcx - 5, tcy - 8, tcx + 6, tcy + 9 };
            RoundBox(cd->hdc, tr, hot ? RGB(255, 220, 160) : RGB(240, 200, 130), hot ? C_AMBER : C_DIM, 5);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

// ----------------------------------------------------------------------------
//  Equalizer window
// ----------------------------------------------------------------------------
static HWND g_eqBand[10], g_eqPre, g_eqOn;

static void ReadEqControls() {
    std::lock_guard<std::mutex> lk(g_eqMtx);
    g_eq.preamp = 12.0f - (float)SendMessage(g_eqPre, TBM_GETPOS, 0, 0);
    for (int i = 0; i < 10; i++) g_eq.gain[i] = 12.0f - (float)SendMessage(g_eqBand[i], TBM_GETPOS, 0, 0);
    ++g_eqVer;
}
static void SyncEqControls() {
    if (!g_eqWnd) return;
    std::lock_guard<std::mutex> lk(g_eqMtx);
    InvalidateRect(g_eqOn, nullptr, FALSE);
    SetTrackPos(g_eqPre, 12 - (int)lroundf(g_eq.preamp));
    for (int i = 0; i < 10; i++) SetTrackPos(g_eqBand[i], 12 - (int)lroundf(g_eq.gain[i]));
}

// ----------------------------------------------------------------------------
//  Window snapping (Winamp-style). The main, playlist, equalizer and Podcasts windows snap to each other.
//  Dragging a window moves the windows snapped to it as one group. Hold Shift when you start the drag to move
//  just that window (it leaves its group). Snapped means the edges touch; snap.h does the geometry.
// ----------------------------------------------------------------------------
static const int kSnapDist = 10;                     // pixels within which edges pull together
struct DockFollower { HWND h; int dx, dy; };         // offset of a snapped window from the dragged one's top-left
static std::vector<DockFollower> g_dockFollowers;

static snap::Rect RectOf(const RECT& r) { return snap::Rect{ r.left, r.top, r.right, r.bottom }; }
static RECT WinRect(HWND w) { RECT r{}; GetWindowRect(w, &r); return r; }

// The windows that take part in snapping right now (visible ones).
static std::vector<HWND> DockWindows() {
    std::vector<HWND> v;
    for (HWND h : { g_hwnd, g_plWnd, g_eqWnd, g_podWnd })
        if (h && IsWindowVisible(h) && !IsIconic(h)) v.push_back(h);
    return v;
}
static void DockBegin(HWND self) {
    g_dockFollowers.clear();
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return;                // Shift: move only this window
    std::vector<HWND> wins = DockWindows();
    std::vector<snap::Rect> rs;
    int si = -1;
    for (int i = 0; i < (int)wins.size(); i++) {
        if (wins[i] == self) si = i;
        rs.push_back(RectOf(WinRect(wins[i])));
    }
    if (si < 0) return;
    for (int i : snap::Group(rs, si))
        if (wins[i] != self) g_dockFollowers.push_back({ wins[i], rs[i].l - rs[si].l, rs[i].t - rs[si].t });
}
static void DockMoving(HWND self, RECT* rc) {
    std::vector<snap::Rect> others;
    for (HWND h : DockWindows()) {
        if (h == self) continue;
        bool follower = false;
        for (auto& f : g_dockFollowers) if (f.h == h) follower = true;
        if (!follower) others.push_back(RectOf(WinRect(h)));
    }
    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(MonitorFromRect(rc, MONITOR_DEFAULTTONEAREST), &mi);
    snap::Rect s = snap::SnapRect(RectOf(*rc), others, RectOf(mi.rcWork), kSnapDist);
    *rc = RECT{ s.l, s.t, s.r, s.b };
    for (auto& f : g_dockFollowers)
        SetWindowPos(f.h, nullptr, s.l + f.dx, s.t + f.dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void DockEnd() { g_dockFollowers.clear(); }

// Places a window from the saved layout (settings keys <prefix>_x/_y/_w/_h), or at `def` if there is none or the
// saved place is no longer on a screen.
static void PlaceWindow(HWND w, const wchar_t* prefix, RECT def) {
    if (!w) return;
    std::wstring ini = AppDataDir() + L"\\settings.ini";
    std::wstring p = prefix;
    auto ri = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"Klyraudio", (p + L"_" + k).c_str(), d, ini.c_str()); };
    RECT r = { ri(L"x", def.left), ri(L"y", def.top), 0, 0 };
    r.right = r.left + ri(L"w", def.right - def.left);
    r.bottom = r.top + ri(L"h", def.bottom - def.top);
    if (r.right - r.left < 200 || r.bottom - r.top < 120 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) r = def;
    SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
static void SaveRectSetting(const std::wstring& ini, const wchar_t* prefix, HWND w) {
    if (!w || IsIconic(w)) return;
    RECT r = WinRect(w);
    std::wstring p = prefix;
    auto wi = [&](const wchar_t* k, int v) {
        WritePrivateProfileStringW(L"Klyraudio", (p + L"_" + k).c_str(), std::to_wstring(v).c_str(), ini.c_str());
    };
    wi(L"x", r.left); wi(L"y", r.top); wi(L"w", r.right - r.left); wi(L"h", r.bottom - r.top);
}
static void SaveLayout(const std::wstring& ini) {
    auto wi = [&](const wchar_t* k, int v) { WritePrivateProfileStringW(L"Klyraudio", k, std::to_wstring(v).c_str(), ini.c_str()); };
    wi(L"pl_show", g_plWanted ? 1 : 0);
    wi(L"eq_show", g_eqWanted ? 1 : 0);
    wi(L"pod_show", (g_podWnd && IsWindowVisible(g_podWnd)) ? 1 : 0);
    SaveRectSetting(ini, L"pl", g_plWnd);
    SaveRectSetting(ini, L"eq", g_eqWnd);
    SaveRectSetting(ini, L"pod", g_podWnd);
}

static LRESULT CALLBACK EqProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_eqOn = CreateWindowW(L"BUTTON", L"Enable EQ", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               12, 8, 100, 24, h, (HMENU)(INT_PTR)ID_EQ_ON, g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      120, 8, 60, 24, h, (HMENU)(INT_PTR)ID_EQ_RESET, g_hInst, nullptr);
        auto mk = [&](int x, int id) {
            HWND t = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_NOTICKS,
                                   x, 40, 30, 130, h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
            SendMessage(t, TBM_SETRANGE, TRUE, MAKELONG(0, 24));
            SendMessage(t, TBM_SETPOS, TRUE, 12);
            return t;
        };
        g_eqPre = mk(30, 3100);
        for (int i = 0; i < 10; i++) g_eqBand[i] = mk(100 + i * 42, 3000 + i);
        EnumChildWindows(h, [](HWND c, LPARAM f) -> BOOL { SendMessage(c, WM_SETFONT, f, TRUE); return TRUE; }, (LPARAM)g_fontUI);
        return 0;
    }
    case WM_VSCROLL: InvalidateRect((HWND)l, nullptr, FALSE); ReadEqControls(); return 0;
    case WM_DRAWITEM: DrawBtn((DRAWITEMSTRUCT*)l); return TRUE;
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->code == NM_CUSTOMDRAW) return TrackbarCD((NMCUSTOMDRAW*)l, true, true);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_EQ_ON) {
            { std::lock_guard<std::mutex> lk(g_eqMtx); g_eq.on = !g_eq.on; }
            ++g_eqVer;
            InvalidateRect(g_eqOn, nullptr, FALSE);
        }
        else if (LOWORD(w) == ID_EQ_RESET) {
            { std::lock_guard<std::mutex> lk(g_eqMtx); g_eq.preamp = 0; for (auto& g : g_eq.gain) g = 0; }
            ++g_eqVer;
            SyncEqControls();
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, g_brBg); return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_TEXT); SelectObject(dc, g_fontSmall);
        static const wchar_t* names[10] = { L"60", L"170", L"310", L"600", L"1K", L"3K", L"6K", L"12K", L"14K", L"16K" };
        for (int i = 0; i < 10; i++) { RECT r = { 100 + i * 42 - 6, 172, 100 + i * 42 + 36, 190 }; DrawTextW(dc, names[i], -1, &r, DT_CENTER | DT_SINGLELINE); }
        RECT pr = { 14, 172, 66, 190 }; DrawTextW(dc, L"Preamp", -1, &pr, DT_CENTER | DT_SINGLELINE);
        RECT a = { 62, 40, 96, 56 }, b = { 62, 98, 96, 114 }, c = { 62, 156, 96, 172 };
        DrawTextW(dc, L"+12", -1, &a, DT_RIGHT | DT_SINGLELINE);
        DrawTextW(dc, L"0", -1, &b, DT_RIGHT | DT_SINGLELINE);
        DrawTextW(dc, L"-12", -1, &c, DT_RIGHT | DT_SINGLELINE);
        EndPaint(h, &ps); return 0;
    }
    case WM_ENTERSIZEMOVE: DockBegin(h); return 0;
    case WM_MOVING: DockMoving(h, (RECT*)l); return TRUE;
    case WM_EXITSIZEMOVE: DockEnd(); return 0;
    case WM_CLOSE:
        g_eqWanted = false;
        ShowWindow(h, SW_HIDE);
        InvalidateRect(GetDlgItem(g_hwnd, IDM_EQ), nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ToggleEq() {
    if (!g_eqWnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EqProc; wc.hInstance = g_hInst; wc.lpszClassName = L"KLYRAUDIO_EQ";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = g_brBg;
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        RECT r = { 0, 0, 540, 200 };
        AdjustWindowRect(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
        RECT mr = WinRect(g_hwnd);
        g_eqWnd = CreateWindowW(L"KLYRAUDIO_EQ", L"Klyraudio Equalizer", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                mr.left, mr.bottom, r.right - r.left, r.bottom - r.top,
                                g_hwnd, nullptr, g_hInst, nullptr);
        DarkTitle(g_eqWnd);
        SyncEqControls();
        PlaceWindow(g_eqWnd, L"eq", RECT{ mr.left, mr.bottom, mr.left + (r.right - r.left), mr.bottom + (r.bottom - r.top) });
        ShowWindow(g_eqWnd, SW_SHOW);
        g_eqWanted = true;
        InvalidateRect(GetDlgItem(g_hwnd, IDM_EQ), nullptr, FALSE);
        return;
    }
    g_eqWanted = !IsWindowVisible(g_eqWnd);
    ShowWindow(g_eqWnd, g_eqWanted ? SW_SHOW : SW_HIDE);
    InvalidateRect(GetDlgItem(g_hwnd, IDM_EQ), nullptr, FALSE);
}

// ----------------------------------------------------------------------------
//  Settings persistence
// ----------------------------------------------------------------------------
// Shortcuts in settings.ini: key_<name> (window shortcut) and gkey_<name> (global hotkey). Only values that
// differ from the default are written, so a later change of defaults still reaches users who never touched them.
// A missing entry means the default; an empty one means "no shortcut".
static void SaveKeyBindings(const std::wstring& ini) {
    for (int i = 0; i < kNumKeyActions; i++) {
        std::wstring nm = kKeyActions[i].name;
        const KeyBind& b = g_keys[i];
        WritePrivateProfileStringW(L"Klyraudio", (L"key_" + nm).c_str(),
            b.window == kKeyActions[i].def ? nullptr : b.window.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Klyraudio", (L"gkey_" + nm).c_str(),
            b.global.empty() ? nullptr : b.global.c_str(), ini.c_str());
    }
}
static void LoadKeyBindings(const std::wstring& ini) {
    g_keys = DefaultKeys();
    const wchar_t* missing = L"\x01";
    wchar_t buf[128];
    auto read = [&](const std::wstring& key, std::wstring& dst, bool allowWin) {
        GetPrivateProfileStringW(L"Klyraudio", key.c_str(), missing, buf, 128, ini.c_str());
        if (wcscmp(buf, missing) == 0) return;               // not in the file: keep the default
        if (buf[0] == 0) { dst.clear(); return; }             // explicitly "no shortcut"
        keymap::Chord c;
        if (keymap::Parse(buf, c) && (allowWin || !(c.mods & keymap::kWin))) dst = keymap::Format(c);
    };
    for (int i = 0; i < kNumKeyActions; i++) {
        std::wstring nm = kKeyActions[i].name;
        read(L"key_" + nm, g_keys[i].window, false);
        read(L"gkey_" + nm, g_keys[i].global, true);
    }
}

static void SaveSettings() {
    std::wstring ini = AppDataDir() + L"\\settings.ini";
    auto wi = [&](const wchar_t* k, int v) { WritePrivateProfileStringW(L"Klyraudio", k, std::to_wstring(v).c_str(), ini.c_str()); };
    wi(L"volume", (int)lroundf(g_player.volume * 100));
    wi(L"shuffle", g_shuffle); wi(L"repeat", g_repeat); wi(L"ontop", g_onTop);
    wi(L"gapless", g_player.gapless.load() ? 1 : 0); wi(L"fade_ms", g_player.fadeMs.load());
    SaveKeyBindings(ini);
    SaveLayout(ini);
    WINDOWPLACEMENT wp{ sizeof wp };
    if (g_hwnd && GetWindowPlacement(g_hwnd, &wp)) {
        wi(L"win_x", wp.rcNormalPosition.left);  wi(L"win_y", wp.rcNormalPosition.top);
        wi(L"win_w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
        wi(L"win_h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        wi(L"win_max", wp.showCmd == SW_SHOWMAXIMIZED);
    }
    std::lock_guard<std::mutex> lk(g_eqMtx);
    wi(L"eq_on", g_eq.on); wi(L"eq_pre", (int)lroundf(g_eq.preamp));
    for (int i = 0; i < 10; i++) wi((L"eq" + std::to_wstring(i)).c_str(), (int)lroundf(g_eq.gain[i]));
}
static void LoadSettings() {
    std::wstring ini = AppDataDir() + L"\\settings.ini";
    auto ri = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"Klyraudio", k, d, ini.c_str()); };
    SetVolume(ri(L"volume", 80));
    g_shuffle = ri(L"shuffle", 0) != 0; g_repeat = std::max(0, std::min(2, ri(L"repeat", 0)));
    g_onTop = ri(L"ontop", 0) != 0;
    g_player.gapless = ri(L"gapless", 1) != 0;
    g_player.fadeMs = std::max(0, std::min(12000, ri(L"fade_ms", 0)));
    LoadKeyBindings(ini);
    g_plWanted = ri(L"pl_show", 1) != 0; g_eqWanted = ri(L"eq_show", 0) != 0; g_podWanted = ri(L"pod_show", 0) != 0;
    std::lock_guard<std::mutex> lk(g_eqMtx);
    g_eq.on = ri(L"eq_on", 0) != 0; g_eq.preamp = (float)std::max(-12, std::min(12, ri(L"eq_pre", 0)));
    for (int i = 0; i < 10; i++) g_eq.gain[i] = (float)std::max(-12, std::min(12, ri((L"eq" + std::to_wstring(i)).c_str(), 0)));
    ++g_eqVer;
}

// Restore last window position/size (only if it still lands on a monitor). Returns true if maximized.
static bool RestoreWindowPos(HWND hwnd) {
    std::wstring ini = AppDataDir() + L"\\settings.ini";
    auto ri = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"Klyraudio", k, d, ini.c_str()); };
    // The main window has a fixed height (the panel and transport row); its position and width are kept.
    int w = ri(L"win_w", 0);
    if (w < 300) return false;
    RECT r = { ri(L"win_x", 0), ri(L"win_y", 0), 0, 0 };
    r.right = r.left + w; r.bottom = r.top + g_mainH;
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL))
        SetWindowPos(hwnd, nullptr, r.left, r.top, w, g_mainH, SWP_NOZORDER | SWP_NOACTIVATE);
    return false;
}

// ----------------------------------------------------------------------------
//  Dialogs
// ----------------------------------------------------------------------------
static void DoAddFiles() {
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = buf.data(); ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrFilter =
        L"Audio files and playlists\0*.mp3;*.mp2;*.aac;*.m4a;*.m4b;*.mp4;*.wma;*.wav;*.flac;*.ogg;*.oga;*.opus;"
        L"*.aif;*.aiff;*.ac3;*.mka;*.3gp;*.amr;*.klyr;*.small;*.cumdrops;*.m3u;*.m3u8\0All files (*.*)\0*.*\0";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> out;
    const wchar_t* p = buf.data();
    std::wstring first = p; p += first.size() + 1;
    if (*p == 0) out.push_back(first);
    else while (*p) { std::wstring f = p; out.push_back(first + L"\\" + f); p += f.size() + 1; }
    AddPaths(out, true);
}

static void DoAddFolder() {
    // Standard Windows Explorer-style folder picker: has the navigation pane with
    // Quick access, This PC, mapped drives and Network (\\server\share). You can also
    // paste a UNC path such as \\myserver\music into its address bar. Multi-select allowed.
    ComPtr<IFileOpenDialog> dlg;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Add folders to Klyraudio playlist");
        dlg->SetOkButtonLabel(L"Add Folder");
        if (FAILED(dlg->Show(g_hwnd))) return;      // cancelled
        ComPtr<IShellItemArray> arr;
        if (FAILED(dlg->GetResults(&arr))) return;
        DWORD n = 0;
        arr->GetCount(&n);
        std::vector<std::wstring> paths;
        for (DWORD i = 0; i < n; i++) {
            ComPtr<IShellItem> item;
            if (FAILED(arr->GetItemAt(i, &item))) continue;
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { paths.push_back(p); CoTaskMemFree(p); }
        }
        if (!paths.empty()) AddPaths(paths, true);
        return;
    }
    // fallback: old-style folder browser
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd; bi.lpszTitle = L"Add folder to Klyraudio playlist";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, path)) AddPaths({ path }, true);
    CoTaskMemFree(pidl);
}

static void DoOpenPlaylist() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_hwnd; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Playlists (*.klyr;*.small;*.cumdrops;*.m3u;*.m3u8)\0*.klyr;*.small;*.cumdrops;*.m3u;*.m3u8\0All files (*.*)\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    StopPlayback(); g_tracks.clear(); g_cur = -1; g_history.clear(); UpdateTitleText();
    AddPaths({ file }, false);
}

static void DoSavePlaylist() {
    wchar_t file[MAX_PATH] = L"playlist.klyr";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_hwnd; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Klyraudio playlist (*.klyr)\0*.klyr\0";
    ofn.lpstrDefExt = L"klyr";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!SavePlaylistFile(file)) MessageBoxW(g_hwnd, L"Could not save the playlist.", L"Klyraudio", MB_ICONERROR);
}

static void RemoveSelected() {
    int n = (int)SendMessage(g_list, LB_GETSELCOUNT, 0, 0);
    if (n <= 0) return;
    std::vector<int> sel(n);
    SendMessage(g_list, LB_GETSELITEMS, n, (LPARAM)sel.data());
    std::sort(sel.begin(), sel.end(), std::greater<int>());
    for (int i : sel) {
        if (i < 0 || i >= (int)g_tracks.size()) continue;
        g_tracks.erase(g_tracks.begin() + i);
        if (i < g_cur) g_cur--;
        else if (i == g_cur) g_cur = -1;
    }
    g_history.clear();
    RefreshList();
}

// ----------------------------------------------------------------------------
//  UI helpers
// ----------------------------------------------------------------------------
// ---- selection helpers -------------------------------------------------------
static std::vector<int> SelectedIndices() {
    std::vector<int> sel;
    int n = (int)SendMessage(g_list, LB_GETSELCOUNT, 0, 0);
    if (n > 0) {
        sel.resize(n);
        int got = (int)SendMessage(g_list, LB_GETSELITEMS, n, (LPARAM)sel.data());
        if (got < 0) sel.clear(); else sel.resize(got);
    }
    sel.erase(std::remove_if(sel.begin(), sel.end(), [](int i) { return i < 0 || i >= (int)g_tracks.size(); }), sel.end());
    return sel;
}

static void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void* p = GlobalLock(g);
        if (p) { memcpy(p, s.c_str(), bytes); GlobalUnlock(g); }
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

static void CopySelected(bool paths) {
    std::wstring out;
    for (int i : SelectedIndices()) {
        if (!out.empty()) out += L"\r\n";
        out += paths ? g_tracks[i].path : g_tracks[i].title;
    }
    if (!out.empty()) CopyText(out);
}

static void ShowInExplorer() {
    auto sel = SelectedIndices();
    if (sel.empty()) return;
    if (IsUrl(g_tracks[sel[0]].path)) return;
    std::wstring arg = L"/select,\"" + g_tracks[sel[0]].path + L"\"";
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

static void ShowProperties() {
    auto sel = SelectedIndices();
    if (sel.empty()) return;
    std::wstring path = g_tracks[sel[0]].path;
    if (IsUrl(path)) return;
    SHELLEXECUTEINFOW sei{ sizeof sei };
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.hwnd = g_hwnd; sei.lpVerb = L"properties"; sei.lpFile = path.c_str(); sei.nShow = SW_SHOW;
    ShellExecuteExW(&sei);
}

static void SortPlaylist(int mode) {
    if (g_tracks.size() < 2) return;
    int curId = (g_cur >= 0 && g_cur < (int)g_tracks.size()) ? g_tracks[g_cur].id : -1;
    switch (mode) {
    case IDM_SORT_TITLE:
        std::stable_sort(g_tracks.begin(), g_tracks.end(), [](const Track& a, const Track& b) {
            return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0; });
        break;
    case IDM_SORT_PATH:
        std::stable_sort(g_tracks.begin(), g_tracks.end(), [](const Track& a, const Track& b) {
            return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0; });
        break;
    case IDM_SORT_DUR:
        std::stable_sort(g_tracks.begin(), g_tracks.end(), [](const Track& a, const Track& b) { return a.dur < b.dur; });
        break;
    case IDM_SORT_REV:  std::reverse(g_tracks.begin(), g_tracks.end()); break;
    case IDM_SORT_RAND: std::shuffle(g_tracks.begin(), g_tracks.end(), g_rng); break;
    }
    g_cur = -1;
    for (int i = 0; i < (int)g_tracks.size(); i++) if (g_tracks[i].id == curId) { g_cur = i; break; }
    g_history.clear();
    RefreshList();
    UpdateTitleText();
}

static HMENU BuildSortMenu() {
    HMENU s = CreatePopupMenu();
    AppendMenuW(s, MF_STRING, IDM_SORT_TITLE, L"By &title");
    AppendMenuW(s, MF_STRING, IDM_SORT_PATH,  L"By file &path");
    AppendMenuW(s, MF_STRING, IDM_SORT_DUR,   L"By &duration");
    AppendMenuW(s, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(s, MF_STRING, IDM_SORT_REV,   L"&Reverse order");
    AppendMenuW(s, MF_STRING, IDM_SORT_RAND,  L"R&andomize");
    return s;
}

// ---- playlist right-click menu -----------------------------------------------
static void ShowListMenu(int x, int y) {
    if (x == -1 && y == -1) {                       // keyboard (Shift+F10 / menu key)
        int idx = (int)SendMessage(g_list, LB_GETCARETINDEX, 0, 0);
        RECT r{};
        if (idx >= 0 && SendMessage(g_list, LB_GETITEMRECT, idx, (LPARAM)&r) != LB_ERR) { x = r.left + 24; y = r.bottom; }
        else { x = 8; y = 8; }
        POINT p = { x, y }; ClientToScreen(g_list, &p); x = p.x; y = p.y;
    }
    auto sel = SelectedIndices();
    UINT on  = sel.empty() ? MF_GRAYED : MF_ENABLED;
    UINT one = sel.size() == 1 ? MF_ENABLED : MF_GRAYED;
    UINT any = g_tracks.empty() ? MF_GRAYED : MF_ENABLED;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | on, IDM_PLAY_SEL, WithKey(L"&Play", IDM_PLAY_SEL).c_str());
    SetMenuDefaultItem(m, IDM_PLAY_SEL, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_ADD_FILES, L"Add &Files...");
    AppendMenuW(m, MF_STRING, IDM_ADD_FOLDER, L"Add F&older...");
    AppendMenuW(m, MF_STRING, IDM_ADD_URL, L"Add &URL (web radio / podcast)...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | on, IDM_COPY_TITLE, L"Copy &title");
    AppendMenuW(m, MF_STRING | on, IDM_COPY_PATH, L"Copy file pat&h");
    AppendMenuW(m, MF_STRING | on, IDM_SHOW_EXPLORER, L"Show in &Explorer");
    AppendMenuW(m, MF_STRING | one, IDM_PROPS, L"Propert&ies...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP | any, (UINT_PTR)BuildSortMenu(), L"S&ort playlist");
    AppendMenuW(m, MF_STRING | any, IDM_SELALL, WithKey(L"Select &all", IDM_SELALL).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | on, IDM_REMOVE, WithKey(L"&Remove from playlist", IDM_REMOVE).c_str());
    AppendMenuW(m, MF_STRING | any, IDM_CLEAR, L"&Clear playlist");

    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) SendMessage(g_hwnd, WM_COMMAND, cmd, 0);
}

// ---- About dialog (text comes from Klyraudio.rc: VERSIONINFO + IDD_ABOUT) ----------
static std::wstring VerString(const wchar_t* key) {
    static std::vector<BYTE> data;
    static bool loaded = false;
    static WORD lang = 0x0409, cp = 0x04B0;
    if (!loaded) {
        loaded = true;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        DWORD dummy = 0, sz = GetFileVersionInfoSizeW(exe, &dummy);
        if (sz) {
            data.resize(sz);
            if (!GetFileVersionInfoW(exe, 0, sz, data.data())) data.clear();
            else {
                struct LangCp { WORD l, c; }* t = nullptr;
                UINT n = 0;
                if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&t, &n) && n >= sizeof(LangCp) && t) {
                    lang = t->l; cp = t->c;
                }
            }
        }
    }
    if (data.empty()) return L"";
    wchar_t sub[160];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", lang, cp, key);
    LPWSTR v = nullptr; UINT n = 0;
    if (VerQueryValueW(data.data(), sub, (LPVOID*)&v, &n) && v) return v;
    return L"";
}

static INT_PTR CALLBACK AboutProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG: {
        DarkTitle(h);
        HICON ic = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR);
        SendDlgItemMessageW(h, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)ic, 0);

        std::wstring name = VerString(L"ProductName");
        if (name.empty()) name = L"Klyraudio";
        std::wstring ver = VerString(L"ProductVersion");
        SetWindowTextW(h, (L"About " + name).c_str());
        SetDlgItemTextW(h, IDC_ABOUT_TITLE, name.c_str());
        SendDlgItemMessageW(h, IDC_ABOUT_TITLE, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
        if (!ver.empty()) SetDlgItemTextW(h, IDC_ABOUT_VER, (L"Version " + ver).c_str());
        std::wstring desc = VerString(L"FileDescription");
        if (!desc.empty()) SetDlgItemTextW(h, IDC_ABOUT_DESC, desc.c_str());
        std::wstring copy = VerString(L"LegalCopyright");
        if (!copy.empty()) SetDlgItemTextW(h, IDC_ABOUT_COPY, copy.c_str());
        return TRUE;
    }
    case WM_CTLCOLORDLG:
        return (INT_PTR)g_brBg;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        int id = GetDlgCtrlID((HWND)l);
        SetBkColor(dc, C_BG);
        SetTextColor(dc, id == IDC_ABOUT_TITLE ? C_AMBER : (id == IDC_ABOUT_VER || id == IDC_ABOUT_COPY || id == IDC_ABOUT_KEYS) ? C_MUTED : C_TEXT);
        return (INT_PTR)g_brBg;
    }
    case WM_DRAWITEM:
        DrawBtn((DRAWITEMSTRUCT*)l);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    case WM_DESTROY: {
        HICON ic = (HICON)SendDlgItemMessageW(h, IDC_ABOUT_ICON, STM_GETICON, 0, 0);
        if (ic) DestroyIcon(ic);
        break;
    }
    }
    return FALSE;
}

static void ShowAbout() {
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_ABOUT), g_hwnd, AboutProc, 0);
}

// ---- "Add web radio / podcast link" dialog -----------------------------------
static std::wstring g_urlInput;

static INT_PTR CALLBACK AddUrlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static HBRUSH brEdit = nullptr;
    switch (m) {
    case WM_INITDIALOG: {
        DarkTitle(h);
        brEdit = CreateSolidBrush(C_LCD);
        if (l == 1) {                                   // opened from the Podcasts window
            SetWindowTextW(h, L"Subscribe to a podcast");
            SetDlgItemTextW(h, IDC_URL_HELP, L"Paste a podcast RSS feed address, or type a podcast name to search the directory:");
            SetDlgItemTextW(h, IDC_URL_NOTE, L"Searching uses Apple's public podcast directory. Subscriptions are saved in your Podcasts library.");
        }
        SendDlgItemMessageW(h, IDC_URL_EDIT, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        SendDlgItemMessageW(h, IDC_URL_EDIT, EM_SETLIMITTEXT, 2000, 0);
        // pre-fill with a link from the clipboard, if there is one
        if (OpenClipboard(h)) {
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            if (hd) {
                const wchar_t* t = (const wchar_t*)GlobalLock(hd);
                if (t) {
                    std::wstring c(t, wcsnlen(t, 2000));
                    GlobalUnlock(hd);
                    while (!c.empty() && (c.back() <= L' ')) c.pop_back();
                    size_t a = 0; while (a < c.size() && c[a] <= L' ') a++;
                    c.erase(0, a);
                    if (IsUrl(c)) SetDlgItemTextW(h, IDC_URL_EDIT, c.c_str());
                }
            }
            CloseClipboard();
        }
        SendDlgItemMessageW(h, IDC_URL_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(h, IDC_URL_EDIT));
        return FALSE;                                   // we set the focus ourselves
    }
    case WM_CTLCOLORDLG:
        return (INT_PTR)g_brBg;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_BG); SetTextColor(dc, C_TEXT);
        return (INT_PTR)g_brBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_LCD); SetTextColor(dc, C_TEXT);
        return (INT_PTR)brEdit;
    }
    case WM_DRAWITEM:
        DrawBtn((DRAWITEMSTRUCT*)l);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t buf[2048] = {};
            GetDlgItemTextW(h, IDC_URL_EDIT, buf, 2047);
            g_urlInput = buf;
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    case WM_DESTROY:
        if (brEdit) { DeleteObject(brEdit); brEdit = nullptr; }
        break;
    }
    return FALSE;
}

// "icy://", "feed://", "pcast://" and bare "host:port/path" all become http(s) links.
static std::wstring NormalizeUserUrl(std::wstring s) {
    size_t a = 0; while (a < s.size() && s[a] <= L' ') a++;
    s.erase(0, a);
    while (!s.empty() && s.back() <= L' ') s.pop_back();
    if (s.empty()) return L"";
    for (const wchar_t* scheme : { L"icy://", L"feed://", L"itpc://", L"pcast://", L"podcast://" }) {
        size_t n = wcslen(scheme);
        if (_wcsnicmp(s.c_str(), scheme, n) == 0) { s = L"http://" + s.substr(n); break; }
    }
    if (s.find(L"://") == std::wstring::npos) s = L"http://" + s;
    return IsUrl(s) ? s : L"";
}

static void DoAddUrl() {
    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_ADDURL), g_hwnd, AddUrlProc, 0) != IDOK) return;
    std::wstring url = NormalizeUserUrl(g_urlInput);
    if (url.empty()) {
        MessageBoxW(g_hwnd, L"Please enter an http:// or https:// address.", L"Klyraudio", MB_ICONINFORMATION);
        return;
    }
    StartUrlFetch(url, true);
}

static void ApplyKeyBindings();

// ---- Keyboard shortcuts (View > Keyboard shortcuts) ----------------------------------------------------
// A key combination is captured by a standard hotkey control in a small dialog; the chord comes back as text.
struct KeyCapture { bool global = false; keymap::Chord chord; };

static INT_PTR CALLBACK KeyCaptureProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG: {
        DarkTitle(h);
        SetWindowLongPtrW(h, DWLP_USER, l);
        KeyCapture* kc = (KeyCapture*)l;
        SetWindowTextW(h, kc->global ? L"Global hotkey" : L"Window shortcut");
        SetDlgItemTextW(h, IDC_CAP_TEXT, kc->global
            ? L"Press the key combination for this global hotkey. It works from any program."
            : L"Press the key combination for this shortcut. It works while Klyraudio is focused.");
        SendDlgItemMessageW(h, IDC_CAP_KEY, HKM_SETRULES, 0, 0);     // accept plain keys too (Left, X, F5 ...)
        SendDlgItemMessageW(h, IDC_CAP_KEY, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        SetFocus(GetDlgItem(h, IDC_CAP_KEY));
        return FALSE;
    }
    case WM_CTLCOLORDLG:
        return (INT_PTR)g_brBg;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)w, C_BG); SetTextColor((HDC)w, C_TEXT);
        return (INT_PTR)g_brBg;
    case WM_DRAWITEM:
        DrawBtn((DRAWITEMSTRUCT*)l);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            KeyCapture* kc = (KeyCapture*)GetWindowLongPtrW(h, DWLP_USER);
            UINT hk = (UINT)SendDlgItemMessageW(h, IDC_CAP_KEY, HKM_GETHOTKEY, 0, 0);
            keymap::Chord c;
            c.vk = hk & 0xFF;
            BYTE hm = (BYTE)(hk >> 8);
            if (hm & HOTKEYF_CONTROL) c.mods |= keymap::kCtrl;
            if (hm & HOTKEYF_ALT)     c.mods |= keymap::kAlt;
            if (hm & HOTKEYF_SHIFT)   c.mods |= keymap::kShift;
            if (c.vk == 0 || keymap::Format(c).empty()) {
                MessageBoxW(h, L"Press a letter, digit, F-key, arrow, Enter, Delete or similar key.", L"Klyraudio", MB_ICONINFORMATION);
                return TRUE;
            }
            kc->chord = c;
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static std::vector<KeyBind> g_keyWork;     // the shortcuts dialog's working copy; copied to g_keys on OK

static void FillKeyList(HWND list) {
    ListView_DeleteAllItems(list);
    for (int i = 0; i < kNumKeyActions; i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT; it.iItem = i;
        it.pszText = (LPWSTR)kKeyActions[i].title;
        ListView_InsertItem(list, &it);
        const std::wstring& w = g_keyWork[i].window;
        ListView_SetItemText(list, i, 1, (LPWSTR)(w.empty() ? L"-" : w.c_str()));
        std::wstring g = g_keyWork[i].global.empty() ? L"-" : g_keyWork[i].global;
        bool failed = std::find(g_hotkeyFailed.begin(), g_hotkeyFailed.end(), i) != g_hotkeyFailed.end();
        if (failed && g_keyWork[i].global == g_keys[i].global) g += L"  (in use)";
        ListView_SetItemText(list, i, 2, (LPWSTR)g.c_str());
    }
}

static void SelectKeyRow(HWND list, int row) {
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, row, FALSE);
}

static INT_PTR CALLBACK ShortcutsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    HWND list = GetDlgItem(h, IDC_KEYS_LIST);
    switch (m) {
    case WM_INITDIALOG: {
        DarkTitle(h);
        g_keyWork = g_keys;
        SendMessageW(list, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(list, C_LCD);
        ListView_SetTextBkColor(list, C_LCD);
        ListView_SetTextColor(list, C_TEXT);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)L"Action";        col.cx = 120; ListView_InsertColumn(list, 0, &col);
        col.pszText = (LPWSTR)L"Window key";    col.cx = 85;  ListView_InsertColumn(list, 1, &col);
        col.pszText = (LPWSTR)L"Global hotkey"; col.cx = 115; ListView_InsertColumn(list, 2, &col);
        FillKeyList(list);
        SelectKeyRow(list, 0);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
        return (INT_PTR)g_brBg;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)w, C_BG); SetTextColor((HDC)w, C_TEXT);
        return (INT_PTR)g_brBg;
    case WM_DRAWITEM:
        DrawBtn((DRAWITEMSTRUCT*)l);
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(w);
        int sel = (int)ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (id == IDOK) {
            g_keys = g_keyWork;
            SaveSettings();
            ApplyKeyBindings();
            if (!g_hotkeyFailed.empty()) {
                std::wstring msg = L"These global hotkeys are already used by another program, so they won't work:\n";
                for (int i : g_hotkeyFailed) msg += L"\n" + std::wstring(kKeyActions[i].title) + L"  (" + g_keys[i].global + L")";
                MessageBoxW(h, msg.c_str(), L"Klyraudio", MB_ICONWARNING);
            }
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        if (id == IDC_KEYS_SET || id == IDC_KEYS_SETG) {
            if (sel < 0) { MessageBoxW(h, L"Select an action first.", L"Klyraudio", MB_ICONINFORMATION); return TRUE; }
            const bool global = id == IDC_KEYS_SETG;
            KeyCapture kc; kc.global = global;
            if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_KEYCAPTURE), h, KeyCaptureProc, (LPARAM)&kc) != IDOK) return TRUE;
            const std::wstring text = keymap::Format(kc.chord);

            // A chord can only do one job: whatever else holds it gives it up (a global hotkey also swallows the
            // window shortcut with the same keys, so the two kinds clash with each other too).
            std::vector<std::wstring> clashes;
            for (int j = 0; j < kNumKeyActions; j++) {
                bool inWin = g_keyWork[j].window == text && !(j == sel && !global);
                bool inGlo = g_keyWork[j].global == text && !(j == sel && global);
                if (inWin || inGlo) clashes.push_back(kKeyActions[j].title);
            }
            if (!clashes.empty()) {
                std::wstring msg = L"\"" + text + L"\" is already used by:\n";
                for (auto& t : clashes) msg += L"\n" + t;
                msg += L"\n\nUse it for \"" + std::wstring(kKeyActions[sel].title) + L"\" instead?";
                if (MessageBoxW(h, msg.c_str(), L"Klyraudio", MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
                for (int j = 0; j < kNumKeyActions; j++) {
                    if (g_keyWork[j].window == text && !(j == sel && !global)) g_keyWork[j].window.clear();
                    if (g_keyWork[j].global == text && !(j == sel && global)) g_keyWork[j].global.clear();
                }
            }
            if (global) g_keyWork[sel].global = text; else g_keyWork[sel].window = text;
            FillKeyList(list);
            SelectKeyRow(list, sel);
            return TRUE;
        }
        if (id == IDC_KEYS_CLEAR) {
            if (sel < 0) return TRUE;
            g_keyWork[sel].window.clear(); g_keyWork[sel].global.clear();
            FillKeyList(list);
            SelectKeyRow(list, sel);
            return TRUE;
        }
        if (id == IDC_KEYS_RESET) {
            if (MessageBoxW(h, L"Restore all keyboard shortcuts to their defaults?", L"Klyraudio",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
            g_keyWork = DefaultKeys();
            FillKeyList(list);
            SelectKeyRow(list, sel < 0 ? 0 : sel);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static void ShowShortcuts() {
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SHORTCUTS), g_hwnd, ShortcutsProc, 0);
}

// ============================================================================
//  Podcasts window: subscriptions, episode catalog, directory search
//
//  The library (pod::Catalog, see podcatalog.h) lives in the data folder (next to the .exe) as podcasts.dat and is only
//  touched on the UI thread. Network work (feed download, directory search) runs on background threads
//  and comes back as WM_APP_POD / WM_APP_PODSEARCH messages to the main window.
// ============================================================================
static long long NowUnix() { return (long long)time(nullptr); }

enum {
    IDC_POD_FEEDS = 4001, IDC_POD_EPS, IDC_POD_DESC, IDC_POD_STATUS, IDC_POD_LBL_FEEDS, IDC_POD_LBL_EPS,
    IDC_POD_SUB, IDC_POD_UNSUB, IDC_POD_REFRESH,
    IDC_POD_PLAY, IDC_POD_ENQ, IDC_POD_ENQNEW, IDC_POD_MARK, IDC_POD_UNMARK,
    IDC_POD_IMPORT, IDC_POD_EXPORT,
    // right-click menu only
    IDC_POD_M_REFRESH_ONE, IDC_POD_M_MARKALL, IDC_POD_M_UNMARKALL, IDC_POD_M_COPYFEED, IDC_POD_M_COPYEP
};

static pod::Catalog g_cat;
static bool g_catLoaded = false;
static HWND g_podFeeds = nullptr, g_podEps = nullptr, g_podDesc = nullptr, g_podStatus = nullptr;
static HBRUSH g_brLcd = nullptr;
static int g_podSel = -1;                       // index into g_cat.feeds of the selected subscription
static std::atomic<int> g_podBusy{0};           // feed downloads in flight
static std::atomic<bool> g_podSearching{false};
static int g_podFailed = 0;                     // feeds that failed in the current refresh

static std::wstring CatalogPath() { return AppDataDir() + L"\\podcasts.dat"; }

static void LoadCatalog() {
    if (g_catLoaded) return;
    g_catLoaded = true;
    std::ifstream f(fs::path(CatalogPath()), std::ios::binary);
    if (!f) return;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    pod::Deserialize(data, g_cat);
}

static void SaveCatalog() {
    std::string s = pod::Serialize(g_cat);
    std::wstring path = CatalogPath(), tmp = path + L".tmp";
    {
        std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(s.data(), (std::streamsize)s.size());
        if (!f) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);   // never leave a half-written library
}

// ---- background work ------------------------------------------------------------------------------------
struct PodResult {
    std::wstring url, error;
    feed::FeedInfo info;
    bool subscribe = false;
    bool silent = false;          // bulk import: count failures instead of showing a box per feed
};
struct PodSearchResult {
    std::vector<pod::SearchHit> hits;
    std::wstring error;
};

static void PodFetchThread(std::vector<std::wstring> urls, bool subscribe, bool silent, HWND hwnd) {
    for (auto& url : urls) {
        if (g_quitting) break;
        auto* r = new PodResult;
        r->url = url;
        r->subscribe = subscribe;
        r->silent = silent;
        try {
            std::wstring host = UrlHostW(url);
            HttpConn c;
            if (!HttpOpen(url, c)) {
                r->error = (c.err == ERROR_INVALID_PARAMETER)
                    ? std::wstring(L"That doesn't look like a valid web address.")
                    : std::wstring(L"Couldn't connect to ") + host + L".\n\nCheck the address and your internet connection.";
            } else if (c.status >= 400) {
                r->error = L"The server at " + host + L" answered with HTTP error " + std::to_wstring(c.status) + L".";
            } else {
                std::string body;
                HttpReadBody(c, 16 << 20, body);
                if (!feed::ParseFeedFull(body, r->info))
                    r->error = L"That address isn't a podcast feed with audio episodes.\n\n"
                               L"Use the podcast's RSS feed address, or search for it by name.";
            }
        } catch (...) {
            r->error = L"Something went wrong while reading the feed.";
        }
        if (g_quitting || !PostMessage(hwnd, WM_APP_POD, 0, (LPARAM)r)) delete r;
    }
}

static void PodSearchThread(std::wstring term, HWND hwnd) {
    auto* r = new PodSearchResult;
    try {
        std::wstring url = L"https://itunes.apple.com/search?media=podcast&entity=podcast&limit=25&term=" +
                           Utf8ToW(pod::UrlEncode(WToUtf8(term)));
        HttpConn c;
        if (!HttpOpen(url, c)) {
            r->error = L"Couldn't reach the podcast directory.\n\n"
                       L"Check your internet connection, or paste the podcast's RSS feed address instead.";
        } else if (c.status >= 400) {
            r->error = L"The podcast directory answered with HTTP error " + std::to_wstring(c.status) + L".";
        } else {
            std::string body;
            HttpReadBody(c, 2 << 20, body);
            r->hits = pod::ParseSearch(body);
            if (r->hits.empty()) r->error = L"No podcasts found for \"" + term + L"\".";
        }
    } catch (...) {
        r->error = L"Something went wrong while searching.";
    }
    if (g_quitting || !PostMessage(hwnd, WM_APP_PODSEARCH, 0, (LPARAM)r)) delete r;
}

// ---- window helpers ---------------------------------------------------------------------------------------
static void PodSetStatus() {
    if (!g_podStatus) return;
    std::wstring s;
    if (g_podSearching) s = L"Searching the podcast directory...";
    else if (g_podBusy > 0) s = L"Downloading feeds... (" + std::to_wstring(g_podBusy.load()) + L" left)";
    else if (g_podFailed > 0) s = std::to_wstring(g_podFailed) + L" feed(s) could not be refreshed";
    else {
        int n = 0;
        for (auto& f : g_cat.feeds) n += pod::CountUnplayed(f);
        s = std::to_wstring(g_cat.feeds.size()) + (g_cat.feeds.size() == 1 ? L" subscription" : L" subscriptions") +
            L"  •  " + std::to_wstring(n) + L" unplayed";
    }
    SetWindowTextW(g_podStatus, s.c_str());
}

static bool PodHasSel() { return g_podSel >= 0 && g_podSel < (int)g_cat.feeds.size(); }

static void PodFillFeeds() {
    if (!g_podFeeds) return;
    SendMessage(g_podFeeds, WM_SETREDRAW, FALSE, 0);
    SendMessage(g_podFeeds, LB_RESETCONTENT, 0, 0);
    for (auto& f : g_cat.feeds) {
        std::wstring t = Utf8ToW(f.title.empty() ? f.url : f.title);
        int n = pod::CountUnplayed(f);
        if (n > 0) t += L"  (" + std::to_wstring(n) + L")";
        SendMessageW(g_podFeeds, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    if (g_podSel >= (int)g_cat.feeds.size()) g_podSel = (int)g_cat.feeds.size() - 1;
    if (g_podSel < 0 && !g_cat.feeds.empty()) g_podSel = 0;
    if (g_podSel >= 0) SendMessage(g_podFeeds, LB_SETCURSEL, (WPARAM)g_podSel, 0);
    SendMessage(g_podFeeds, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_podFeeds, nullptr, TRUE);
}

static std::vector<int> PodSelectedEpisodes() {
    std::vector<int> v;
    if (!g_podEps || !PodHasSel()) return v;
    int n = (int)g_cat.feeds[g_podSel].eps.size();
    int i = -1;
    while ((i = ListView_GetNextItem(g_podEps, i, LVNI_SELECTED)) != -1)
        if (i < n) v.push_back(i);
    return v;
}

// Text under the episode list: the selected episode's description, else the show's.
static void PodShowDesc() {
    if (!g_podDesc) return;
    std::wstring t;
    if (PodHasSel()) {
        const pod::Feed& f = g_cat.feeds[g_podSel];
        std::vector<int> sel = PodSelectedEpisodes();
        if (!sel.empty()) {
            const pod::Episode& e = f.eps[sel[0]];
            t = Utf8ToW(e.title) + L"\r\n" + Utf8ToW(e.desc.empty() ? std::string("(no description)") : e.desc);
        } else {
            t = Utf8ToW(f.title.empty() ? f.url : f.title);
            if (!f.author.empty()) t += L"  —  " + Utf8ToW(f.author);
            if (!f.desc.empty()) t += L"\r\n" + Utf8ToW(f.desc);
        }
    } else {
        t = L"No subscriptions yet. Click \"Subscribe...\" and paste a podcast RSS feed address, or type a podcast name to search.";
    }
    SetWindowTextW(g_podDesc, t.c_str());
}

// Reload the episode list for the selected subscription (clears the episode selection).
static void PodFillEpisodes() {
    if (!g_podEps) return;
    int n = PodHasSel() ? (int)g_cat.feeds[g_podSel].eps.size() : 0;
    ListView_SetItemCountEx(g_podEps, n, 0);
    ListView_SetItemState(g_podEps, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    InvalidateRect(g_podEps, nullptr, FALSE);
    PodShowDesc();
}

static void PodRefreshViews() {          // after played flags changed: labels + list, keep the selection
    PodFillFeeds();
    if (g_podEps) InvalidateRect(g_podEps, nullptr, FALSE);
    PodSetStatus();
}

static void PodStartFetch(const std::vector<std::wstring>& urls, bool subscribe, bool silent = false) {
    if (urls.empty()) return;
    if (g_podBusy.load() == 0) g_podFailed = 0;
    g_podBusy += (int)urls.size();
    PodSetStatus();
    std::thread(PodFetchThread, urls, subscribe, silent, g_hwnd).detach();
}

static void PodApply(PodResult* r) {     // UI thread; takes ownership of r
    --g_podBusy;
    LoadCatalog();
    bool subscribe = r->subscribe, silent = r->silent;
    std::wstring err = r->error;
    int touched = -1;
    if (err.empty()) {
        std::string u8 = WToUtf8(r->url);
        int idx = pod::FindFeed(g_cat, u8);
        if (idx < 0 && subscribe) {
            pod::Feed nf;
            nf.url = u8;
            g_cat.feeds.push_back(nf);
            idx = (int)g_cat.feeds.size() - 1;
        }
        if (idx >= 0) {
            pod::Merge(g_cat.feeds[idx], r->info, NowUnix());
            SaveCatalog();
            touched = idx;
            if (subscribe) g_podSel = idx;
        }
    } else if (!subscribe || silent) {
        ++g_podFailed;
    }
    delete r;
    if (g_podWnd) {
        PodFillFeeds();
        if (touched >= 0 && touched == g_podSel) PodFillEpisodes();
        PodSetStatus();
    }
    if (!err.empty() && subscribe && !silent)
        MessageBoxW(g_podWnd && IsWindowVisible(g_podWnd) ? g_podWnd : g_hwnd, err.c_str(), L"Klyraudio", MB_ICONWARNING);
}

// Marks an episode as played when the player starts it (from the Podcasts window or from the playlist).
static void PodOnTrackStart(const std::wstring& path) {
    if (!IsUrl(path)) return;
    LoadCatalog();
    if (g_cat.feeds.empty()) return;
    std::string u = WToUtf8(path);
    bool changed = false;
    for (auto& f : g_cat.feeds)
        for (auto& e : f.eps)
            if (!e.played && e.url == u) { e.played = true; changed = true; }
    if (!changed) return;
    SaveCatalog();
    if (g_podWnd) PodRefreshViews();
}

// ---- picking a search result ---------------------------------------------------------------------------
static const std::vector<pod::SearchHit>* g_pickHits = nullptr;
static int g_pickSel = -1;

static INT_PTR CALLBACK PodPickProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static HBRUSH brList = nullptr;
    switch (m) {
    case WM_INITDIALOG: {
        DarkTitle(h);
        brList = CreateSolidBrush(C_LCD);
        HWND lb = GetDlgItem(h, IDC_POD_PICKLIST);
        SendMessage(lb, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        if (g_pickHits)
            for (auto& hit : *g_pickHits) {
                std::wstring t = Utf8ToW(hit.name);
                if (!hit.artist.empty()) t += L"  —  " + Utf8ToW(hit.artist);
                SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t.c_str());
            }
        SendMessage(lb, LB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
        return (INT_PTR)g_brBg;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_BG); SetTextColor(dc, C_TEXT);
        return (INT_PTR)g_brBg;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_LCD); SetTextColor(dc, C_TEXT);
        return (INT_PTR)brList;
    }
    case WM_DRAWITEM:
        DrawBtn((DRAWITEMSTRUCT*)l);
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_POD_PICKLIST && HIWORD(w) == LBN_DBLCLK) id = IDOK;
        if (id == IDOK) {
            g_pickSel = (int)SendDlgItemMessageW(h, IDC_POD_PICKLIST, LB_GETCURSEL, 0, 0);
            EndDialog(h, g_pickSel >= 0 ? IDOK : IDCANCEL);
            return TRUE;
        }
        if (id == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    case WM_DESTROY:
        if (brList) { DeleteObject(brList); brList = nullptr; }
        break;
    }
    return FALSE;
}

static void PodOnSearch(PodSearchResult* r) {     // UI thread; takes ownership of r
    g_podSearching = false;
    PodSetStatus();
    HWND owner = (g_podWnd && IsWindowVisible(g_podWnd)) ? g_podWnd : g_hwnd;
    if (!r->error.empty()) {
        MessageBoxW(owner, r->error.c_str(), L"Klyraudio", MB_ICONINFORMATION);
        delete r;
        return;
    }
    g_pickHits = &r->hits;
    g_pickSel = -1;
    INT_PTR rc = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PODPICK), owner, PodPickProc, 0);
    g_pickHits = nullptr;
    std::wstring url;
    if (rc == IDOK && g_pickSel >= 0 && g_pickSel < (int)r->hits.size()) url = Utf8ToW(r->hits[g_pickSel].feedUrl);
    delete r;
    if (!url.empty()) PodStartFetch({ url }, true);
}

// ---- button actions ----------------------------------------------------------------------------------------
static void PodSubscribe() {
    HWND owner = g_podWnd ? g_podWnd : g_hwnd;
    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_ADDURL), owner, AddUrlProc, 1) != IDOK) return;
    std::wstring in = g_urlInput;
    size_t a = 0; while (a < in.size() && in[a] <= L' ') a++;
    in.erase(0, a);
    while (!in.empty() && in.back() <= L' ') in.pop_back();
    if (in.empty()) return;
    bool looksLikeUrl = in.find(L' ') == std::wstring::npos && (in.find(L"://") != std::wstring::npos || in.find(L'.') != std::wstring::npos);
    if (looksLikeUrl) {
        std::wstring url = NormalizeUserUrl(in);
        if (url.empty()) { MessageBoxW(owner, L"Please enter an http:// or https:// address.", L"Klyraudio", MB_ICONINFORMATION); return; }
        PodStartFetch({ url }, true);
    } else {
        g_podSearching = true;
        PodSetStatus();
        std::thread(PodSearchThread, in, g_hwnd).detach();
    }
}

static void PodUnsubscribe() {
    if (!PodHasSel()) return;
    std::wstring t = Utf8ToW(g_cat.feeds[g_podSel].title.empty() ? g_cat.feeds[g_podSel].url : g_cat.feeds[g_podSel].title);
    std::wstring q = L"Unsubscribe from \"" + t + L"\"?\n\nIts episode list and played history are removed from the library.";
    if (MessageBoxW(g_podWnd, q.c_str(), L"Klyraudio", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_cat.feeds.erase(g_cat.feeds.begin() + g_podSel);
    SaveCatalog();
    PodFillFeeds();
    PodFillEpisodes();
    PodSetStatus();
}

static void PodRefreshAll() {
    std::vector<std::wstring> urls;
    for (auto& f : g_cat.feeds) urls.push_back(Utf8ToW(f.url));
    PodStartFetch(urls, false);
}

// ---- OPML: move a subscription list in or out (every podcast app can read and write it) ------------
static void PodImportOpml() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_podWnd;
    ofn.lpstrFilter = L"OPML subscription lists (*.opml;*.xml)\0*.opml;*.xml\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string data;
    {
        std::ifstream f(fs::path(file), std::ios::binary);
        if (!f) { MessageBoxW(g_podWnd, L"Could not read that file.", L"Klyraudio", MB_ICONWARNING); return; }
        data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    std::vector<pod::OpmlEntry> entries = pod::ParseOpml(data);
    if (entries.empty()) {
        MessageBoxW(g_podWnd, L"That file doesn't contain any podcast subscriptions.", L"Klyraudio", MB_ICONINFORMATION);
        return;
    }
    std::vector<std::wstring> urls;
    for (auto& e : entries) {
        std::wstring u = NormalizeUserUrl(Utf8ToW(e.url));
        if (u.empty()) continue;
        if (pod::FindFeed(g_cat, WToUtf8(u)) >= 0) continue;                 // already subscribed
        if (std::find(urls.begin(), urls.end(), u) != urls.end()) continue;  // listed twice in the file
        urls.push_back(u);
    }
    if (urls.empty()) {
        MessageBoxW(g_podWnd, L"Every podcast in that file is already in your library.", L"Klyraudio", MB_ICONINFORMATION);
        return;
    }
    PodStartFetch(urls, true, true);
}

static void PodExportOpml() {
    if (g_cat.feeds.empty()) {
        MessageBoxW(g_podWnd, L"There are no subscriptions to export yet.", L"Klyraudio", MB_ICONINFORMATION);
        return;
    }
    wchar_t file[MAX_PATH] = L"klyraudio-podcasts.opml";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_podWnd;
    ofn.lpstrFilter = L"OPML subscription list (*.opml)\0*.opml\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"opml";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return;
    std::string s = pod::BuildOpml(g_cat);
    std::ofstream f(fs::path(file), std::ios::binary | std::ios::trunc);
    f.write(s.data(), (std::streamsize)s.size());
    f.close();
    if (!f) MessageBoxW(g_podWnd, L"Could not write the file.", L"Klyraudio", MB_ICONWARNING);
}

// Adds episodes to the main playlist; optionally starts the first one. (The player marks it played.)
static void PodEnqueue(const std::vector<int>& idx, bool playFirst) {
    if (!PodHasSel() || idx.empty()) return;
    const pod::Feed& f = g_cat.feeds[g_podSel];
    size_t before = g_tracks.size();
    MetaList meta;
    std::wstring ftitle = Utf8ToW(f.title.empty() ? f.url : f.title);
    for (int i : idx) {
        if (i < 0 || i >= (int)f.eps.size()) continue;
        const pod::Episode& e = f.eps[i];
        AddTrack(Utf8ToW(e.url), ftitle + L" - " + Utf8ToW(e.title), e.dur, meta);
    }
    if (g_tracks.size() == before) return;
    RefreshList();
    if (playFirst) PlayIndex((int)before);
    else UpdateUi();
}

static void PodQueueUnplayed() {         // oldest first, so a series plays in release order
    if (!PodHasSel()) return;
    const pod::Feed& f = g_cat.feeds[g_podSel];
    std::vector<int> idx;
    for (int i = (int)f.eps.size() - 1; i >= 0; i--)
        if (!f.eps[i].played) idx.push_back(i);
    if (idx.empty()) { MessageBoxW(g_podWnd, L"No unplayed episodes in this show.", L"Klyraudio", MB_ICONINFORMATION); return; }
    PodEnqueue(idx, false);
}

static void PodMark(bool played) {
    if (!PodHasSel()) return;
    pod::Feed& f = g_cat.feeds[g_podSel];
    for (int i : PodSelectedEpisodes()) f.eps[i].played = played;
    SaveCatalog();
    PodRefreshViews();
}

static void PodMarkAll(bool played) {    // every episode of the selected show
    if (!PodHasSel()) return;
    for (auto& e : g_cat.feeds[g_podSel].eps) e.played = played;
    SaveCatalog();
    PodRefreshViews();
}

static void PodRefreshOne() {
    if (!PodHasSel()) return;
    PodStartFetch({ Utf8ToW(g_cat.feeds[g_podSel].url) }, false);
}

static void PodCopyFeedLink() {
    if (PodHasSel()) CopyText(Utf8ToW(g_cat.feeds[g_podSel].url));
}

static void PodCopyEpisodeLink() {
    if (!PodHasSel()) return;
    std::wstring out;
    for (int i : PodSelectedEpisodes()) {
        if (!out.empty()) out += L"\r\n";
        out += Utf8ToW(g_cat.feeds[g_podSel].eps[i].url);
    }
    if (!out.empty()) CopyText(out);
}

// Right-click (or the menu key) on the subscriptions list or the episodes list. Returns false when the click
// was somewhere else so the default handling can run. The row under the cursor is selected first, like Explorer.
static bool PodContextMenu(HWND src, int sx, int sy) {
    const bool onEps = (src == g_podEps), onFeeds = (src == g_podFeeds);
    if (!onEps && !onFeeds) return false;

    POINT pt = { sx, sy };
    if (sx == -1 && sy == -1) {                      // keyboard: open next to the selected row
        RECT rc{};
        bool have = false;
        if (onEps) {
            int i = ListView_GetNextItem(g_podEps, -1, LVNI_SELECTED);
            have = i >= 0 && ListView_GetItemRect(g_podEps, i, &rc, LVIR_BOUNDS);
        } else {
            int s = (int)SendMessage(g_podFeeds, LB_GETCURSEL, 0, 0);
            have = s != LB_ERR && SendMessage(g_podFeeds, LB_GETITEMRECT, (WPARAM)s, (LPARAM)&rc) != LB_ERR;
        }
        pt.x = have ? rc.left + 24 : 8;
        pt.y = have ? rc.bottom : 8;
        ClientToScreen(src, &pt);
    } else {
        POINT cp = pt;
        ScreenToClient(src, &cp);
        if (onEps) {
            LVHITTESTINFO hi{};
            hi.pt = cp;
            int i = ListView_HitTest(g_podEps, &hi);
            if (i >= 0) {
                if (!(ListView_GetItemState(g_podEps, i, LVIS_SELECTED) & LVIS_SELECTED)) {   // keep a multi-selection intact
                    ListView_SetItemState(g_podEps, -1, 0, LVIS_SELECTED);
                    ListView_SetItemState(g_podEps, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
            } else {
                ListView_SetItemState(g_podEps, -1, 0, LVIS_SELECTED);
            }
        } else {
            LRESULT r = SendMessage(g_podFeeds, LB_ITEMFROMPOINT, 0, MAKELPARAM(cp.x, cp.y));
            int i = (int)LOWORD(r);
            if (HIWORD(r) == 0 && i >= 0 && i < (int)g_cat.feeds.size() && i != g_podSel) {
                SendMessage(g_podFeeds, LB_SETCURSEL, (WPARAM)i, 0);
                g_podSel = i;
                PodFillEpisodes();
            }
        }
    }

    const bool hasFeed = PodHasSel();
    const bool hasEp = hasFeed && !PodSelectedEpisodes().empty();
    const UINT f = hasFeed ? MF_ENABLED : MF_GRAYED;
    const UINT e = hasEp ? MF_ENABLED : MF_GRAYED;

    HMENU m = CreatePopupMenu();
    if (onEps) {
        AppendMenuW(m, MF_STRING | e, IDC_POD_PLAY, L"&Play\tEnter");
        SetMenuDefaultItem(m, IDC_POD_PLAY, FALSE);
        AppendMenuW(m, MF_STRING | e, IDC_POD_ENQ, L"Add to &playlist");
        AppendMenuW(m, MF_STRING | f, IDC_POD_ENQNEW, L"&Queue all unplayed");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | e, IDC_POD_MARK, L"Mark as pla&yed");
        AppendMenuW(m, MF_STRING | e, IDC_POD_UNMARK, L"Mark as &unplayed");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | e, IDC_POD_M_COPYEP, L"Copy episode &link");
    } else {
        AppendMenuW(m, MF_STRING | f, IDC_POD_M_REFRESH_ONE, L"&Refresh this podcast");
        AppendMenuW(m, MF_STRING | f, IDC_POD_ENQNEW, L"&Queue unplayed episodes");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | f, IDC_POD_M_MARKALL, L"Mark all as pla&yed");
        AppendMenuW(m, MF_STRING | f, IDC_POD_M_UNMARKALL, L"Mark all as &unplayed");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | f, IDC_POD_M_COPYFEED, L"Copy feed &link");
        AppendMenuW(m, MF_STRING | f, IDC_POD_UNSUB, L"U&nsubscribe...");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDC_POD_SUB, L"&Subscribe...");
        AppendMenuW(m, MF_STRING, IDC_POD_REFRESH, L"Refresh &all");
        AppendMenuW(m, MF_STRING, IDC_POD_IMPORT, L"&Import OPML...");
        AppendMenuW(m, MF_STRING, IDC_POD_EXPORT, L"E&xport OPML...");
    }
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_podWnd, nullptr);
    DestroyMenu(m);
    if (cmd && g_podWnd) SendMessage(g_podWnd, WM_COMMAND, cmd, 0);
    return true;
}

// ---- the window ------------------------------------------------------------------------------------------------
static void PodLayout(int W, int H) {
    if (!g_podWnd || !g_podFeeds) return;
    const int lw = 230, gap = 8, bh = 26, descH = 92;
    int rx = 8 + lw + gap, rw = std::max(200, W - rx - 8);
    int by = H - 8 - bh;
    MoveWindow(GetDlgItem(g_podWnd, IDC_POD_LBL_FEEDS), 8, 8, lw, 16, TRUE);
    MoveWindow(g_podFeeds, 8, 28, lw, std::max(40, H - 28 - 8 - bh - 6 - (bh + 4)), TRUE);
    MoveWindow(GetDlgItem(g_podWnd, IDC_POD_IMPORT), 8,       by - bh - 4, 113, bh, TRUE);
    MoveWindow(GetDlgItem(g_podWnd, IDC_POD_EXPORT), 8 + 117, by - bh - 4, 113, bh, TRUE);
    MoveWindow(GetDlgItem(g_podWnd, IDC_POD_SUB),     8,       by, 80, bh, TRUE);
    MoveWindow(GetDlgItem(g_podWnd, IDC_POD_UNSUB),   8 + 84,  by, 76, bh, TRUE);
    MoveWindow(GetDlgItem(g_podWnd, IDC_POD_REFRESH), 8 + 164, by, 66, bh, TRUE);

    MoveWindow(GetDlgItem(g_podWnd, IDC_POD_LBL_EPS), rx, 8, 80, 16, TRUE);
    MoveWindow(g_podStatus, rx + 84, 8, std::max(40, rw - 84), 16, TRUE);
    int listH = std::max(60, H - 28 - 8 - bh - 6 - descH - 6);
    MoveWindow(g_podEps, rx, 28, rw, listH, TRUE);
    MoveWindow(g_podDesc, rx, 28 + listH + 6, rw, descH, TRUE);
    struct B { int id, w; };
    const B bs[] = { {IDC_POD_PLAY, 70}, {IDC_POD_ENQ, 120}, {IDC_POD_ENQNEW, 110}, {IDC_POD_MARK, 100}, {IDC_POD_UNMARK, 110} };
    int x = rx;
    for (auto& b : bs) { MoveWindow(GetDlgItem(g_podWnd, b.id), x, by, b.w, bh, TRUE); x += b.w + 4; }

    const int cDate = 84, cLen = 64, cState = 62;
    ListView_SetColumnWidth(g_podEps, 0, std::max(120, rw - cDate - cLen - cState - GetSystemMetrics(SM_CXVSCROLL) - 6));
    ListView_SetColumnWidth(g_podEps, 1, cDate);
    ListView_SetColumnWidth(g_podEps, 2, cLen);
    ListView_SetColumnWidth(g_podEps, 3, cState);
}

static LRESULT CALLBACK PodProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_brLcd = CreateSolidBrush(C_LCD);
        auto stc = [&](int id, const wchar_t* txt, DWORD style) {
            return CreateWindowW(L"STATIC", txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        };
        auto btn = [&](int id, const wchar_t* txt) {
            CreateWindowW(L"BUTTON", txt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        };
        stc(IDC_POD_LBL_FEEDS, L"Subscriptions", 0);
        stc(IDC_POD_LBL_EPS, L"Episodes", 0);
        g_podStatus = stc(IDC_POD_STATUS, L"", SS_RIGHT);

        g_podFeeds = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_POD_FEEDS, g_hInst, nullptr);
        SetWindowTheme(g_podFeeds, L"DarkMode_Explorer", nullptr);

        g_podEps = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
                                   0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_POD_EPS, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(g_podEps, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(g_podEps, C_BG);
        ListView_SetTextBkColor(g_podEps, C_BG);
        ListView_SetTextColor(g_podEps, C_TEXT);
        SetWindowTheme(g_podEps, L"DarkMode_Explorer", nullptr);
        const wchar_t* heads[] = { L"Episode", L"Date", L"Length", L"State" };
        for (int i = 0; i < 4; i++) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.fmt = LVCFMT_LEFT;
            col.cx = 100;
            col.pszText = (LPWSTR)heads[i];
            ListView_InsertColumn(g_podEps, i, &col);
        }

        g_podDesc = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_POD_DESC, g_hInst, nullptr);

        btn(IDC_POD_SUB, L"Subscribe...");
        btn(IDC_POD_UNSUB, L"Unsubscribe");
        btn(IDC_POD_REFRESH, L"Refresh all");
        btn(IDC_POD_PLAY, L"Play");
        btn(IDC_POD_ENQ, L"Add to playlist");
        btn(IDC_POD_ENQNEW, L"Queue unplayed");
        btn(IDC_POD_MARK, L"Mark played");
        btn(IDC_POD_UNMARK, L"Mark unplayed");
        btn(IDC_POD_IMPORT, L"Import OPML...");
        btn(IDC_POD_EXPORT, L"Export OPML...");
        EnumChildWindows(h, [](HWND c, LPARAM f) -> BOOL { SendMessage(c, WM_SETFONT, f, TRUE); return TRUE; }, (LPARAM)g_fontUI);
        return 0;
    }
    case WM_SIZE:
        PodLayout(LOWORD(l), HIWORD(l));
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = 820; mm->ptMinTrackSize.y = 420;
        return 0;
    }
    case WM_CTLCOLORDLG:
        return (LRESULT)g_brBg;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        if ((HWND)l == g_podDesc) {                       // read-only edit boxes report as "static"
            SetBkColor(dc, C_LCD); SetTextColor(dc, C_TEXT);
            return (LRESULT)g_brLcd;
        }
        SetBkColor(dc, C_BG); SetTextColor(dc, C_MUTED);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_LCD); SetTextColor(dc, C_TEXT);
        return (LRESULT)g_brLcd;
    }
    case WM_DRAWITEM:
        DrawBtn((DRAWITEMSTRUCT*)l);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->idFrom != IDC_POD_EPS) break;
        int code = (int)nh->code;
        if (code == LVN_GETDISPINFOW) {
            NMLVDISPINFOW* di = (NMLVDISPINFOW*)l;
            if (di->item.mask & LVIF_TEXT) {
                static std::wstring buf;
                buf.clear();
                int i = di->item.iItem;
                if (PodHasSel() && i >= 0 && i < (int)g_cat.feeds[g_podSel].eps.size()) {
                    const pod::Episode& e = g_cat.feeds[g_podSel].eps[i];
                    switch (di->item.iSubItem) {
                    case 0: buf = Utf8ToW(e.title); break;
                    case 1: buf = Utf8ToW(feed::FormatDate(e.pub)); break;
                    case 2: if (e.dur > 0) buf = FmtTime(e.dur); break;
                    case 3: buf = e.played ? L"played" : L"new"; break;
                    }
                }
                di->item.pszText = (LPWSTR)buf.c_str();
            }
            return 0;
        }
        if (code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int i = (int)cd->nmcd.dwItemSpec;
                bool played = PodHasSel() && i >= 0 && i < (int)g_cat.feeds[g_podSel].eps.size() && g_cat.feeds[g_podSel].eps[i].played;
                cd->clrText = played ? C_MUTED : C_TEXT;
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        if (code == LVN_ITEMCHANGED) {
            if (((NMLISTVIEW*)l)->uChanged & LVIF_STATE) PodShowDesc();
            return 0;
        }
        if (code == NM_DBLCLK) { PodEnqueue(PodSelectedEpisodes(), true); return 0; }
        if (code == LVN_KEYDOWN) {
            if (((NMLVKEYDOWN*)l)->wVKey == VK_RETURN) PodEnqueue(PodSelectedEpisodes(), true);
            return 0;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        switch (id) {
        case IDC_POD_FEEDS:
            if (code == LBN_SELCHANGE) {
                int s = (int)SendMessage(g_podFeeds, LB_GETCURSEL, 0, 0);
                if (s != LB_ERR && s != g_podSel) { g_podSel = s; PodFillEpisodes(); }
            }
            return 0;
        case IDC_POD_SUB:     PodSubscribe(); return 0;
        case IDC_POD_UNSUB:   PodUnsubscribe(); return 0;
        case IDC_POD_REFRESH: PodRefreshAll(); return 0;
        case IDC_POD_PLAY:    PodEnqueue(PodSelectedEpisodes(), true); return 0;
        case IDC_POD_ENQ:     PodEnqueue(PodSelectedEpisodes(), false); return 0;
        case IDC_POD_ENQNEW:  PodQueueUnplayed(); return 0;
        case IDC_POD_MARK:    PodMark(true); return 0;
        case IDC_POD_UNMARK:  PodMark(false); return 0;
        case IDC_POD_IMPORT:  PodImportOpml(); return 0;
        case IDC_POD_EXPORT:  PodExportOpml(); return 0;
        case IDC_POD_M_REFRESH_ONE: PodRefreshOne(); return 0;
        case IDC_POD_M_MARKALL:     PodMarkAll(true); return 0;
        case IDC_POD_M_UNMARKALL:   PodMarkAll(false); return 0;
        case IDC_POD_M_COPYFEED:    PodCopyFeedLink(); return 0;
        case IDC_POD_M_COPYEP:      PodCopyEpisodeLink(); return 0;
        }
        break;
    }
    case WM_CONTEXTMENU:
        if (PodContextMenu((HWND)w, (short)LOWORD(l), (short)HIWORD(l))) return 0;
        break;
    case WM_ENTERSIZEMOVE: DockBegin(h); return 0;
    case WM_MOVING: DockMoving(h, (RECT*)l); return TRUE;
    case WM_EXITSIZEMOVE: DockEnd(); return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (g_brLcd) { DeleteObject(g_brLcd); g_brLcd = nullptr; }
        g_podWnd = nullptr; g_podFeeds = g_podEps = g_podDesc = g_podStatus = nullptr;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowPodcasts() {
    LoadCatalog();
    if (!g_podWnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PodProc; wc.hInstance = g_hInst; wc.lpszClassName = L"KLYRAUDIO_POD";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = g_brBg;
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        RECT r = { 0, 0, 900, 560 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        RECT mr; GetWindowRect(g_hwnd, &mr);
        // A top-level window of its own (no owner): its own taskbar button, it stays open when the player
        // is minimised, and nothing in it is part of the playlist window.
        HWND w = CreateWindowExW(WS_EX_APPWINDOW, L"KLYRAUDIO_POD", L"Klyraudio Podcasts", WS_OVERLAPPEDWINDOW,
                                 mr.left + 40, mr.top + 40, r.right - r.left, r.bottom - r.top,
                                 nullptr, nullptr, g_hInst, nullptr);
        if (!w) return;
        g_podWnd = w;
        DarkTitle(g_podWnd);
        RECT cr; GetClientRect(g_podWnd, &cr);
        PodLayout(cr.right, cr.bottom);
        PodFillFeeds();
        PodFillEpisodes();
        PodSetStatus();
        ShowWindow(g_podWnd, SW_SHOW);
        // bring subscriptions that haven't been updated for a few hours up to date
        std::vector<std::wstring> stale;
        long long now = NowUnix();
        for (auto& f : g_cat.feeds)
            if (now - f.lastRefresh > 3 * 3600) stale.push_back(Utf8ToW(f.url));
        PodStartFetch(stale, false);
        return;
    }
    if (IsIconic(g_podWnd)) ShowWindow(g_podWnd, SW_RESTORE);
    ShowWindow(g_podWnd, SW_SHOW);
    SetForegroundWindow(g_podWnd);
}

static HMENU g_playMenu = nullptr, g_fadeMenu = nullptr;   // Playback and Crossfade popups (for check marks)

static HMENU BuildMenu() {
    HMENU mb = CreateMenu();
    HMENU f = CreatePopupMenu();
    AppendMenuW(f, MF_STRING, IDM_ADD_FILES, L"Add &Files...");
    AppendMenuW(f, MF_STRING, IDM_ADD_FOLDER, L"Add F&older...");
    AppendMenuW(f, MF_STRING, IDM_ADD_URL, L"Add &URL (web radio / podcast)...");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, IDM_OPEN_PL, L"&Open Playlist...");
    AppendMenuW(f, MF_STRING, IDM_SAVE_PL, L"&Save Playlist (.klyr)...");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)f, L"&File");

    HMENU p = CreatePopupMenu();
    AppendMenuW(p, MF_STRING, IDM_PLAY, L"&Play");
    AppendMenuW(p, MF_STRING, IDM_PAUSE, L"Pa&use / Resume");
    AppendMenuW(p, MF_STRING, IDM_STOP, L"&Stop");
    AppendMenuW(p, MF_STRING, IDM_PREV, L"Pre&vious");
    AppendMenuW(p, MF_STRING, IDM_NEXT, L"&Next");
    AppendMenuW(p, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(p, MF_STRING, IDM_SEEK_BACK, L"Back 5 sec");
    AppendMenuW(p, MF_STRING, IDM_SEEK_FWD, L"Forward 5 sec");
    AppendMenuW(p, MF_STRING, IDM_VOL_UP, L"Volume up");
    AppendMenuW(p, MF_STRING, IDM_VOL_DN, L"Volume down");
    AppendMenuW(p, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(p, MF_STRING, IDM_SHUFFLE, L"Shuffle");
    AppendMenuW(p, MF_STRING, IDM_REPEAT, L"Repeat (off / all / one)");
    AppendMenuW(p, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(p, MF_STRING, IDM_GAPLESS, L"&Gapless playback");
    g_fadeMenu = CreatePopupMenu();
    static const wchar_t* fadeTxt[7] = { L"Off", L"1 second", L"2 seconds", L"3 seconds",
                                         L"5 seconds", L"8 seconds", L"12 seconds" };
    for (int i = 0; i < 7; i++) AppendMenuW(g_fadeMenu, MF_STRING, IDM_FADE_0 + i, fadeTxt[i]);
    AppendMenuW(p, MF_POPUP, (UINT_PTR)g_fadeMenu, L"&Crossfade");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)p, L"&Playback");
    g_playMenu = p;

    HMENU l = CreatePopupMenu();
    AppendMenuW(l, MF_STRING, IDM_REMOVE, L"&Remove Selected");
    AppendMenuW(l, MF_STRING, IDM_CLEAR, L"&Clear Playlist");
    AppendMenuW(l, MF_STRING, IDM_SELALL, L"Select &All");
    AppendMenuW(l, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(l, MF_POPUP, (UINT_PTR)BuildSortMenu(), L"S&ort");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)l, L"&Playlist");

    HMENU v = CreatePopupMenu();
    AppendMenuW(v, MF_STRING, IDM_PODCASTS, L"&Podcasts...");
    AppendMenuW(v, MF_STRING, IDM_EQ, L"&Equalizer");
    AppendMenuW(v, MF_STRING, IDM_PLAYLIST_WIN, L"&Playlist window");
    AppendMenuW(v, MF_STRING, IDM_ONTOP, L"Always on &top");
    AppendMenuW(v, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(v, MF_STRING, IDM_KEYS, L"&Keyboard shortcuts...");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)v, L"&View");

    HMENU h = CreatePopupMenu();
    AppendMenuW(h, MF_STRING, IDM_ABOUT, L"&About Klyraudio");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)h, L"&Help");
    return mb;
}

// Rebuilds everything that depends on the shortcut settings: the accelerator table, the global hotkeys and the
// shortcut texts in the menu bar. Global hotkeys another program already owns end up in g_hotkeyFailed.
static void RefreshMenuKeys(HMENU m) {
    int n = GetMenuItemCount(m);
    for (int pos = 0; pos < n; pos++) {
        if (HMENU sub = GetSubMenu(m, pos)) { RefreshMenuKeys(sub); continue; }
        UINT id = GetMenuItemID(m, pos);
        if (KeyIndexOf((int)id) < 0) continue;
        wchar_t buf[256] = {};
        GetMenuStringW(m, pos, buf, 256, MF_BYPOSITION);
        std::wstring label = buf;
        size_t tab = label.find(L'\t');
        if (tab != std::wstring::npos) label.erase(tab);
        ModifyMenuW(m, pos, MF_BYPOSITION | MF_STRING, id, WithKey(label.c_str(), (int)id).c_str());
    }
}

static void ApplyKeyBindings() {
    std::vector<ACCEL> acc;
    for (int i = 0; i < kNumKeyActions; i++) {
        keymap::Chord c;
        if (!keymap::Parse(g_keys[i].window, c) || (c.mods & keymap::kWin)) continue;   // Win can't be a window key
        BYTE f = FVIRTKEY;
        if (c.mods & keymap::kCtrl)  f |= FCONTROL;
        if (c.mods & keymap::kAlt)   f |= FALT;
        if (c.mods & keymap::kShift) f |= FSHIFT;
        acc.push_back(ACCEL{ f, (WORD)c.vk, (WORD)kKeyActions[i].cmd });
    }
    if (g_accel) DestroyAcceleratorTable(g_accel);
    g_accel = acc.empty() ? nullptr : CreateAcceleratorTableW(acc.data(), (int)acc.size());

    // global hotkeys: id = action index + 1, delivered as WM_HOTKEY
    for (int i = 0; i < kNumKeyActions; i++) UnregisterHotKey(g_hwnd, i + 1);
    g_hotkeyFailed.clear();
    for (int i = 0; i < kNumKeyActions; i++) {
        keymap::Chord c;
        if (!keymap::Parse(g_keys[i].global, c)) continue;
        UINT mods = MOD_NOREPEAT;
        if (c.mods & keymap::kCtrl)  mods |= MOD_CONTROL;
        if (c.mods & keymap::kAlt)   mods |= MOD_ALT;
        if (c.mods & keymap::kShift) mods |= MOD_SHIFT;
        if (c.mods & keymap::kWin)   mods |= MOD_WIN;
        if (!RegisterHotKey(g_hwnd, i + 1, mods, c.vk)) g_hotkeyFailed.push_back(i);
    }

    if (HMENU mb = GetMenu(g_hwnd)) RefreshMenuKeys(mb);
}

static void UpdateUi() {
    static const wchar_t* repTxt[] = { L"Repeat: Off", L"Repeat: All", L"Repeat: One" };
    SetWindowTextW(GetDlgItem(g_hwnd, IDM_REPEAT), repTxt[g_repeat]);
    for (int id : { IDM_PREV, IDM_PLAY, IDM_PAUSE, IDM_STOP, IDM_NEXT, IDM_SHUFFLE, IDM_REPEAT, IDM_EQ })
        InvalidateRect(GetDlgItem(g_hwnd, id), nullptr, FALSE);
    HMENU mb = GetMenu(g_hwnd);
    CheckMenuItem(mb, IDM_SHUFFLE, MF_BYCOMMAND | (g_shuffle ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(mb, IDM_REPEAT, MF_BYCOMMAND | (g_repeat ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(mb, IDM_ONTOP, MF_BYCOMMAND | (g_onTop ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(mb, IDM_PLAYLIST_WIN, MF_BYCOMMAND | (g_plWanted ? MF_CHECKED : MF_UNCHECKED));
    if (g_playMenu) CheckMenuItem(g_playMenu, IDM_GAPLESS, MF_BYCOMMAND | (g_player.gapless.load() ? MF_CHECKED : MF_UNCHECKED));
    if (g_fadeMenu) {
        const int ms = g_player.fadeMs.load();
        for (int i = 0; i < 7; i++)
            CheckMenuItem(g_fadeMenu, IDM_FADE_0 + i, MF_BYCOMMAND | (ms == kFadeSecs[i] * 1000 ? MF_CHECKED : MF_UNCHECKED));
    }
    UpdateNext();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Puts every window back where it was, and opens the ones that were open. Snapped windows come back snapped,
// because snapping is just touching edges and the positions are what gets saved.
static void RestoreWindowLayout() {
    RECT mr = WinRect(g_hwnd);
    PlaceWindow(g_plWnd, L"pl", RECT{ mr.left, mr.bottom, mr.left + 620, mr.bottom + 420 });
    ShowWindow(g_plWnd, g_plWanted ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (g_eqWanted) ToggleEq();
    if (g_podWanted) {
        ShowPodcasts();
        if (g_podWnd) {
            RECT pr = WinRect(g_podWnd);
            PlaceWindow(g_podWnd, L"pod", pr);
        }
    }
    SetForegroundWindow(g_hwnd);
}

static void Layout(int W, int H) {
    MoveWindow(g_seek, 8, 98, W - 16, 24, TRUE);
    int x = 8, y = 126, h = 26;
    struct B { int id, w; };
    const B bs[] = { {IDM_PREV, 46}, {IDM_PLAY, 46}, {IDM_PAUSE, 50}, {IDM_STOP, 46}, {IDM_NEXT, 46},
                     {IDM_SHUFFLE, 60}, {IDM_REPEAT, 84}, {IDM_EQ, 36} };
    for (auto& b : bs) {
        MoveWindow(GetDlgItem(g_hwnd, b.id), x, y, b.w, h, TRUE);
        x += b.w + 4;
        if (b.id == IDM_NEXT) x += 8;
    }
    MoveWindow(g_vol, x + 4, y, std::max(40, W - x - 12), h, TRUE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK ListSubProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_RBUTTONDOWN: {
        // right-click selects the row under the cursor (unless it is already part of the selection)
        LRESULT r = SendMessage(h, LB_ITEMFROMPOINT, 0, l);
        int idx = LOWORD(r);
        if (HIWORD(r) == 0 && idx < (int)g_tracks.size()) {
            SetFocus(h);
            if (!SendMessage(h, LB_GETSEL, idx, 0)) {
                SendMessage(h, LB_SETSEL, FALSE, -1);
                SendMessage(h, LB_SETSEL, TRUE, idx);
            }
            SendMessage(h, LB_SETCARETINDEX, idx, FALSE);
        }
        break;
    }
    case WM_DROPFILES:
        SendMessage(g_hwnd, WM_DROPFILES, w, l);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static void HandleDrop(HDROP drop) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    for (UINT i = 0; i < n; i++) {
        wchar_t buf[MAX_PATH * 2] = {};
        DragQueryFileW(drop, i, buf, MAX_PATH * 2);
        paths.push_back(buf);
    }
    DragFinish(drop);
    AddPaths(paths, true);
}

// ----------------------------------------------------------------------------
//  Main window procedure
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
//  Playlist window: the track list (a window of its own, owned by the main window)
// ----------------------------------------------------------------------------
static void PaintFooter(HDC dc, const RECT& cr) {
        // footer: playlist summary
        RECT fr = { 0, cr.bottom - FOOTER_H, cr.right, cr.bottom };
        FillRect(dc, &fr, g_brBg);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontSmall);
        SetTextColor(dc, C_MUTED);
        std::wstring info;
        if (g_tracks.empty() && g_urlCount > 0) info = L"Fetching link...";
        else if (g_tracks.empty() && g_scanCount > 0) info = L"Scanning folders...";
        else if (g_tracks.empty()) info = L"Playlist is empty \u2014 drop audio files or folders here, or right-click for options";
        else {
            double total = 0; for (auto& t : g_tracks) if (t.dur > 0) total += t.dur;
            info = std::to_wstring(g_tracks.size()) + (g_tracks.size() == 1 ? L" track" : L" tracks");
            if (total > 0) info += L"  \u2022  " + FmtTime(total);
            if (g_shuffle) info += L"  \u2022  shuffle";
            if (g_scanCount > 0) info += L"  \u2022  scanning folders...";
            if (g_urlCount > 0) info += L"  \u2022  fetching link...";
            if (g_repeat) info += g_repeat == 2 ? L"  \u2022  repeat one" : L"  \u2022  repeat all";
        }
        RECT ir = { 12, fr.top, cr.right - 12, fr.bottom };
        DrawTextW(dc, info.c_str(), -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void ShowPlaylistWindow(bool show) {
    g_plWanted = show;
    if (g_plWnd) ShowWindow(g_plWnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    UpdateUi();
}

static LRESULT CALLBACK PlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_list = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_NOTIFY | LBS_EXTENDEDSEL,
            0, 0, 10, 10, h, (HMENU)IDC_LIST, g_hInst, nullptr);
        SendMessage(g_list, LB_SETITEMHEIGHT, 0, 20);
        SetWindowTheme(g_list, L"DarkMode_Explorer", nullptr);   // dark scrollbar (Win10 1809+/11)
        SetWindowSubclass(g_list, ListSubProc, 1, 0);
        DragAcceptFiles(g_list, TRUE);
        return 0;
    case WM_SIZE: {
        RECT c; GetClientRect(h, &c);
        MoveWindow(g_list, 8, 8, std::max(20, (int)c.right - 16), std::max(20, (int)c.bottom - FOOTER_H - 12), TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = 300; mm->ptMinTrackSize.y = 160;
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)w, &r, g_brBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        PaintFooter(dc, cr);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlID == IDC_LIST) { mi->itemHeight = 20; return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlID == IDC_LIST) { DrawListItem(di); return TRUE; }
        break;
    }
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)g_brBg;
    case WM_CONTEXTMENU:
        if ((HWND)w == g_list) { ShowListMenu(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0; }
        break;
    case WM_COMMAND:                       // the list's notifications and the menu commands belong to the main window
        return SendMessage(g_hwnd, WM_COMMAND, w, l);
    case WM_DROPFILES:
        SendMessage(g_hwnd, WM_DROPFILES, w, l);
        return 0;
    case WM_ENTERSIZEMOVE: DockBegin(h); return 0;
    case WM_MOVING: DockMoving(h, (RECT*)l); return TRUE;
    case WM_EXITSIZEMOVE: DockEnd(); return 0;
    case WM_CLOSE:
        ShowPlaylistWindow(false);         // hide, don't destroy: the window comes back where it was
        return 0;
    case WM_DESTROY:
        g_plWnd = nullptr;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void CreatePlaylistWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = PlProc; wc.hInstance = g_hInst; wc.lpszClassName = L"KLYRAUDIO_PLAYLIST";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = g_brBg;
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassW(&wc);
    // Owned by the main window: it hides and minimises with it, stays above it, and has no taskbar button of its own.
    g_plWnd = CreateWindowExW(0, L"KLYRAUDIO_PLAYLIST", L"Klyraudio Playlist", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 620, 420, g_hwnd, nullptr, g_hInst, nullptr);
    DarkTitle(g_plWnd);
}


static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_player.notify = hwnd;
        SetMenu(hwnd, BuildMenu());
        {   // fixed height: the playlist is a window of its own now
            RECT fr = { 0, 0, 620, kMainClientH };
            AdjustWindowRect(&fr, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), TRUE);
            g_mainH = fr.bottom - fr.top;
            SetWindowPos(hwnd, nullptr, 0, 0, 620, g_mainH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        DarkTitle(hwnd);
        g_seek = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_TOOLTIPS,
            0, 0, 10, 10, hwnd, (HMENU)IDC_SEEK, g_hInst, nullptr);
        SendMessage(g_seek, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        g_vol = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS,
            0, 0, 10, 10, hwnd, (HMENU)IDC_VOL, g_hInst, nullptr);
        SendMessage(g_vol, TBM_SETRANGE, TRUE, MAKELONG(0, 100));

        auto btn = [&](const wchar_t* txt, int id) {
            CreateWindowW(L"BUTTON", txt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        };
        btn(L"Prev", IDM_PREV); btn(L"Play", IDM_PLAY); btn(L"Pause", IDM_PAUSE);
        btn(L"Stop", IDM_STOP); btn(L"Next", IDM_NEXT);
        btn(L"Shuffle", IDM_SHUFFLE);
        btn(L"Repeat: Off", IDM_REPEAT); btn(L"EQ", IDM_EQ);

        EnumChildWindows(hwnd, [](HWND c, LPARAM f) -> BOOL {
            if (c != g_list) SendMessage(c, WM_SETFONT, f, TRUE); return TRUE; }, (LPARAM)g_fontUI);

        DragAcceptFiles(hwnd, TRUE);
        CreatePlaylistWindow();
        g_curTitle = L"Klyraudio";
        LoadSettings();
        ApplyKeyBindings();
        if (g_onTop) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        // restore last playlist
        MetaList meta;
        LoadPlaylistFile(AppDataDir() + L"\\last.klyr", meta);
        if (meta.empty() && g_tracks.empty()) LoadPlaylistFile(AppDataDir() + L"\\last.small", meta);   // pre-1.4 file
        if (meta.empty() && g_tracks.empty()) LoadPlaylistFile(AppDataDir() + L"\\last.cumdrops", meta);   // pre-1.2.7 file
        RefreshList();
        if (!meta.empty()) std::thread(MetaThread, meta, hwnd).detach();

        SetTimer(hwnd, 1, 33, nullptr);
        UpdateUi();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = 580;
        if (g_mainH) { mm->ptMinTrackSize.y = g_mainH; mm->ptMaxTrackSize.y = g_mainH; }   // fixed height
        return 0;
    }
    case WM_ENTERSIZEMOVE: DockBegin(hwnd); return 0;
    case WM_MOVING: DockMoving(hwnd, (RECT*)lParam); return TRUE;
    case WM_EXITSIZEMOVE: DockEnd(); return 0;
    case WM_SIZE:
        Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hwnd, &r);
        FillRect((HDC)wParam, &r, g_brBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        PaintPanel(dc, cr.right);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lParam;
        if (nh->code == NM_CUSTOMDRAW && (nh->hwndFrom == g_seek || nh->hwndFrom == g_vol))
            return TrackbarCD((NMCUSTOMDRAW*)lParam, false, false);
        break;
    }
    case WM_CONTEXTMENU:
        if ((HWND)wParam == g_list) { ShowListMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0; }
        break;
    case WM_TIMER: {
        // If the mouse button was released somewhere we never heard about (focus loss, a dialog,
        // alt-tab), the "dragging" flag would stay set and freeze the bar for good. Self-heal.
        if (g_player.state != Player::Stopped && g_cur >= 0 && g_cur < (int)g_tracks.size() && g_tracks[g_cur].dur <= 0) {
            double d = g_player.durSec;
            if (d > 0) { g_tracks[g_cur].dur = d; InvalidateRect(g_list, nullptr, FALSE); InvalidateFooter(); }
        }
        if (g_cur >= 0 && g_player.Stalled()) {      // playback thread wedged: restart where we were
            double resume = g_player.posSec;
            PlayIndex(g_cur, false);
            g_player.Seek(resume);
        }
        if (g_seekDragging && GetCapture() != g_seek && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            g_seekDragging = false;
        if (g_player.state != Player::Stopped && !g_seekDragging) {
            double dur = CurrentDuration();
            int pos = dur > 0 ? (int)(g_player.posSec / dur * 1000.0) : 0;
            SetTrackPos(g_seek, std::max(0, std::min(1000, pos)));
        } else if (g_player.state == Player::Stopped && !g_seekDragging) {
            SetTrackPos(g_seek, 0);
        }
        UpdateSpectrum();
        RECT pr = { 0, 0, 4000, PANEL_H };
        InvalidateRect(hwnd, &pr, FALSE);

        // hover highlight for the owner-drawn buttons
        POINT cp; GetCursorPos(&cp);
        HWND under = WindowFromPoint(cp), nh = nullptr;
        if (under && GetParent(under) == hwnd && under != g_list) {
            wchar_t cls[16] = {};
            GetClassNameW(under, cls, 16);
            if (_wcsicmp(cls, L"Button") == 0) nh = under;
        }
        if (nh != g_hotBtn) {
            HWND old = g_hotBtn;
            g_hotBtn = nh;
            if (old) InvalidateRect(old, nullptr, FALSE);
            if (nh)  InvalidateRect(nh, nullptr, FALSE);
        }
        return 0;
    }
    case WM_HSCROLL: {
        HWND src = (HWND)lParam;
        int code = LOWORD(wParam);
        if (src) InvalidateRect(src, nullptr, FALSE);
        if (src == g_seek) {
            auto commit = [&]() {
                int v = (int)SendMessage(g_seek, TBM_GETPOS, 0, 0);
                double dur = CurrentDuration();
                if (dur > 0 && g_player.state != Player::Stopped) g_player.Seek(v / 1000.0 * dur);
            };
            // While dragging, only seek once, when the thumb is released (TB_ENDTRACK). Releasing
            // also sends TB_THUMBPOSITION, which used to trigger a second back-to-back seek.
            if (code == TB_THUMBTRACK) g_seekDragging = true;
            else if (code == TB_THUMBPOSITION) { if (!g_seekDragging) commit(); }
            else if (code == TB_ENDTRACK) { if (g_seekDragging) commit(); g_seekDragging = false; }
            else commit();
        } else if (src == g_vol) {
            g_player.volume = (float)SendMessage(g_vol, TBM_GETPOS, 0, 0) / 100.0f;
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == IDC_LIST) { if (code == LBN_DBLCLK) PlayCaret(); return 0; }
        g_failCount = 0;
        switch (id) {
        case IDM_ADD_FILES:  DoAddFiles(); break;
        case IDM_ADD_FOLDER: DoAddFolder(); break;
        case IDM_ADD_URL:    DoAddUrl(); break;
        case IDM_PODCASTS:   ShowPodcasts(); break;
        case IDM_OPEN_PL:    DoOpenPlaylist(); break;
        case IDM_SAVE_PL:    DoSavePlaylist(); break;
        case IDM_EXIT:       DestroyWindow(hwnd); break;
        case IDM_PLAY:
            if (g_player.state == Player::Paused) g_player.TogglePause();
            else if (!g_tracks.empty()) {
                int idx = g_cur;
                if (idx < 0) {
                    int c = (int)SendMessage(g_list, LB_GETCARETINDEX, 0, 0);
                    idx = (c >= 0 && c < (int)g_tracks.size()) ? c : 0;
                }
                PlayIndex(idx, false);
            }
            break;
        case IDM_PAUSE:  g_player.TogglePause(); break;
        case IDM_STOP:   StopPlayback(); break;
        case IDM_PREV:   PrevTrack(); break;
        case IDM_NEXT:   NextTrack(false); break;
        case IDM_SHUFFLE: g_shuffle = !g_shuffle; break;
        case IDM_REPEAT:  g_repeat = (g_repeat + 1) % 3; break;
        case IDM_GAPLESS: g_player.gapless = !g_player.gapless.load(); break;
        case IDM_FADE_0: case IDM_FADE_1: case IDM_FADE_2: case IDM_FADE_3:
        case IDM_FADE_4: case IDM_FADE_5: case IDM_FADE_6:
            g_player.fadeMs = kFadeSecs[id - IDM_FADE_0] * 1000; break;
        case IDM_SEEK_FWD:  SeekRelative(5); break;
        case IDM_SEEK_BACK: SeekRelative(-5); break;
        case IDM_VOL_UP: SetVolume((int)SendMessage(g_vol, TBM_GETPOS, 0, 0) + 5); break;
        case IDM_VOL_DN: SetVolume((int)SendMessage(g_vol, TBM_GETPOS, 0, 0) - 5); break;
        case IDM_EQ:     ToggleEq(); break;
        case IDM_ONTOP:
            g_onTop = !g_onTop;
            SetWindowPos(hwnd, g_onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            break;
        case IDM_REMOVE: RemoveSelected(); break;
        case IDM_CLEAR:
            StopPlayback(); g_tracks.clear(); g_cur = -1; g_history.clear();
            UpdateTitleText(); RefreshList();
            break;
        case IDM_SELALL: SendMessage(g_list, LB_SETSEL, TRUE, -1); break;
        case IDM_PLAY_SEL:      PlayCaret(); break;
        case IDM_COPY_TITLE:    CopySelected(false); break;
        case IDM_COPY_PATH:     CopySelected(true); break;
        case IDM_SHOW_EXPLORER: ShowInExplorer(); break;
        case IDM_PROPS:         ShowProperties(); break;
        case IDM_SORT_TITLE: case IDM_SORT_PATH: case IDM_SORT_DUR:
        case IDM_SORT_REV:   case IDM_SORT_RAND:
            SortPlaylist(id);
            break;
        case IDM_ABOUT: ShowAbout(); break;
        case IDM_KEYS:  ShowShortcuts(); break;
        case IDM_PLAYLIST_WIN: ShowPlaylistWindow(!g_plWanted); break;
        }
        UpdateUi();
        return 0;
    }
    case WM_DROPFILES:
        HandleDrop((HDROP)wParam);
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lParam;
        if (mi->CtlID == IDC_LIST) { mi->itemHeight = 20; return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lParam;
        if (di->CtlID == IDC_LIST) { DrawListItem(di); return TRUE; }
        if (di->CtlType == ODT_BUTTON) { DrawBtn(di); return TRUE; }
        break;
    }
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)g_brBg;
    case WM_HOTKEY: {
        // a global hotkey: run the command it is bound to (wParam = action index + 1)
        int idx = (int)wParam - 1;
        if (idx >= 0 && idx < kNumKeyActions) SendMessage(hwnd, WM_COMMAND, kKeyActions[idx].cmd, 0);
        return 0;
    }
    case WM_APP_TRACKEND:
        if ((int)lParam == g_player.gen.load()) { g_failCount = 0; NextTrack(true); UpdateUi(); }
        return 0;
    case WM_APP_TRACKSWITCH:
        // the player carried on into the next track without a gap: follow it in the playlist
        if ((int)lParam == g_player.gen.load()) {
            int idx = -1;
            for (int i = 0; i < (int)g_tracks.size(); i++) if (g_tracks[i].id == (int)wParam) { idx = i; break; }
            if (idx < 0) { StopPlayback(); return 0; }
            g_failCount = 0;
            CommitTrack(idx, true);
            UpdateUi();
        }
        return 0;
    case WM_APP_TRACKERR:
        if ((int)lParam == g_player.gen.load()) {
            g_failCount++;
            if (g_failCount >= (int)g_tracks.size()) {
                StopPlayback();
                MessageBoxW(hwnd, L"None of the queued tracks could be decoded.\n\n"
                    L"For OGG Vorbis / Opus, install the free \"Web Media Extensions\" from the Microsoft Store.",
                    L"Klyraudio", MB_ICONWARNING);
                g_failCount = 0;
            } else {
                NextTrack(true, true);
            }
        }
        return 0;
    case WM_APP_SCAN: {
        ScanResult* r = (ScanResult*)lParam;
        --g_scanCount;
        size_t before = g_tracks.size();
        MetaList meta;
        for (auto& f : r->files) AddTrack(f, L"", -1, meta);
        bool ap = r->autoplay;
        delete r;
        RefreshList();
        if (!meta.empty()) std::thread(MetaThread, meta, hwnd).detach();
        if (ap && g_player.state == Player::Stopped && g_tracks.size() > before) PlayIndex((int)before);
        UpdateUi();
        return 0;
    }
    case WM_APP_POD:
        PodApply((PodResult*)lParam);
        return 0;
    case WM_APP_PODSEARCH:
        PodOnSearch((PodSearchResult*)lParam);
        return 0;
    case WM_APP_URL: {
        UrlResult* r = (UrlResult*)lParam;
        --g_urlCount;
        size_t before = g_tracks.size();
        MetaList meta;                                  // links are never probed in the background
        for (auto& it : r->items) AddTrack(it.url, it.title, it.dur, meta);
        bool ap = r->autoplay;
        std::wstring err = r->error;
        delete r;
        RefreshList();
        if (!err.empty()) MessageBoxW(hwnd, err.c_str(), L"Klyraudio", MB_ICONWARNING);
        else if (ap && g_player.state == Player::Stopped && g_tracks.size() > before) PlayIndex((int)before);
        UpdateUi();
        return 0;
    }
    case WM_APP_META: {
        MetaResult* r = (MetaResult*)lParam;
        for (size_t i = 0; i < g_tracks.size(); i++) {
            if (g_tracks[i].id == r->id) {
                if (!r->title.empty()) g_tracks[i].title = r->title;
                if (r->dur > 0) g_tracks[i].dur = r->dur;
                if ((int)i == g_cur) UpdateTitleText();
                break;
            }
        }
        delete r;
        InvalidateRect(g_list, nullptr, FALSE);
        InvalidateFooter();
        return 0;
    }
    case WM_DESTROY: {
        g_quitting = true;
        KillTimer(hwnd, 1);
        for (int i = 0; i < kNumKeyActions; i++) UnregisterHotKey(hwnd, i + 1);
        g_player.Stop();
        SaveSettings();
        SavePlaylistFile(AppDataDir() + L"\\last.klyr");
        if (g_podWnd) DestroyWindow(g_podWnd);        // no longer owned by this window, so close it ourselves
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------------
//  Entry point
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    g_hInst = hInst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icc);

    g_brBg = CreateSolidBrush(C_BG);
    g_fontBig   = CreateFontW(-44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    g_fontMed   = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontUI    = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontTitle = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"KLYRAUDIO_MAIN";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"KLYRAUDIO_MAIN", L"Klyraudio",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 620, 640,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    bool wasMax = RestoreWindowPos(hwnd);
    ShowWindow(hwnd, wasMax ? SW_SHOWMAXIMIZED : nShow);
    UpdateWindow(hwnd);
    RestoreWindowLayout();

    // files passed on the command line ("Open with Klyraudio")
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; i++) args.push_back(argv[i]);
        LocalFree(argv);
        if (!args.empty()) AddPaths(args, true);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        // Shortcuts apply to the main window and the playlist window. The Podcasts and equalizer windows have
        // their own keys (type-ahead in their lists must not trigger player shortcuts).
        HWND root = GetAncestor(msg.hwnd, GA_ROOT);
        const bool ours = root && (root == hwnd || (g_plWnd && root == g_plWnd));
        if (!(ours && g_accel && TranslateAccelerator(root, g_accel, &msg))) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
