// teddy_unitygui.cpp - arm64 il2cpp (Granny): Dobby + Unity's built-in IMGUI
// Menu appears when the teddy is placed.
//   * "Show image" button ARMS a trap (and preloads yedu.png + naxui.wav).
//   * When Granny catches the player (EnemyAIGranny.Playercaught) -> Time.timeScale = 0
//     and a GUI question appears: "Хочеш штоб тебя сьел дрон?"
//       - Да  -> full-screen yedu.png + naxui.wav; after 3 s the text "Ти теперь щастлив" appears,
//                image + text stay forever (timeScale stays 0)
//       - Нет -> timeScale = 1, game continues
//   * Granny skin swap button kept as before. Speed selector removed. FPS counter hidden.
//   * NEW: "Golda" difficulty added to the difficulty Dropdown (Granny = 2x Hard speed).
//
// Files (png / wav) are looked up in the game's files folder first:
//   /sdcard/Android/data/<package>/files/yedu.png , naxui.wav
// and then in the APK's assets/ folder (assets/yedu.png, assets/naxui.wav).
//
// WAV: PCM 8/16-bit or 32-bit float, any channels / sample rate.
// PNG: 8-bit, non-interlaced (or anything Unity's ImageConversion can load).
//
// Unity GUI calls only work inside an OnGUI() callback. We borrow the game's own FPSDisplay.OnGUI:
// when the teddy is placed we AddComponent(FPSDisplay) onto the teddy object and hook its OnGUI.
//
// Link: -llog -ldl -lz     Hook lib: Dobby

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <zlib.h>
#include <cstdlib>
#include "dobby.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "TeddyMod", __VA_ARGS__)

// on-screen status lines (shown in the menu) - so you can debug without logcat
static char g_status[300]  = "image: idle";
static char g_status2[300] = "skin: idle";
static char g_status3[300] = "sound: idle";
static char *g_st = g_status;                        // which line STATUS() currently writes to
#define STATUS(...) do { snprintf(g_st, 300, __VA_ARGS__); LOG("%s", g_st); } while (0)

constexpr uintptr_t RVA_OnTriggerEnter = 0x15531F8;
constexpr uintptr_t RVA_FPS_OnGUI      = 0x144D828;
constexpr uintptr_t RVA_Playercaught   = 0x145BACC;   // EnemyAIGranny.Playercaught() : IEnumerator
constexpr uintptr_t RVA_get_tag        = 0x2A9ACD0;
constexpr uintptr_t RVA_get_gameObject = 0x2A9A678;
constexpr uintptr_t RVA_AddComponent   = 0x2A9DEB0;
constexpr uintptr_t RVA_Screen_height  = 0x2A76984;
constexpr uintptr_t RVA_GUI_Box        = 0x2ACA434;
constexpr uintptr_t RVA_GUI_Label      = 0x2AC90AC;
constexpr uintptr_t RVA_GUI_Button     = 0x2ACA814;
constexpr uintptr_t RVA_GUI_set_matrix = 0x2AC9024;
constexpr uintptr_t RVA_set_timeScale  = 0x2AA61C0;
// GUI.DrawTexture(Rect, Texture, ScaleMode, bool alphaBlend, float imageAspect, Color, float borderWidth, float borderRadius)
constexpr uintptr_t RVA_GUI_DrawTexture = 0x2AC98E4;

static const char *TEDDY_TAG   = "teddy";
static const char *IMAGE_FILE  = "yedu.png";
static const char *SOUND_FILE  = "naxui.wav";
static const int   TEXT_DELAY_MS = 3000;              // after "Да": when the final text appears
static char g_lastTag[64] = "(none yet)";

static std::atomic<bool> g_teddyPlaced{false};
static std::atomic<bool> g_showMenu{false};
static std::atomic<bool> g_armed{false};              // "Show image" clicked: waiting for Playercaught
static std::atomic<bool> g_prompt{false};             // question dialog is on screen
static std::atomic<bool> g_yes{false};                // player answered "Да"
static long long g_yesAt = 0;                         // steady-clock ms when "Да" was clicked
static bool g_hostAdded = false;

// ---------------------------------------------------------------- types
struct Rect { float x, y, w, h; };
struct Color { float r, g, b, a; };
struct Matrix4x4 { float m[16]; };

static void *(*get_tag)(void *, const void *)            = nullptr;
static void *(*get_gameObject)(void *, const void *)     = nullptr;
static void *(*AddComponent)(void *, void *, const void *) = nullptr;
static int   (*Screen_height)(const void *)              = nullptr;
static void  (*GUI_Box)(Rect, void *, const void *)      = nullptr;
static void  (*GUI_Label)(Rect, void *, const void *)    = nullptr;
static bool  (*GUI_Button)(Rect, void *, const void *)    = nullptr;
static void  (*GUI_set_matrix)(Matrix4x4, const void *)  = nullptr;
static void  (*set_timeScale)(float)                     = nullptr;
static void  (*GUI_DrawTexture)(Rect, void *, int, bool, float, Color, float, float, const void *) = nullptr;

// il2cpp exports (resolved with dlsym)
static void *(*il2cpp_domain_get)()                                  = nullptr;
static void *(*il2cpp_domain_assembly_open)(void *, const char *)    = nullptr;
static void *(*il2cpp_assembly_get_image)(void *)                    = nullptr;
static void *(*il2cpp_class_from_name)(void *, const char *, const char *) = nullptr;
static void *(*il2cpp_class_get_type)(void *)                        = nullptr;
static void *(*il2cpp_type_get_object)(void *)                       = nullptr;
static void *(*il2cpp_string_new)(const char *)                      = nullptr;
static void *(*il2cpp_object_new)(void *)                            = nullptr;
static void *(*il2cpp_class_get_method_from_name)(void *, const char *, int) = nullptr;
static void *(*il2cpp_get_corlib)()                                  = nullptr;
static void *(*il2cpp_array_new)(void *, uintptr_t)                  = nullptr;
static uint32_t (*il2cpp_gchandle_new)(void *, bool)                 = nullptr;

// ---------------------------------------------------------------- helpers
static uintptr_t getLibBase(const char *name) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t start = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, name)) { sscanf(line, "%lx", &start); break; }
    fclose(f);
    return start;
}

static std::string il2cppToStd(void *str) {          // length @0x10, UTF-16 @0x14
    if (!str) return {};
    int len = *(int *)((uintptr_t)str + 0x10);
    auto *c = (char16_t *)((uintptr_t)str + 0x14);
    std::string out;
    for (int i = 0; i < len && i < 256; i++) out.push_back(c[i] < 128 ? (char)c[i] : '?');
    return out;
}

static void *Str(const char *s) { return il2cpp_string_new(s); }   // UTF-8 in, so Cyrillic is fine

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void *ImageOf(const char *asmName) {
    void *a = il2cpp_domain_assembly_open(il2cpp_domain_get(), asmName);
    return a ? il2cpp_assembly_get_image(a) : nullptr;
}
// look up an il2cpp method by name; returns the native function pointer (MethodInfo's first field)
static void *MethodOf(const char *asmName, const char *ns, const char *cls, const char *name, int argc, void **miOut = nullptr) {
    void *img = ImageOf(asmName);
    void *k   = img ? il2cpp_class_from_name(img, ns, cls) : nullptr;
    void *mi  = k ? il2cpp_class_get_method_from_name(k, name, argc) : nullptr;
    if (miOut) *miOut = mi;
    if (!mi) LOG("method not found: %s.%s::%s/%d (%s)", ns, cls, name, argc, asmName);
    return mi ? *(void **)mi : nullptr;
}

static std::string packageName() {
    char buf[256] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return std::string(buf);
}

static bool readFile(const std::string &path, std::vector<char> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize(n);
    size_t got = fread(out.data(), 1, n, f);
    fclose(f);
    return got == (size_t)n;
}

// ---------------------------------------------------------------- read assets/<name> from the installed APK
// (an APK is a zip: find the entry in the central directory and unpack it with zlib)
static bool zipRead(const std::string &apk, const std::string &entry, std::vector<char> &out) {
    FILE *f = fopen(apk.c_str(), "rb");
    if (!f) return false;
    auto u16 = [](const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); };
    auto u32 = [](const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    bool ok = false;
    do {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        long tail = sz < 70000 ? sz : 70000;
        if (tail < 22) break;
        std::vector<uint8_t> t(tail);
        fseek(f, sz - tail, SEEK_SET);
        if (fread(t.data(), 1, tail, f) != (size_t)tail) break;
        long e = -1;
        for (long i = tail - 22; i >= 0; i--)
            if (t[i] == 0x50 && t[i + 1] == 0x4b && t[i + 2] == 5 && t[i + 3] == 6) { e = i; break; }
        if (e < 0) break;
        uint32_t cdSize = u32(&t[e + 12]), cdOff = u32(&t[e + 16]);
        if (cdOff == 0xFFFFFFFFu || cdSize > (64u << 20)) break;          // zip64 not supported
        std::vector<uint8_t> cd(cdSize);
        fseek(f, cdOff, SEEK_SET);
        if (fread(cd.data(), 1, cdSize, f) != cdSize) break;
        size_t p = 0;
        while (p + 46 <= cd.size() && u32(&cd[p]) == 0x02014b50u) {
            uint32_t method = u16(&cd[p + 10]), csz = u32(&cd[p + 20]), usz = u32(&cd[p + 24]);
            uint32_t nl = u16(&cd[p + 28]), el = u16(&cd[p + 30]), cl = u16(&cd[p + 32]), lho = u32(&cd[p + 42]);
            if (p + 46 + nl > cd.size()) break;
            if (std::string((const char *)&cd[p + 46], nl) == entry) {
                uint8_t lh[30];
                fseek(f, lho, SEEK_SET);
                if (fread(lh, 1, 30, f) != 30 || u32(lh) != 0x04034b50u) break;
                long dataOff = (long)lho + 30 + u16(lh + 26) + u16(lh + 28);
                if (csz > (64u << 20) || usz > (64u << 20)) break;
                std::vector<uint8_t> comp(csz);
                fseek(f, dataOff, SEEK_SET);
                if (fread(comp.data(), 1, csz, f) != csz) break;
                out.assign(usz, 0);
                if (method == 0) {
                    ok = (csz == usz);
                    if (ok) memcpy(out.data(), comp.data(), usz);
                } else if (method == 8) {
                    z_stream zs{};
                    if (inflateInit2(&zs, -MAX_WBITS) == Z_OK) {
                        zs.next_in = comp.data(); zs.avail_in = csz;
                        zs.next_out = (Bytef *)out.data(); zs.avail_out = usz;
                        ok = (inflate(&zs, Z_FINISH) == Z_STREAM_END);
                        inflateEnd(&zs);
                    }
                }
                break;
            }
            p += 46 + nl + el + cl;
        }
    } while (0);
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

static void addUnique(std::vector<std::string> &v, const std::string &s) {
    for (auto &x : v) if (x == s) return;
    v.push_back(s);
}

static std::vector<std::string> apkCandidates() {
    std::vector<std::string> v;
    Dl_info info{};
    if (dladdr((void *)&apkCandidates, &info) && info.dli_fname) {      // where our own .so was loaded from
        std::string p = info.dli_fname;
        size_t b = p.find(".apk");
        if (b != std::string::npos) addUnique(v, p.substr(0, b + 4));
        size_t l = p.find("/lib/");
        if (l != std::string::npos) addUnique(v, p.substr(0, l) + "/base.apk");
    }
    if (FILE *f = fopen("/proc/self/maps", "r")) {                      // any .apk mapped into the process
        char line[768];
        while (fgets(line, sizeof line, f)) {
            if (!strstr(line, ".apk")) continue;
            char *path = strchr(line, '/');
            if (!path) continue;
            std::string s(path);
            while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
            size_t b = s.find(".apk");
            if (b == std::string::npos) continue;
            s = s.substr(0, b + 4);
            addUnique(v, s);
            size_t sl = s.rfind('/');
            if (sl != std::string::npos) addUnique(v, s.substr(0, sl) + "/base.apk");
        }
        fclose(f);
    }
    return v;
}

static bool readFromApk(const char *name, std::vector<char> &out, std::string &tried) {
    std::string entry = std::string("assets/") + name;
    for (auto &apk : apkCandidates()) {
        if (tried.empty()) tried = apk;
        if (zipRead(apk, entry, out)) { tried = apk; return true; }
    }
    return false;
}

// files folder first, then APK assets/. Writes a STATUS line on failure (to the current g_st line).
static bool LoadBytes(const char *file, std::vector<char> &bytes, bool &fromFiles) {
    std::string pkg = packageName();
    std::vector<std::string> cands = {
        "/storage/emulated/0/Android/data/" + pkg + "/files/" + file,
        "/sdcard/Android/data/" + pkg + "/files/" + file,
        "/data/data/" + pkg + "/files/" + file,
        "/data/user/0/" + pkg + "/files/" + file,
    };
    fromFiles = false;
    for (auto &p : cands)
        if (readFile(p, bytes)) { fromFiles = true; return true; }
    std::string apkTried;
    if (!readFromApk(file, bytes, apkTried)) {
        STATUS("FAIL: '%s' not in files folder (pkg '%s') and not in APK assets/ (apk tried: %s)",
               file, pkg.c_str(), apkTried.empty() ? "none found" : apkTried.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- minimal PNG decoder
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static bool decodePNG(const std::vector<char> &in, int &W, int &H, std::vector<uint8_t> &rgba, std::string &err) {
    const uint8_t *p = (const uint8_t *)in.data();
    size_t n = in.size();
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 33 || memcmp(p, sig, 8)) { err = "not a PNG file"; return false; }
    int bd = 0, ct = 0, il = 0;
    W = H = 0;
    std::vector<uint8_t> idat, plte, trns;
    size_t pos = 8;
    while (pos + 12 <= n) {
        uint32_t len = be32(p + pos);
        const uint8_t *t = p + pos + 4, *d = p + pos + 8;
        if (pos + 12 + (size_t)len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) { W = be32(d); H = be32(d + 4); bd = d[8]; ct = d[9]; il = d[12]; }
        else if (!memcmp(t, "IDAT", 4)) idat.insert(idat.end(), d, d + len);
        else if (!memcmp(t, "PLTE", 4)) plte.assign(d, d + len);
        else if (!memcmp(t, "tRNS", 4)) trns.assign(d, d + len);
        else if (!memcmp(t, "IEND", 4)) break;
        pos += 12 + len;
    }
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) { err = "bad PNG size"; return false; }
    if (bd != 8 || il != 0) { err = "PNG must be 8-bit and not interlaced (re-save as normal RGBA PNG)"; return false; }
    int ch;
    switch (ct) { case 0: ch = 1; break; case 2: ch = 3; break; case 3: ch = 1; break; case 4: ch = 2; break; case 6: ch = 4; break;
                  default: err = "unsupported PNG color type"; return false; }
    size_t stride = (size_t)W * ch;
    std::vector<uint8_t> raw((stride + 1) * H);
    uLongf outLen = raw.size();
    if (uncompress(raw.data(), &outLen, idat.data(), idat.size()) != Z_OK || outLen != raw.size()) { err = "PNG zlib error"; return false; }

    for (int y = 0; y < H; y++) {                      // un-filter
        uint8_t *line = &raw[(size_t)y * (stride + 1) + 1];
        uint8_t ft = raw[(size_t)y * (stride + 1)];
        const uint8_t *prev = y ? line - (stride + 1) : nullptr;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)ch ? line[i - ch] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)ch) ? prev[i - ch] : 0;
            int v = line[i];
            switch (ft) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: { int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
                          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
                default: break;
            }
            line[i] = (uint8_t)v;
        }
    }

    rgba.assign((size_t)W * H * 4, 255);               // to RGBA (top-down)
    for (int y = 0; y < H; y++) {
        const uint8_t *line = &raw[(size_t)y * (stride + 1) + 1];
        for (int x = 0; x < W; x++) {
            uint8_t *o = &rgba[((size_t)y * W + x) * 4];
            const uint8_t *s = line + (size_t)x * ch;
            switch (ct) {
                case 0: o[0] = o[1] = o[2] = s[0]; break;
                case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; break;
                case 3: { size_t k = s[0] * 3; if (k + 2 < plte.size()) { o[0] = plte[k]; o[1] = plte[k + 1]; o[2] = plte[k + 2]; }
                          if (s[0] < trns.size()) o[3] = trns[s[0]]; break; }
                case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
                case 6: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- minimal WAV decoder -> interleaved floats
static bool decodeWAV(const std::vector<char> &in, int &channels, int &hz, std::vector<float> &out, std::string &err) {
    const uint8_t *p = (const uint8_t *)in.data();
    size_t n = in.size();
    auto le16 = [](const uint8_t *q) { return (int)(q[0] | q[1] << 8); };
    auto le32 = [](const uint8_t *q) { return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24; };
    if (n < 44 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) { err = "not a WAV file"; return false; }
    int fmtTag = 0, bits = 0;
    channels = 0; hz = 0;
    const uint8_t *data = nullptr;
    size_t dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= n) {
        uint32_t len = le32(p + pos + 4);
        const uint8_t *d = p + pos + 8;
        if (!memcmp(p + pos, "fmt ", 4) && len >= 16 && pos + 8 + 16 <= n) {
            fmtTag = le16(d); channels = le16(d + 2); hz = (int)le32(d + 4); bits = le16(d + 14);
            if (fmtTag == 0xFFFE && len >= 26 && pos + 8 + 26 <= n) fmtTag = le16(d + 24);   // extensible: real tag
        } else if (!memcmp(p + pos, "data", 4)) {
            data = d;
            dataLen = pos + 8 + len <= n ? len : n - pos - 8;
            break;
        }
        pos += 8 + (size_t)len + (len & 1);
    }
    if (!data || channels < 1 || channels > 8 || hz < 8000 || hz > 192000) { err = "bad WAV header"; return false; }
    if (fmtTag == 1 && bits == 16) {
        size_t cnt = dataLen / 2;
        out.resize(cnt);
        for (size_t i = 0; i < cnt; i++) out[i] = (int16_t)(data[i * 2] | data[i * 2 + 1] << 8) / 32768.0f;
    } else if (fmtTag == 1 && bits == 8) {
        out.resize(dataLen);
        for (size_t i = 0; i < dataLen; i++) out[i] = ((int)data[i] - 128) / 128.0f;
    } else if (fmtTag == 3 && bits == 32) {
        size_t cnt = dataLen / 4;
        out.resize(cnt);
        for (size_t i = 0; i < cnt; i++) memcpy(&out[i], data + i * 4, 4);
    } else { err = "WAV must be PCM 8/16-bit or float32"; return false; }
    out.resize(out.size() / channels * channels);
    if (out.empty()) { err = "WAV has no samples"; return false; }
    return true;
}

// ---------------------------------------------------------------- textures (main thread only)
static void *MakeTexture(int W, int H) {                 // new Texture2D(W, H) -> RGBA32
    void *coreImg = ImageOf("UnityEngine.CoreModule");
    void *texK = coreImg ? il2cpp_class_from_name(coreImg, "UnityEngine", "Texture2D") : nullptr;
    if (!texK) { STATUS("FAIL: Texture2D class not found"); return nullptr; }
    void *ctorMI = nullptr;
    void *ctor = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Texture2D", ".ctor", 2, &ctorMI);
    void *tex = il2cpp_object_new(texK);
    if (!ctor || !tex) { STATUS("FAIL: Texture2D ctor(int,int) not found"); return nullptr; }
    ((void (*)(void *, int, int, const void *))ctor)(tex, W, H, ctorMI);
    return tex;
}

static void KeepAlive(void *obj) {                       // HideAndDontSave + GC handle
    void *hfMI = nullptr;
    void *hf = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Object", "set_hideFlags", 1, &hfMI);
    if (hf) ((void (*)(void *, int, const void *))hf)(obj, 61, hfMI);   // HideFlags.HideAndDontSave
    if (il2cpp_gchandle_new) il2cpp_gchandle_new(obj, false);
}

static void *LoadTextureFile(const char *file) {
    std::vector<char> bytes;
    bool fromFiles = false;
    if (!LoadBytes(file, bytes, fromFiles)) return nullptr;
    STATUS("1 '%s' read OK %zu B from %s", file, bytes.size(), fromFiles ? "files folder" : "APK assets/");

    if (!il2cpp_object_new || !il2cpp_array_new || !il2cpp_get_corlib || !il2cpp_class_get_method_from_name) {
        STATUS("FAIL: il2cpp exports missing (object_new/array_new/get_corlib/get_method_from_name)");
        return nullptr;
    }

    void *tex = nullptr;

    // --- way 1: Unity's own decoder (only if the ImageConversion module is in the game)
    void *liMI = nullptr;
    void *img = ImageOf("UnityEngine.ImageConversionModule");
    void *li = nullptr;
    if (img) {
        void *k = il2cpp_class_from_name(img, "UnityEngine", "ImageConversion");
        liMI = k ? il2cpp_class_get_method_from_name(k, "LoadImage", 2) : nullptr;
        li = liMI ? *(void **)liMI : nullptr;
    }
    if (li) {
        void *byteK = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");
        void *arr = byteK ? il2cpp_array_new(byteK, bytes.size()) : nullptr;
        void *t = arr ? MakeTexture(2, 2) : nullptr;
        if (t) {
            memcpy((char *)arr + 0x20, bytes.data(), bytes.size());
            if (((bool (*)(void *, void *, const void *))li)(t, arr, liMI)) tex = t;
        }
    }

    // --- way 2: decode the PNG ourselves, fill the texture with SetPixels32 + Apply
    if (!tex) {
        int W, H; std::vector<uint8_t> rgba; std::string err;
        if (!decodePNG(bytes, W, H, rgba, err)) { STATUS("FAIL: %s", err.c_str()); return nullptr; }
        void *coreImg = ImageOf("UnityEngine.CoreModule");
        void *c32K = coreImg ? il2cpp_class_from_name(coreImg, "UnityEngine", "Color32") : nullptr;
        void *spMI = nullptr, *apMI = nullptr;
        void *sp = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Texture2D", "SetPixels32", 1, &spMI);
        void *ap = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Texture2D", "Apply", 2, &apMI);
        if (!c32K || !sp || !ap) { STATUS("FAIL: Color32/SetPixels32/Apply not found"); return nullptr; }
        void *t = MakeTexture(W, H);
        if (!t) return nullptr;
        void *arr = il2cpp_array_new(c32K, (uintptr_t)W * H);
        if (!arr) { STATUS("FAIL: Color32[] alloc"); return nullptr; }
        uint8_t *dst = (uint8_t *)arr + 0x20;
        for (int y = 0; y < H; y++)                       // Unity rows go bottom-up
            memcpy(dst + (size_t)(H - 1 - y) * W * 4, &rgba[(size_t)y * W * 4], (size_t)W * 4);
        ((void (*)(void *, void *, const void *))sp)(t, arr, spMI);
        ((void (*)(void *, bool, bool, const void *))ap)(t, true, false, apMI);
        tex = t;
        STATUS("2 texture OK via PNG decode %dx%d", W, H);
    }

    KeepAlive(tex);
    return tex;
}

static int ScreenWidth() {
    static void *fn = nullptr, *mi = nullptr;
    if (!fn) fn = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Screen", "get_width", 0, &mi);
    return fn ? ((int (*)(const void *))fn)(mi) : (int)(Screen_height(nullptr) * 16 / 9);
}

// ---------------------------------------------------------------- sound: WAV -> AudioClip -> AudioSource
static void *g_clip = nullptr;
static bool  g_soundPlayed = false;

static void *LoadClipFile(const char *file) {
    std::vector<char> bytes;
    bool fromFiles = false;
    if (!LoadBytes(file, bytes, fromFiles)) return nullptr;

    int ch, hz; std::vector<float> pcm; std::string err;
    if (!decodeWAV(bytes, ch, hz, pcm, err)) { STATUS("FAIL: %s", err.c_str()); return nullptr; }
    int frames = (int)(pcm.size() / ch);

    void *createMI = nullptr, *setMI = nullptr;
    void *create = MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioClip", "Create", 5, &createMI);
    void *setData = MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioClip", "SetData", 2, &setMI);
    if (!create || !setData || !il2cpp_array_new || !il2cpp_get_corlib) { STATUS("FAIL: AudioClip.Create/SetData not found"); return nullptr; }

    void *clip = ((void *(*)(void *, int, int, int, bool, const void *))create)(Str("naxui"), frames, ch, hz, false, createMI);
    if (!clip) { STATUS("FAIL: AudioClip.Create returned null"); return nullptr; }
    void *floatK = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Single");
    void *arr = floatK ? il2cpp_array_new(floatK, pcm.size()) : nullptr;
    if (!arr) { STATUS("FAIL: float[] alloc"); return nullptr; }
    memcpy((char *)arr + 0x20, pcm.data(), pcm.size() * sizeof(float));
    ((bool (*)(void *, void *, int, const void *))setData)(clip, arr, 0, setMI);
    KeepAlive(clip);
    STATUS("sound OK: %d ch, %d Hz, %.1f s (%s)", ch, hz, (double)frames / hz, fromFiles ? "files folder" : "APK assets/");
    return clip;
}

static void PlayClip(void *clip) {
    if (!clip) return;
    void *goK = nullptr;
    if (void *core = ImageOf("UnityEngine.CoreModule")) goK = il2cpp_class_from_name(core, "UnityEngine", "GameObject");
    void *ctorMI = nullptr;
    void *ctor = MethodOf("UnityEngine.CoreModule", "UnityEngine", "GameObject", ".ctor", 0, &ctorMI);
    void *audImg = ImageOf("UnityEngine.AudioModule");
    void *asK = audImg ? il2cpp_class_from_name(audImg, "UnityEngine", "AudioSource") : nullptr;
    if (!goK || !ctor || !asK) { STATUS("FAIL: GameObject/AudioSource not found"); return; }

    void *go = il2cpp_object_new(goK);
    ((void (*)(void *, const void *))ctor)(go, ctorMI);
    if (il2cpp_gchandle_new) il2cpp_gchandle_new(go, false);
    void *dMI = nullptr;
    void *dont = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Object", "DontDestroyOnLoad", 1, &dMI);
    if (dont) ((void (*)(void *, const void *))dont)(go, dMI);

    void *src = AddComponent(go, il2cpp_type_get_object(il2cpp_class_get_type(asK)), nullptr);
    if (!src) { STATUS("FAIL: AddComponent(AudioSource)"); return; }

    void *m1 = nullptr, *m2 = nullptr, *m3 = nullptr, *m4 = nullptr, *m5 = nullptr, *m6 = nullptr;
    void *setClip = MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioSource", "set_clip", 1, &m1);
    void *setVol  = MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioSource", "set_volume", 1, &m2);
    void *setBlend= MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioSource", "set_spatialBlend", 1, &m3);
    void *setIgn  = MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioSource", "set_ignoreListenerPause", 1, &m4);
    void *setLoop = MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioSource", "set_loop", 1, &m5);
    void *play    = MethodOf("UnityEngine.AudioModule", "UnityEngine", "AudioSource", "Play", 0, &m6);
    if (!setClip || !play) { STATUS("FAIL: AudioSource.set_clip/Play not found"); return; }
    ((void (*)(void *, void *, const void *))setClip)(src, clip, m1);
    if (setVol)   ((void (*)(void *, float, const void *))setVol)(src, 1.0f, m2);
    if (setBlend) ((void (*)(void *, float, const void *))setBlend)(src, 0.0f, m3);   // 2D sound
    if (setIgn)   ((void (*)(void *, bool, const void *))setIgn)(src, true, m4);
    if (setLoop)  ((void (*)(void *, bool, const void *))setLoop)(src, false, m5);
    ((void (*)(void *, const void *))play)(src, m6);
    LOG("sound playing");
}

// ---------------------------------------------------------------- Granny skin swap (unchanged)
// Replaces the texture of every Material that uses Granny's texture with our own image.
static const char *SKIN_FILE    = "lisovskiy.png";
static const char *SKIN_TARGET  = "GrannyTexturePainted2";   // prefix of the texture's in-game name
static const bool  SKIN_ON_SHOW = true;                       // also swap when "Show image" is clicked
static void *g_skinTex = nullptr;
static bool  g_skinApplied = false;

static std::string NameOf(void *obj) {                        // UnityEngine.Object.name
    static void *fn = nullptr, *mi = nullptr;
    if (!fn) fn = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Object", "get_name", 0, &mi);
    if (!fn || !obj) return {};
    return il2cppToStd(((void *(*)(void *, const void *))fn)(obj, mi));
}

// all loaded objects of a Unity class, as an il2cpp array (length @+0x18, items @+0x20)
static void *FindAllOfType(const char *asmName, const char *ns, const char *cls) {
    void *img = ImageOf(asmName);
    void *k = img ? il2cpp_class_from_name(img, ns, cls) : nullptr;
    if (!k) return nullptr;
    void *typeObj = il2cpp_type_get_object(il2cpp_class_get_type(k));
    void *mi = nullptr;
    void *fn = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Resources", "FindObjectsOfTypeAll", 1, &mi);
    if (!fn) fn = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Object", "FindObjectsOfType", 1, &mi);
    if (!fn || !typeObj) return nullptr;
    return ((void *(*)(void *, const void *))fn)(typeObj, mi);
}

static void ApplyGrannySkin() {
    g_st = g_status2;
    if (!g_skinTex) {
        g_skinTex = LoadTextureFile(SKIN_FILE);
        if (!g_skinTex) return;                                // LoadTextureFile already wrote why
    }

    void *mats = FindAllOfType("UnityEngine.CoreModule", "UnityEngine", "Material");
    if (!mats) { STATUS("FAIL: cannot list Materials (Resources.FindObjectsOfTypeAll missing)"); return; }

    void *gmtMI = nullptr, *smtMI = nullptr;
    void *gmt = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Material", "get_mainTexture", 0, &gmtMI);
    void *smt = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Material", "set_mainTexture", 1, &smtMI);
    if (!gmt || !smt) { STATUS("FAIL: Material.get/set_mainTexture not found"); return; }

    uintptr_t n = *(uintptr_t *)((char *)mats + 0x18);
    void **items = (void **)((char *)mats + 0x20);
    if (n > 200000) { STATUS("FAIL: bad material array (%lu)", (unsigned long)n); return; }

    int swapped = 0, hints = 0;
    std::string hint;
    for (uintptr_t i = 0; i < n; i++) {
        void *m = items[i];
        if (!m) continue;
        void *t = ((void *(*)(void *, const void *))gmt)(m, gmtMI);
        if (!t) continue;
        std::string tn = NameOf(t);
        if (tn.compare(0, strlen(SKIN_TARGET), SKIN_TARGET) == 0) {
            ((void (*)(void *, void *, const void *))smt)(m, g_skinTex, smtMI);
            swapped++;
        } else if (hints < 3 && tn.find("Granny") != std::string::npos && hint.find(tn) == std::string::npos) {
            hint += tn + " ";
            hints++;
        }
    }

    if (swapped > 0) {
        g_skinApplied = true;
        STATUS("skin OK: swapped %d material(s) (of %lu)", swapped, (unsigned long)n);
    } else if (g_skinApplied) {
        STATUS("skin already applied (no material uses '%s' any more)", SKIN_TARGET);
    } else {
        STATUS("skin: no material uses '%s*' (of %lu). Granny textures seen: %s", SKIN_TARGET, (unsigned long)n,
               hint.empty() ? "(none - is Granny loaded in this scene?)" : hint.c_str());
    }
}

// ---------------------------------------------------------------- game glue
static void *g_yeduTex = nullptr;

// Add FPSDisplay to the teddy's GameObject so its OnGUI (which we hook) is guaranteed to run.
static void ensureHost(void *teddyBehaviour) {
    if (g_hostAdded) return;
    void *dom   = il2cpp_domain_get();
    void *asmbl = il2cpp_domain_assembly_open(dom, "Assembly-CSharp");
    void *img   = asmbl ? il2cpp_assembly_get_image(asmbl) : nullptr;
    void *klass = img ? il2cpp_class_from_name(img, "", "FPSDisplay") : nullptr;
    if (!klass) { LOG("FPSDisplay class not found"); return; }
    void *typeObj = il2cpp_type_get_object(il2cpp_class_get_type(klass));
    void *go = get_gameObject(teddyBehaviour, nullptr);
    if (go && typeObj) { AddComponent(go, typeObj, nullptr); g_hostAdded = true; LOG("host component added"); }
}

// ---------------------------------------------------------------- hooks
static void (*orig_OnTriggerEnter)(void *, void *, const void *) = nullptr;
static void hook_OnTriggerEnter(void *self, void *other, const void *mi) {
    if (self && other) {
        std::string tag = il2cppToStd(get_tag(other, nullptr));
        LOG("teddyInVagga.OnTriggerEnter other.tag='%s'", tag.c_str());
        snprintf(g_lastTag, sizeof g_lastTag, "%s", tag.c_str());
        if (TEDDY_TAG[0] == 0 || tag == TEDDY_TAG) {
            g_teddyPlaced = true;
            g_showMenu    = true;
            ensureHost(self);
            LOG("Teddy placed -> menu on");
        }
    }
    orig_OnTriggerEnter(self, other, mi);
}

// EnemyAIGranny.Playercaught() - runs when Granny catches the player
static void *(*orig_Playercaught)(void *, const void *) = nullptr;
static void *hook_Playercaught(void *self, const void *mi) {
    if (g_armed && !g_prompt && !g_yes) {
        g_armed  = false;
        g_prompt = true;
        set_timeScale(0.0f);                          // freeze the game, GUI keeps working
        LOG("Player caught -> timeScale 0, prompt shown");
    }
    return orig_Playercaught(self, mi);
}

static void DrawMenu() {
    float s = Screen_height(nullptr) / 540.0f;        // scale so text/buttons are phone-sized
    Matrix4x4 sc{}; sc.m[0] = sc.m[5] = sc.m[10] = s; sc.m[15] = 1.0f;
    GUI_set_matrix(sc, nullptr);

    GUI_Box(Rect{40, 20, 300, 500}, Str("Mod Menu"), nullptr);
    GUI_Label(Rect{55, 50, 270, 30}, Str(g_teddyPlaced ? "Teddy placed: YES" : "Teddy placed: NO"), nullptr);

    std::string t = std::string("Last tag: ") + g_lastTag;      // shows what entered the trigger
    GUI_Label(Rect{55, 70, 270, 30}, Str(t.c_str()), nullptr);

    GUI_Label(Rect{55, 95, 270, 30}, Str(g_armed ? "Trap: ARMED (waiting for Granny)" : "Trap: not armed"), nullptr);

    // arm the trap: preload image + sound, so any problem shows up right now
    if (GUI_Button(Rect{55, 125, 270, 40}, Str("Show image"), nullptr)) {
        if (SKIN_ON_SHOW) ApplyGrannySkin();              // skin first, so its status is not overwritten
        g_st = g_status;
        if (!g_yeduTex) g_yeduTex = LoadTextureFile(IMAGE_FILE);
        g_st = g_status3;
        if (!g_clip) g_clip = LoadClipFile(SOUND_FILE);
        g_armed = true;
        LOG("trap armed");
    }

    if (GUI_Button(Rect{55, 171, 270, 40}, Str("Granny skin"), nullptr))
        ApplyGrannySkin();

    if (GUI_Button(Rect{55, 217, 270, 40}, Str("Close"), nullptr))
        g_showMenu = false;                           // reappears next time the teddy is placed

    GUI_Label(Rect{55, 265, 270, 70}, Str(g_status), nullptr);     // image status (word-wrapped)
    GUI_Label(Rect{55, 340, 270, 70}, Str(g_status2), nullptr);    // skin status
    GUI_Label(Rect{55, 415, 270, 90}, Str(g_status3), nullptr);    // sound status

    Matrix4x4 id{}; id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1.0f;
    GUI_set_matrix(id, nullptr);                      // restore so the game's own GUI isn't scaled
}

static void DrawPrompt() {
    float s = Screen_height(nullptr) / 540.0f;
    float vw = ScreenWidth() / s;                     // virtual width after scaling
    Matrix4x4 sc{}; sc.m[0] = sc.m[5] = sc.m[10] = s; sc.m[15] = 1.0f;
    GUI_set_matrix(sc, nullptr);

    float bw = 360, bh = 180;
    float bx = (vw - bw) / 2, by = (540 - bh) / 2;
    GUI_Box(Rect{bx, by, bw, bh}, Str(""), nullptr);
    GUI_Label(Rect{bx + 20, by + 25, bw - 40, 50}, Str("Хочеш штоб тебя сьел дрон?"), nullptr);

    if (GUI_Button(Rect{bx + 30, by + 100, 130, 50}, Str("Да"), nullptr)) {
        g_yes    = true;
        g_prompt = false;
        g_yesAt  = nowMs();
        g_soundPlayed = false;
        LOG("answer: yes");
    }
    if (GUI_Button(Rect{bx + bw - 160, by + 100, 130, 50}, Str("Нет"), nullptr)) {
        g_prompt = false;
        set_timeScale(1.0f);                          // just go on playing
        LOG("answer: no -> timeScale 1");
    }

    Matrix4x4 id{}; id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1.0f;
    GUI_set_matrix(id, nullptr);
}

static void DrawFinal() {
    if (!g_soundPlayed) {                             // start the sound once
        g_soundPlayed = true;
        g_st = g_status3;
        if (!g_clip) g_clip = LoadClipFile(SOUND_FILE);
        PlayClip(g_clip);
    }
    if (!g_yeduTex) { g_st = g_status; g_yeduTex = LoadTextureFile(IMAGE_FILE); }

    Matrix4x4 id{}; id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1.0f;
    GUI_set_matrix(id, nullptr);                      // real screen pixels
    Rect full{0, 0, (float)ScreenWidth(), (float)Screen_height(nullptr)};
    if (g_yeduTex)
        GUI_DrawTexture(full, g_yeduTex, 0 /*StretchToFill*/, true, 0.0f, Color{1, 1, 1, 1}, 0.0f, 0.0f, nullptr);

    if (nowMs() - g_yesAt >= TEXT_DELAY_MS) {         // 3 s later: the text, forever
        float k = Screen_height(nullptr) / 540.0f * 3.0f;
        Matrix4x4 sc{}; sc.m[0] = sc.m[5] = sc.m[10] = k; sc.m[15] = 1.0f;
        GUI_set_matrix(sc, nullptr);
        float vw = ScreenWidth() / k;
        float bw = 150, bh = 34;
        float bx = (vw - bw) / 2, by = (180 - bh) / 2;
        GUI_Box(Rect{bx, by, bw, bh}, Str(""), nullptr);                  // dark backing for readability
        GUI_Label(Rect{bx + 14, by + 7, bw - 20, 24}, Str("Ти теперь щастлив"), nullptr);
        GUI_set_matrix(id, nullptr);
    }
}

static void (*orig_FPS_OnGUI)(void *, const void *) = nullptr;
static void hook_FPS_OnGUI(void *self, const void *mi) {
    if (g_yes)         { DrawFinal();  return; }      // final screen stays forever
    if (g_prompt)      { DrawPrompt(); return; }      // question while the game is frozen
    if (g_showMenu)      DrawMenu();                  // we are inside an OnGUI call here
    // orig_FPS_OnGUI intentionally NOT called -> FPS counter is hidden
}

// ============================================================================
// GOLDA MODE
//   * Finds the difficulty Dropdown (the one that has a "Hard" option) and adds a
//     "Golda" option. Done in Dropdown.Start and again in Dropdown.Show, so it
//     appears even if the game rebuilds the list.
//   * Choosing "Golda" tells the game "Hard" (so Hard's normal logic runs), but
//     keeps "Golda" shown in the dropdown and sets g_golda = true.
//   * While g_golda is on, every EnemyAIGranny.FixedUpdate multiplies Granny's
//     speed fields by GOLDA_MULT (2x Hard). It re-bases if the game overwrites a
//     value, so the multiplier never compounds.
// ============================================================================
constexpr uintptr_t RVA_DD_Start       = 0x2B659C4;   // Dropdown.Start
constexpr uintptr_t RVA_DD_Show        = 0x2B66784;   // Dropdown.Show
constexpr uintptr_t RVA_DD_set_value   = 0x2B6558C;   // Dropdown.set_value(int)
constexpr uintptr_t RVA_DD_get_options = 0x2B647E4;   // Dropdown.get_options() -> List<OptionData>
constexpr uintptr_t RVA_DD_SetNoNotify = 0x2B656C8;   // Dropdown.SetValueWithoutNotify(int)
constexpr uintptr_t RVA_DD_Refresh     = 0x2B64804;   // Dropdown.RefreshShownValue()
constexpr uintptr_t RVA_OD_get_text    = 0x2B68458;   // OptionData.get_text()
constexpr uintptr_t RVA_OD_ctor_str    = 0x2B65E70;   // OptionData..ctor(string)
constexpr uintptr_t RVA_Granny_FixedUpdate = 0x14549E4; // EnemyAIGranny.FixedUpdate()

static const char *GOLDA_NAME = "Golda";
static const float GOLDA_MULT = 2.0f;                  // 2x Hard mode speed
static std::atomic<bool> g_golda{false};

static void *(*dd_get_options)(void *, const void *)       = nullptr;
static void  (*dd_set_nonotify)(void *, int, const void *) = nullptr;
static void  (*dd_refresh)(void *, const void *)           = nullptr;
static void *(*od_get_text)(void *, const void *)          = nullptr;
static void  (*od_ctor_str)(void *, void *, const void *)  = nullptr;

static void *(*il2cpp_object_get_class)(void *)                          = nullptr;
static void *(*il2cpp_class_get_field_from_name)(void *, const char *)   = nullptr;
static int   (*il2cpp_field_get_offset)(void *)                          = nullptr;

// ---- List<T> / array helpers (il2cpp, 64-bit): List._items @0x10, _size @0x18; array items @0x20
static void *ListAt(void *list, int i) {
    void *arr = *(void **)((char *)list + 0x10);
    return arr ? ((void **)((char *)arr + 0x20))[i] : nullptr;
}
static int ListSize(void *list) { return list ? *(int *)((char *)list + 0x18) : 0; }

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}
static std::string OptText(void *opt) { return opt ? Lower(il2cppToStd(od_get_text(opt, nullptr))) : std::string(); }

// finds "Hard" and "Golda" indexes; returns false if this is not the difficulty dropdown
static bool ScanDropdown(void *dd, int &hard, int &golda) {
    hard = golda = -1;
    void *list = dd_get_options(dd, nullptr);
    int n = ListSize(list);
    if (!list || n <= 0 || n > 64) return false;
    for (int i = 0; i < n; i++) {
        std::string t = OptText(ListAt(list, i));
        if (t == Lower(GOLDA_NAME)) golda = i;
        else if (hard < 0 && t.find("hard") != std::string::npos && t.find("extreme") == std::string::npos) hard = i;
    }
    return hard >= 0;
}

static void EnsureGolda(void *dd) {
    if (!dd) return;
    int hard, golda;
    if (!ScanDropdown(dd, hard, golda) || golda >= 0) return;          // not difficulty list, or already added
    void *list = dd_get_options(dd, nullptr);
    void *first = ListAt(list, 0);
    void *odK = first ? il2cpp_object_get_class(first) : nullptr;      // OptionData class
    void *listK = il2cpp_object_get_class(list);                       // List<OptionData> class
    void *addMI = listK ? il2cpp_class_get_method_from_name(listK, "Add", 1) : nullptr;
    if (!odK || !addMI) { LOG("Golda: cannot find OptionData/List.Add"); return; }
    void *opt = il2cpp_object_new(odK);
    od_ctor_str(opt, Str(GOLDA_NAME), nullptr);
    ((void (*)(void *, void *, const void *))(*(void **)addMI))(list, opt, addMI);
    dd_refresh(dd, nullptr);
    LOG("Golda option added (total %d)", ListSize(list));
}

// ---- Dropdown hooks
static void (*orig_DD_Start)(void *, const void *) = nullptr;
static void hook_DD_Start(void *self, const void *mi) { if (orig_DD_Start) orig_DD_Start(self, mi); EnsureGolda(self); }

static void (*orig_DD_Show)(void *, const void *) = nullptr;
static void hook_DD_Show(void *self, const void *mi) { EnsureGolda(self); if (orig_DD_Show) orig_DD_Show(self, mi); }

static void (*orig_DD_set_value)(void *, int, const void *) = nullptr;
static void hook_DD_set_value(void *self, int v, const void *mi) {
    int hard, golda;
    if (self && ScanDropdown(self, hard, golda)) {
        if (golda >= 0 && v == golda) {
            g_golda = true;
            LOG("Golda selected -> game gets Hard (%d)", hard);
            orig_DD_set_value(self, hard, mi);          // game's callback sees "Hard"
            dd_set_nonotify(self, golda, nullptr);      // but the dropdown keeps showing "Golda"
            return;
        }
        g_golda = false;
    }
    orig_DD_set_value(self, v, mi);
}

// ---- Granny speed scaling
static const char *SPEED_FIELDS[6] = {"walkSpeed", "walkAnimSpeed", "grannysFollowSpeed",
                                      "grannysAnimFollowSpeed", "grannysVarSpeed", "grannysAnimSpeed"};
static int  g_off[6];
static bool g_offTried = false, g_offOk = false;
struct GState { float last[6]; bool init = false; };
static std::unordered_map<void *, GState> g_gstates;

static void ResolveGrannyFields() {
    g_offTried = true;
    void *img = ImageOf("Assembly-CSharp");
    void *k = img ? il2cpp_class_from_name(img, "", "EnemyAIGranny") : nullptr;
    if (!k || !il2cpp_class_get_field_from_name || !il2cpp_field_get_offset) { LOG("Golda: EnemyAIGranny not found"); return; }
    for (int i = 0; i < 6; i++) {
        void *f = il2cpp_class_get_field_from_name(k, SPEED_FIELDS[i]);
        if (!f) { LOG("Golda: field %s not found", SPEED_FIELDS[i]); return; }
        g_off[i] = il2cpp_field_get_offset(f);
    }
    g_offOk = true;
}

static void (*orig_Granny_FixedUpdate)(void *, const void *) = nullptr;
static void hook_Granny_FixedUpdate(void *self, const void *mi) {
    if (g_golda && self) {
        if (!g_offTried) ResolveGrannyFields();
        if (g_offOk) {
            GState &s = g_gstates[self];
            for (int i = 0; i < 6; i++) {
                float *p = (float *)((char *)self + g_off[i]);
                if (!s.init || *p != s.last[i]) {        // game (re)set the value -> take it as the new Hard base
                    s.last[i] = *p * GOLDA_MULT;
                    *p = s.last[i];
                }
            }
            s.init = true;
        }
    } else if (!g_golda && !g_gstates.empty()) {
        g_gstates.clear();
    }
    orig_Granny_FixedUpdate(self, mi);
}

// hook with logging + failure check: if Dobby fails, orig stays null and we must NOT keep our hook logic running
static bool SafeHook(const char *name, uintptr_t addr, void *repl, void **orig) {
    LOG("Golda: hooking %s @ %p ...", name, (void *)addr);      // if the log ends on this line, THIS hook crashed
    int r = DobbyHook((void *)addr, repl, orig);
    LOG("Golda: %s -> %d (orig=%p)", name, r, orig ? *orig : nullptr);
    return r == 0 && orig && *orig;
}

static void golda_install(uintptr_t base, void *h) {
#define RES2(n) *(void **)&n = dlsym(h, #n)
    RES2(il2cpp_object_get_class); RES2(il2cpp_class_get_field_from_name); RES2(il2cpp_field_get_offset);
#undef RES2
    if (!il2cpp_object_get_class || !il2cpp_class_get_method_from_name || !il2cpp_object_new || !il2cpp_string_new) {
        LOG("Golda: il2cpp exports missing, Golda disabled"); return;
    }
    dd_get_options  = (decltype(dd_get_options))(base + RVA_DD_get_options);
    dd_set_nonotify = (decltype(dd_set_nonotify))(base + RVA_DD_SetNoNotify);
    dd_refresh      = (decltype(dd_refresh))(base + RVA_DD_Refresh);
    od_get_text     = (decltype(od_get_text))(base + RVA_OD_get_text);
    od_ctor_str     = (decltype(od_ctor_str))(base + RVA_OD_ctor_str);

    // one at a time; a hook that fails to install is simply skipped (no null orig_* call -> no crash)
    bool ok = true;
    ok &= SafeHook("Dropdown.Start",     base + RVA_DD_Start,     (void *)hook_DD_Start,     (void **)&orig_DD_Start);
    ok &= SafeHook("Dropdown.Show",      base + RVA_DD_Show,      (void *)hook_DD_Show,      (void **)&orig_DD_Show);
    ok &= SafeHook("Dropdown.set_value", base + RVA_DD_set_value, (void *)hook_DD_set_value, (void **)&orig_DD_set_value);
    ok &= SafeHook("Granny.FixedUpdate", base + RVA_Granny_FixedUpdate, (void *)hook_Granny_FixedUpdate, (void **)&orig_Granny_FixedUpdate);
    LOG(ok ? "Golda hooks installed" : "Golda: some hooks FAILED (see log above)");
}

// ---------------------------------------------------------------- init
static void *init_thread(void *) {
    uintptr_t base;
    while ((base = getLibBase("libil2cpp.so")) == 0) sleep(1);
    sleep(3);
    LOG("libil2cpp base = %p", (void *)base);

    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
#define RES(n) *(void **)&n = dlsym(h, #n)
    RES(il2cpp_domain_get); RES(il2cpp_domain_assembly_open); RES(il2cpp_assembly_get_image);
    RES(il2cpp_class_from_name); RES(il2cpp_class_get_type); RES(il2cpp_type_get_object);
    RES(il2cpp_string_new);
    RES(il2cpp_object_new); RES(il2cpp_class_get_method_from_name); RES(il2cpp_get_corlib);
    RES(il2cpp_array_new); RES(il2cpp_gchandle_new);
#undef RES
    if (!il2cpp_string_new || !il2cpp_class_from_name)
        LOG("il2cpp exports missing (stripped?) - need another way to make strings/find FPSDisplay");
    if (!il2cpp_object_new || !il2cpp_array_new || !il2cpp_get_corlib || !il2cpp_class_get_method_from_name)
        LOG("exports for image/sound loading missing - they will not work");

    get_tag         = (decltype(get_tag))(base + RVA_get_tag);
    get_gameObject  = (decltype(get_gameObject))(base + RVA_get_gameObject);
    AddComponent    = (decltype(AddComponent))(base + RVA_AddComponent);
    Screen_height   = (decltype(Screen_height))(base + RVA_Screen_height);
    GUI_Box         = (decltype(GUI_Box))(base + RVA_GUI_Box);
    GUI_Label       = (decltype(GUI_Label))(base + RVA_GUI_Label);
    GUI_Button      = (decltype(GUI_Button))(base + RVA_GUI_Button);
    GUI_set_matrix  = (decltype(GUI_set_matrix))(base + RVA_GUI_set_matrix);
    set_timeScale   = (decltype(set_timeScale))(base + RVA_set_timeScale);
    GUI_DrawTexture = (decltype(GUI_DrawTexture))(base + RVA_GUI_DrawTexture);

    DobbyHook((void *)(base + RVA_OnTriggerEnter), (void *)hook_OnTriggerEnter, (void **)&orig_OnTriggerEnter);
    DobbyHook((void *)(base + RVA_FPS_OnGUI),      (void *)hook_FPS_OnGUI,      (void **)&orig_FPS_OnGUI);
    DobbyHook((void *)(base + RVA_Playercaught),   (void *)hook_Playercaught,   (void **)&orig_Playercaught);
    LOG("hooks installed");
    sleep(12);                                    // let the first scene finish loading before patching more code
    golda_install(base, h);                       // Golda difficulty + 2x Granny speed
    return nullptr;
}

__attribute__((constructor)) static void lib_main() {
    pthread_t t;
    pthread_create(&t, nullptr, init_thread, nullptr);
}
