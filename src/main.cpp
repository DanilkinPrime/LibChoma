// teddy_unitygui.cpp - arm64 il2cpp (Slendrina): Dobby + Unity's built-in IMGUI
// Menu appears when the teddy is placed.
//   * Speed selector 1x / 2x / 3x (Time.timeScale), FPS counter hidden
//   * "Show image" button -> full-screen image (jumpscare) for 1.5 seconds, loaded from a file
//
// Image file (PNG/JPG), put it in the game's own files folder (no permission needed):
//   /sdcard/Android/data/<package>/files/jumpscare.png
// (also tried: /data/data/<package>/files/jumpscare.png)
//
// Unity GUI calls only work inside an OnGUI() callback. We borrow the game's own FPSDisplay.OnGUI:
// when the teddy is placed we AddComponent(FPSDisplay) onto the teddy object and hook its OnGUI.
//
// Everything that touches Unity objects (texture creation, drawing) runs inside that OnGUI hook,
// i.e. on Unity's main thread. Extra methods (Texture2D ctor, ImageConversion.LoadImage,
// Screen.get_width, Object.set_hideFlags) are looked up by NAME through il2cpp, so no new RVAs
// are needed. Only GUI.DrawTexture (8-arg overload) uses an RVA from your dump.
//
// Link: -llog -ldl -lz     Hook lib: Dobby

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <zlib.h>
#include <cstdlib>
#include "dobby.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "TeddyMod", __VA_ARGS__)

// on-screen status line (shown in the menu) - so you can debug without logcat
static char g_status[300] = "status: idle";
#define STATUS(...) do { snprintf(g_status, sizeof g_status, __VA_ARGS__); LOG("%s", g_status); } while (0)

constexpr uintptr_t RVA_OnTriggerEnter = 0x15531F8;
constexpr uintptr_t RVA_FPS_OnGUI      = 0x144D828;
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
static const char *IMAGE_FILE  = "jumpscare.png";
static const int   SCARE_MS    = 1500;               // how long the image stays on screen
static char g_lastTag[64] = "(none yet)";

static std::atomic<bool> g_teddyPlaced{false};
static std::atomic<bool> g_showMenu{false};
static std::atomic<long long> g_scareUntil{0};       // steady-clock ms; image shown while now < this
static bool g_hostAdded = false;
static int  g_speed = 1;                              // 1, 2 or 3

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

static void *Str(const char *s) { return il2cpp_string_new(s); }

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// look up an il2cpp method by name; returns the native function pointer (MethodInfo's first field)
static void *ImageOf(const char *asmName) {
    void *a = il2cpp_domain_assembly_open(il2cpp_domain_get(), asmName);
    return a ? il2cpp_assembly_get_image(a) : nullptr;
}
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

// ---------------------------------------------------------------- texture loading (main thread only)
static void *g_tex = nullptr;

// ---- minimal PNG decoder (8-bit, non-interlaced; gray/RGB/palette/gray+alpha/RGBA) using zlib
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

    // un-filter
    for (int y = 0; y < H; y++) {
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

    // to RGBA (top-down)
    rgba.assign((size_t)W * H * 4, 255);
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

static bool LoadTexture() {
    if (g_tex) return true;

    std::string pkg = packageName();
    std::vector<std::string> cands = {
        "/storage/emulated/0/Android/data/" + pkg + "/files/" + IMAGE_FILE,
        "/sdcard/Android/data/" + pkg + "/files/" + IMAGE_FILE,
        "/data/data/" + pkg + "/files/" + IMAGE_FILE,
        "/data/user/0/" + pkg + "/files/" + IMAGE_FILE,
    };
    std::vector<char> bytes;
    for (auto &p : cands)
        if (readFile(p, bytes)) { STATUS("1 file read OK %zu B", bytes.size()); break; }
    if (bytes.empty()) { STATUS("FAIL: file not found/empty. pkg='%s' tried: %s", pkg.c_str(), cands[0].c_str()); return false; }

    if (!il2cpp_object_new || !il2cpp_array_new || !il2cpp_get_corlib || !il2cpp_class_get_method_from_name) {
        STATUS("FAIL: il2cpp exports missing (object_new/array_new/get_corlib/get_method_from_name)");
        return false;
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
        if (!decodePNG(bytes, W, H, rgba, err)) { STATUS("FAIL: %s", err.c_str()); return false; }
        void *coreImg = ImageOf("UnityEngine.CoreModule");
        void *c32K = coreImg ? il2cpp_class_from_name(coreImg, "UnityEngine", "Color32") : nullptr;
        void *spMI = nullptr, *apMI = nullptr;
        void *sp = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Texture2D", "SetPixels32", 1, &spMI);
        void *ap = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Texture2D", "Apply", 2, &apMI);
        if (!c32K || !sp || !ap) { STATUS("FAIL: Color32/SetPixels32/Apply not found"); return false; }
        void *t = MakeTexture(W, H);
        if (!t) return false;
        void *arr = il2cpp_array_new(c32K, (uintptr_t)W * H);
        if (!arr) { STATUS("FAIL: Color32[] alloc"); return false; }
        uint8_t *dst = (uint8_t *)arr + 0x20;
        for (int y = 0; y < H; y++)                       // Unity rows go bottom-up
            memcpy(dst + (size_t)(H - 1 - y) * W * 4, &rgba[(size_t)y * W * 4], (size_t)W * 4);
        ((void (*)(void *, void *, const void *))sp)(t, arr, spMI);
        ((void (*)(void *, bool, bool, const void *))ap)(t, true, false, apMI);
        tex = t;
        STATUS("2 texture OK via PNG decode %dx%d", W, H);
    }

    // keep it alive: HideAndDontSave + GC handle
    void *hfMI = nullptr;
    void *hf = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Object", "set_hideFlags", 1, &hfMI);
    if (hf) ((void (*)(void *, int, const void *))hf)(tex, 61, hfMI);   // HideFlags.HideAndDontSave
    if (il2cpp_gchandle_new) il2cpp_gchandle_new(tex, false);

    g_tex = tex;
    return true;
}

static int ScreenWidth() {
    static void *fn = nullptr, *mi = nullptr;
    if (!fn) fn = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Screen", "get_width", 0, &mi);
    return fn ? ((int (*)(const void *))fn)(mi) : (int)(Screen_height(nullptr) * 16 / 9);
}

static void DrawScare() {
    if (!g_tex) return;
    static bool said = false;
    if (!said) { STATUS("3 drawing image (DrawTexture)"); said = true; }
    Matrix4x4 id{}; id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1.0f;
    GUI_set_matrix(id, nullptr);                      // no scaling: use real screen pixels
    Rect full{0, 0, (float)ScreenWidth(), (float)Screen_height(nullptr)};
    GUI_DrawTexture(full, g_tex, 0 /*StretchToFill*/, true, 0.0f, Color{1, 1, 1, 1}, 0.0f, 0.0f, nullptr);
}

// ---------------------------------------------------------------- game glue
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

static void setSpeed(int s) {
    g_speed = s;
    set_timeScale((float)s);
    LOG("timeScale -> %d", s);
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

static void DrawMenu() {
    float s = Screen_height(nullptr) / 540.0f;        // scale so text/buttons are phone-sized
    Matrix4x4 sc{}; sc.m[0] = sc.m[5] = sc.m[10] = s; sc.m[15] = 1.0f;
    GUI_set_matrix(sc, nullptr);

    GUI_Box(Rect{40, 40, 300, 420}, Str("Mod Menu"), nullptr);
    GUI_Label(Rect{55, 70, 270, 30}, Str(g_teddyPlaced ? "Teddy placed: YES" : "Teddy placed: NO"), nullptr);

    std::string t = std::string("Last tag: ") + g_lastTag;      // shows what entered the trigger
    GUI_Label(Rect{55, 90, 270, 30}, Str(t.c_str()), nullptr);

    std::string sp = "Speed: x" + std::to_string(g_speed);
    GUI_Label(Rect{55, 125, 270, 30}, Str(sp.c_str()), nullptr);

    // speed selector: 1x / 2x / 3x (selected one shown in brackets)
    static const char *names[3]  = {"1x", "2x", "3x"};
    static const char *namesOn[3] = {"[1x]", "[2x]", "[3x]"};
    for (int i = 0; i < 3; i++) {
        Rect r{55.0f + i * 92.0f, 150, 85, 40};
        if (GUI_Button(r, Str(g_speed == i + 1 ? namesOn[i] : names[i]), nullptr))
            setSpeed(i + 1);
    }

    // show image: full screen for SCARE_MS milliseconds
    if (GUI_Button(Rect{55, 215, 270, 50}, Str("Show image"), nullptr)) {
        if (LoadTexture()) g_scareUntil = nowMs() + SCARE_MS;
    }

    if (GUI_Button(Rect{55, 280, 270, 50}, Str("Close"), nullptr))
        g_showMenu = false;                           // reappears next time the teddy is placed

    GUI_Label(Rect{55, 340, 270, 110}, Str(g_status), nullptr);   // debug status (word-wrapped)

    Matrix4x4 id{}; id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1.0f;
    GUI_set_matrix(id, nullptr);                      // restore so the game's own GUI isn't scaled
}

static void (*orig_FPS_OnGUI)(void *, const void *) = nullptr;
static void hook_FPS_OnGUI(void *self, const void *mi) {
    if (nowMs() < g_scareUntil) { DrawScare(); return; }   // image only, menu hidden meanwhile
    if (g_showMenu) DrawMenu();                       // we are inside an OnGUI call here
    // orig_FPS_OnGUI intentionally NOT called -> FPS counter is hidden
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
        LOG("exports for image loading missing - 'Show image' will not work");

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
    LOG("hooks installed");
    return nullptr;
}

__attribute__((constructor)) static void lib_main() {
    pthread_t t;
    pthread_create(&t, nullptr, init_thread, nullptr);
}
