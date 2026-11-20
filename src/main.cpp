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
// Link: -llog -ldl     Hook lib: Dobby

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
#include "dobby.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "TeddyMod", __VA_ARGS__)

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
        if (readFile(p, bytes)) { LOG("image read: %s (%zu bytes)", p.c_str(), bytes.size()); break; }
    if (bytes.empty()) { LOG("image file not found (tried %s ...)", cands[0].c_str()); return false; }

    if (!il2cpp_object_new || !il2cpp_array_new || !il2cpp_get_corlib || !il2cpp_class_get_method_from_name) {
        LOG("missing il2cpp exports for texture loading");
        return false;
    }

    // byte[] with the file contents (arm64: array data starts at +0x20)
    void *byteK = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");
    if (!byteK) { LOG("System.Byte not found"); return false; }
    void *arr = il2cpp_array_new(byteK, bytes.size());
    if (!arr) return false;
    memcpy((char *)arr + 0x20, bytes.data(), bytes.size());

    // new Texture2D(2, 2)
    void *coreImg = ImageOf("UnityEngine.CoreModule");
    void *texK    = coreImg ? il2cpp_class_from_name(coreImg, "UnityEngine", "Texture2D") : nullptr;
    if (!texK) { LOG("Texture2D class not found"); return false; }
    void *ctorMI = nullptr;
    void *ctor = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Texture2D", ".ctor", 2, &ctorMI);
    void *tex = il2cpp_object_new(texK);
    if (!ctor || !tex) return false;
    ((void (*)(void *, int, int, const void *))ctor)(tex, 2, 2, ctorMI);

    // ImageConversion.LoadImage(tex, bytes)
    void *liMI = nullptr;
    void *li = MethodOf("UnityEngine.ImageConversionModule", "UnityEngine", "ImageConversion", "LoadImage", 2, &liMI);
    if (!li) return false;
    bool ok = ((bool (*)(void *, void *, const void *))li)(tex, arr, liMI);
    if (!ok) { LOG("LoadImage failed (bad image file?)"); return false; }

    // keep it alive: HideAndDontSave + GC handle
    void *hfMI = nullptr;
    void *hf = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Object", "set_hideFlags", 1, &hfMI);
    if (hf) ((void (*)(void *, int, const void *))hf)(tex, 61, hfMI);   // HideFlags.HideAndDontSave
    if (il2cpp_gchandle_new) il2cpp_gchandle_new(tex, false);

    g_tex = tex;
    LOG("texture created");
    return true;
}

static int ScreenWidth() {
    static void *fn = nullptr, *mi = nullptr;
    if (!fn) fn = MethodOf("UnityEngine.CoreModule", "UnityEngine", "Screen", "get_width", 0, &mi);
    return fn ? ((int (*)(const void *))fn)(mi) : (int)(Screen_height(nullptr) * 16 / 9);
}

static void DrawScare() {
    if (!g_tex) return;
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

    GUI_Box(Rect{40, 40, 300, 310}, Str("Mod Menu"), nullptr);
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
