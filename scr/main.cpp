// teddy_unitygui.cpp - arm64 il2cpp (Slendrina): Dobby + Unity's built-in IMGUI (GUI.Box/Label/Button)
// Menu appears when the teddy is placed. No ImGui, no EGL hook, touch works through Unity itself.
//
// Unity GUI calls only work inside an OnGUI() callback. We have no OnGUI of our own, so we
// borrow the game's own FPSDisplay.OnGUI: when the teddy is placed we AddComponent(FPSDisplay)
// onto the teddy object (guarantees one exists) and hook its OnGUI to draw the menu.
//
// RVAs from your dump.cs:
//   teddyInVagga.OnTriggerEnter = 0x15531F8      FPSDisplay.OnGUI        = 0x144D828
//   Component.get_tag           = 0x2A9ACD0      Component.get_gameObject= 0x2A9A678
//   GameObject.AddComponent(Type)= 0x2A9DEB0     Screen.get_height       = 0x2A76984
//   GUI.Box(Rect,string)        = 0x2ACA434      GUI.Label(Rect,string)  = 0x2AC90AC
//   GUI.Button(Rect,string)     = 0x2ACA814      GUI.set_matrix(Matrix4x4)= 0x2AC9024
//
// Link: -llog -ldl     Hook lib: Dobby

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
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

// Set after reading logcat (filter "TeddyMod") when you place the teddy. "Teddy" is a guess.
static const char *TEDDY_TAG = "Teddy";

static std::atomic<bool> g_teddyPlaced{false};
static std::atomic<bool> g_showMenu{false};
static bool g_hostAdded = false;

// ---------------------------------------------------------------- types
struct Rect { float x, y, w, h; };
struct Matrix4x4 { float m[16]; };

static void *(*get_tag)(void *, const void *)            = nullptr;
static void *(*get_gameObject)(void *, const void *)     = nullptr;
static void *(*AddComponent)(void *, void *, const void *) = nullptr;
static int   (*Screen_height)(const void *)              = nullptr;
static void  (*GUI_Box)(Rect, void *, const void *)      = nullptr;
static void  (*GUI_Label)(Rect, void *, const void *)    = nullptr;
static bool  (*GUI_Button)(Rect, void *, const void *)    = nullptr;
static void  (*GUI_set_matrix)(Matrix4x4, const void *)  = nullptr;

// il2cpp exports (resolved with dlsym)
static void *(*il2cpp_domain_get)()                                  = nullptr;
static void *(*il2cpp_domain_assembly_open)(void *, const char *)    = nullptr;
static void *(*il2cpp_assembly_get_image)(void *)                    = nullptr;
static void *(*il2cpp_class_from_name)(void *, const char *, const char *) = nullptr;
static void *(*il2cpp_class_get_type)(void *)                        = nullptr;
static void *(*il2cpp_type_get_object)(void *)                       = nullptr;
static void *(*il2cpp_string_new)(const char *)                      = nullptr;

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
        LOG("teddyInVagga.OnTriggerEnter other.tag='%s'", tag.c_str());   // read once in logcat
        if (tag == TEDDY_TAG) {
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

    GUI_Box(Rect{40, 40, 300, 230}, Str("Mod Menu"), nullptr);
    GUI_Label(Rect{55, 70, 270, 30}, Str(g_teddyPlaced ? "Teddy placed: YES" : "Teddy placed: NO"), nullptr);

    static bool example = false;
    if (GUI_Button(Rect{55, 110, 270, 50}, Str(example ? "Example: ON" : "Example: OFF"), nullptr))
        example = !example;                           // add your features here

    if (GUI_Button(Rect{55, 190, 270, 50}, Str("Close"), nullptr))
        g_showMenu = false;                           // reappears next time the teddy is placed

    Matrix4x4 id{}; id.m[0] = id.m[5] = id.m[10] = id.m[15] = 1.0f;
    GUI_set_matrix(id, nullptr);                      // restore so the game's own GUI isn't scaled
}

static void (*orig_FPS_OnGUI)(void *, const void *) = nullptr;
static void hook_FPS_OnGUI(void *self, const void *mi) {
    if (g_showMenu) DrawMenu();                       // we are inside an OnGUI call here
    orig_FPS_OnGUI(self, mi);
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
#undef RES
    if (!il2cpp_string_new || !il2cpp_class_from_name)
        LOG("il2cpp exports missing (stripped?) - need another way to make strings/find FPSDisplay");

    get_tag        = (decltype(get_tag))(base + RVA_get_tag);
    get_gameObject = (decltype(get_gameObject))(base + RVA_get_gameObject);
    AddComponent   = (decltype(AddComponent))(base + RVA_AddComponent);
    Screen_height  = (decltype(Screen_height))(base + RVA_Screen_height);
    GUI_Box        = (decltype(GUI_Box))(base + RVA_GUI_Box);
    GUI_Label      = (decltype(GUI_Label))(base + RVA_GUI_Label);
    GUI_Button     = (decltype(GUI_Button))(base + RVA_GUI_Button);
    GUI_set_matrix = (decltype(GUI_set_matrix))(base + RVA_GUI_set_matrix);

    DobbyHook((void *)(base + RVA_OnTriggerEnter), (void *)hook_OnTriggerEnter, (void **)&orig_OnTriggerEnter);
    DobbyHook((void *)(base + RVA_FPS_OnGUI),      (void *)hook_FPS_OnGUI,      (void **)&orig_FPS_OnGUI);
    LOG("hooks installed");
    return nullptr;
}

__attribute__((constructor)) static void lib_main() {
    pthread_t t;
    pthread_create(&t, nullptr, init_thread, nullptr);
}
