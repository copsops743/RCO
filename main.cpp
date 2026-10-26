// SkinShop: локальный магазин скинов за монеты (1 монета за 10 сек в матче).
// Работает только на вашем устройстве. Сеть, PlayFab и пакеты игры не затрагиваются.
// Цель: Unity IL2CPP, armeabi-v7a. Всё ищется по именам через il2cpp API.
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include "dobby.h"

#define TAG "SkinShop"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ====================== НАСТРОЙКИ (можно менять) ======================
static const int   PRICE            = 100;   // цена каждого скина
static const float SECONDS_PER_COIN = 10.f;  // 1 монета за 10 секунд в матче

struct Item { const char* name; int slot; int team; }; // team: 0 любая, 1 красная, 2 синяя
static Item ITEMS[4] = {
    {"Skeleton Skin (Any Team)", 8, 0},
    {"Zombie Skin (Any Team)", 13, 0},
    {"Military Skin (Red Team)", 2, 1},
    {"Military Skin (Blue Team)", 3, 2},
};
// Тестеры: если testerlvl игрока >= TESTER_MIN, по умолчанию ему показывается скин 24 (красная) / 25 (синяя команда)
static const int TESTER_MIN = 3, TESTER_SLOT_RED = 24, TESTER_SLOT_BLUE = 25;
// Как по ID стандартного скина игры определить команду. Если не угадал,
// в окне магазина кнопкой "Команда" выберите вручную.
static const int RED_IDS[]  = {2, 4, 10};
static const int BLUE_IDS[] = {0, 3, 5, 11, 12};
// =======================================================================

struct Rect { float x, y, w, h; };
struct Mat  { float m[16]; };

// ---- il2cpp API ----
static void* (*il2cpp_domain_get)();
static void* (*il2cpp_domain_assembly_open)(void*, const char*);
static void* (*il2cpp_assembly_get_image)(void*);
static void* (*il2cpp_class_from_name)(void*, const char*, const char*);
static void* (*il2cpp_class_get_methods)(void*, void**);
static void* (*il2cpp_class_get_field_from_name)(void*, const char*);
static int   (*il2cpp_field_get_offset)(void*);
static const char* (*il2cpp_method_get_name)(void*);
static uint32_t (*il2cpp_method_get_param_count)(void*);
static const void* (*il2cpp_method_get_param)(void*, uint32_t);
static const char* (*il2cpp_type_get_name)(const void*);
static void* (*il2cpp_string_new)(const char*);
static void* (*il2cpp_thread_attach)(void*);
static void* (*il2cpp_object_new)(void*);
static void* (*il2cpp_value_box)(void*, void*);
static void* (*il2cpp_object_unbox)(void*);
static void* (*il2cpp_object_get_class)(void*);
static const char* (*il2cpp_class_get_name)(void*);
static const void* (*il2cpp_class_get_type)(void*);
static void* (*il2cpp_type_get_object)(const void*);

static void* find_class(const char* asmName, const char* ns, const char* name) {
    void* a = il2cpp_domain_assembly_open(il2cpp_domain_get(), asmName);
    if (!a) return nullptr;
    return il2cpp_class_from_name(il2cpp_assembly_get_image(a), ns, name);
}
// ищет метод по имени, числу параметров и (опционально) типу параметра
static void* find_method(void* k, const char* name, int argc, int pidx = -1, const char* ptype = nullptr) {
    if (!k) return nullptr;
    void* it = nullptr; void* m;
    while ((m = il2cpp_class_get_methods(k, &it))) {
        if (strcmp(il2cpp_method_get_name(m), name) || (int)il2cpp_method_get_param_count(m) != argc) continue;
        if (!ptype) return m;
        const char* tn = il2cpp_type_get_name(il2cpp_method_get_param(m, pidx));
        if (tn && !strcmp(tn, ptype)) return m;
    }
    return nullptr;
}
static void* fptr(void* methodInfo) { return methodInfo ? *(void**)methodInfo : nullptr; }
static int field_off(void* k, const char* name) {
    void* f = k ? il2cpp_class_get_field_from_name(k, name) : nullptr;
    return f ? il2cpp_field_get_offset(f) : -1;
}

// ---- найденные методы ----
static void *mi_SetSkin, *mi_getTechId, *mi_SetAll, *mi_getPhoton;
static void *mi_Button, *mi_Label, *mi_Box, *mi_getMat, *mi_setMat, *mi_scrW, *mi_scrH;
static int off_Awatar = -1, off_Ctrl = -1, off_IsLocal = -1;
// синхронизация скина между игроками с модом (Photon custom properties, ключ "ssk")
static void *kInt32, *kHashtable, *mi_HtCtor, *mi_HtSet, *mi_HtGet, *mi_PlSet, *mi_PlGet, *mi_PNLocal;
static bool g_syncOk = false, g_syncDirty = true;
static double g_lastSend = 0, g_lastTry = 0;
static int g_sentVal = -2, g_remoteSeen = -2, g_testerLvl = -1;
// главное меню: модель персонажа в меню получает наш скин
static void *kLTS, *kSetterCls, *mi_LTSget, *mi_FindObj, *mi_GetCIP, *mi_CtrlStart;
static double g_lastHudT = 0, g_ltsT = 0;
static int g_ltsLvl = -1;
static int local_tester_level();
static void *kCtrlCls, *kHandlerCls, *mi_FindObjs, *mi_ChatUpd, *mi_FriendsUpd;
static int off_Handlers = -1, off_HSkin = -1;
static double g_pumpT = 0;
struct Applied { void* p; int slot; };
static Applied g_app[32];
static int g_appN = 0;

typedef void (*fn_SetSkin)(void*, void*);
typedef void (*fn_ctor)(void*, void*);
typedef void (*fn_htset)(void*, void*, void*, void*);
typedef void* (*fn_htget)(void*, void*, void*);
typedef bool (*fn_plset)(void*, void*, void*, void*, void*);
typedef void* (*fn_stat0)(void*);
typedef void* (*fn_findobj)(void*, void*);
typedef void* (*fn_getcip)(void*, void*, void*);
typedef int (*fn_geti)(void*, void*);
typedef uint8_t (*fn_getTech)(void*, void*);
typedef void (*fn_SetAll)(void*, uint8_t, void*);
typedef void* (*fn_getPhoton)(void*, void*);
typedef bool (*fn_Button)(Rect, void*, void*);
typedef void (*fn_Label)(Rect, void*, void*);
typedef Mat  (*fn_getMat)(void*);
typedef void (*fn_setMat)(Mat, void*);
typedef int  (*fn_Scr)(void*);

// ---- состояние ----
static int  g_coins = 0;
static int  g_owned = 0;       // битовая маска купленных
static int  g_equipped = -1;   // индекс надетого или -1
static int  g_teamMode = 0;    // 0 авто, 1 красная, 2 синяя
static bool g_open = false;
static std::string g_msg;
static int  g_lastId = -1, g_lastLocal = -1, g_lastApplied = -1;
static int  g_realTeam = 0;
static std::string g_path, g_logInt, g_logExt, g_guard, g_off;
static double g_acc = 0, g_lastT = 0, g_lastSave = 0;

static double now_s() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static void blog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    LOG("%s", buf);
    const std::string* ps[2] = {&g_logInt, &g_logExt};
    for (int i = 0; i < 2; i++) {
        if (ps[i]->empty()) continue;
        FILE* f = fopen(ps[i]->c_str(), "a");
        if (f) { fprintf(f, "%s\n", buf); fclose(f); }
    }
}
static void init_path() {
    char buf[256] = {0};
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    std::string dir = std::string("/data/data/") + buf + "/files";
    mkdir(dir.c_str(), 0771);
    g_path = dir + "/skinshop.dat";
    g_guard = dir + "/skinshop.guard";
    g_logInt = dir + "/skinshop.log";
    g_logExt = std::string("/storage/emulated/0/Android/data/") + buf + "/files/skinshop.log";
    g_off = std::string("/storage/emulated/0/Android/data/") + buf + "/files/skinshop_off";
    remove(g_logInt.c_str()); remove(g_logExt.c_str());
}
static void save_state() {
    FILE* f = fopen(g_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "%d %d %d\n", g_coins, g_owned, g_equipped);
    fclose(f);
}
static void load_state() {
    FILE* f = fopen(g_path.c_str(), "r");
    if (!f) return;
    int c, o, e;
    if (fscanf(f, "%d %d %d", &c, &o, &e) == 3) { g_coins = c; g_owned = o; g_equipped = e; }
    fclose(f);
}

static int team_from_id(int id) {
    for (int v : RED_IDS) if (v == id) return 1;
    for (int v : BLUE_IDS) if (v == id) return 2;
    return 0;
}
static int cur_team() { return g_teamMode ? g_teamMode : g_realTeam; }

// ---- синхронизация скина (только между игроками с этим модом) ----
// В свойстве "ssk" лежит номер слота купленного скина (или -1).
static int purchased_slot_now() {
    if (g_equipped < 0 || !(g_owned & (1 << g_equipped))) return -1;
    const Item& it = ITEMS[g_equipped];
    return (it.team == 0 || it.team == cur_team()) ? it.slot : -1;
}
// какой скин показывать на модели меню для команды team (0 неизвестна, 1 красная, 2 синяя)
static int menu_slot_for_team(int team, int* lvOut) {
    int slot = -1;
    if (g_equipped >= 0 && (g_owned & (1 << g_equipped))) {
        const Item& it = ITEMS[g_equipped];
        if (it.team == 0 || it.team == team) slot = it.slot;
    }
    int lv = local_tester_level();
    if (lvOut) *lvOut = lv;
    if (slot < 0 && lv >= TESTER_MIN) slot = team == 1 ? TESTER_SLOT_RED : team == 2 ? TESTER_SLOT_BLUE : -1;
    return slot;
}
static bool slot_allowed(int sl) {
    if (sl == TESTER_SLOT_RED || sl == TESTER_SLOT_BLUE) return true;
    for (int i = 0; i < 4; i++) if (ITEMS[i].slot == sl) return true;
    return false;
}
// читает числовое свойство игрока Photon (int/byte/short/long)
static bool prop_int(void* photonPlayer, const char* key, int* out) {
    if (!g_syncOk || !photonPlayer) return false;
    void* props = ((fn_getPhoton)fptr(mi_PlGet))(photonPlayer, mi_PlGet);
    if (!props) return false;
    void* val = ((fn_htget)fptr(mi_HtGet))(props, il2cpp_string_new(key), mi_HtGet);
    if (!val) return false;
    const char* n = il2cpp_class_get_name(il2cpp_object_get_class(val));
    void* p = il2cpp_object_unbox(val);
    if (!n || !p) return false;
    if (!strcmp(n, "Int32")) *out = *(int32_t*)p;
    else if (!strcmp(n, "UInt32")) *out = (int)*(uint32_t*)p;
    else if (!strcmp(n, "Int16")) *out = *(int16_t*)p;
    else if (!strcmp(n, "UInt16")) *out = *(uint16_t*)p;
    else if (!strcmp(n, "Byte")) *out = *(uint8_t*)p;
    else if (!strcmp(n, "SByte")) *out = *(int8_t*)p;
    else if (!strcmp(n, "Int64")) *out = (int)*(int64_t*)p;
    else return false;
    return true;
}
static int tester_slot(void* photonPlayer, int team) {
    int lv = 0;
    if (!prop_int(photonPlayer, "testerlvl", &lv) || lv < TESTER_MIN) return -1;
    return team == 1 ? TESTER_SLOT_RED : team == 2 ? TESTER_SLOT_BLUE : -1;
}
static bool sync_send() {
    if (!g_syncOk) return false;
    void* lp = ((fn_stat0)fptr(mi_PNLocal))(mi_PNLocal);
    if (!lp) return false;
    void* ht = il2cpp_object_new(kHashtable);
    ((fn_ctor)fptr(mi_HtCtor))(ht, mi_HtCtor);
    int v = purchased_slot_now();
    void* box = il2cpp_value_box(kInt32, &v);
    ((fn_htset)fptr(mi_HtSet))(ht, il2cpp_string_new("ssk"), box, mi_HtSet);
    ((fn_plset)fptr(mi_PlSet))(lp, ht, nullptr, nullptr, mi_PlSet);
    g_sentVal = v; g_lastSend = now_s();
    return true;
}
static int sync_read(void* photonPlayer) {
    int v = -1;
    if (!prop_int(photonPlayer, "ssk", &v)) return -1;
    return (v >= 0 && v < 32) ? v : -1;
}
static void sync_tick() {
    if (!g_syncOk || !g_syncDirty) return;
    double t = now_s();
    if (t - g_lastTry < 2.0) return;
    g_lastTry = t;
    if (sync_send()) g_syncDirty = false;
}
static void set_skin_slot(void* self, int slot) {
    void* ctrl = off_Ctrl >= 0 ? *(void**)((char*)self + off_Ctrl) : nullptr;
    if (!ctrl || !mi_SetAll) return;
    ((fn_SetAll)fptr(mi_SetAll))(ctrl, (uint8_t)slot, mi_SetAll);
}

// ---- хук: CommandSkinSetter.SetSkin ----
static fn_SetSkin orig_SetSkin;
static void hook_SetSkin(void* self, void* mi) {
    orig_SetSkin(self, mi);
    if (!self) return;
    int id = mi_getTechId ? ((fn_getTech)fptr(mi_getTechId))(self, mi_getTechId) : -1;
    int local = 0;
    void* pl = nullptr;
    void* av = off_Awatar >= 0 ? *(void**)((char*)self + off_Awatar) : nullptr;
    if (av && mi_getPhoton && off_IsLocal >= 0) {
        pl = ((fn_getPhoton)fptr(mi_getPhoton))(av, mi_getPhoton);
        if (pl) local = *((uint8_t*)pl + off_IsLocal) ? 1 : 0;
    }
    static int logs = 0;
    if (!av) {      // нет аватара матча: это модель в главном меню (синяя/красная команда)
        int team = team_from_id(id), lv = -1;
        int slot = menu_slot_for_team(team, &lv);
        if (slot >= 0) set_skin_slot(self, slot);
        if (logs++ < 40) blog("SetSkin(меню): id=%d team=%d tester=%d -> слот %d", id, team, lv, slot);
        return;
    }
    if (!local) {   // чужой игрок: купленный скин (если у него наш мод), иначе скин тестера
        int v = sync_read(pl);
        g_remoteSeen = v;
        int slot = (v >= 0 && slot_allowed(v)) ? v : tester_slot(pl, team_from_id(id));
        if (slot >= 0) set_skin_slot(self, slot);
        if (logs++ < 40) blog("SetSkin(чужой): id=%d team=%d ssk=%d -> слот %d", id, team_from_id(id), v, slot);
        return;
    }
    g_lastId = id; g_lastLocal = 1;
    g_realTeam = team_from_id(id);
    int lv = -1; if (!prop_int(pl, "testerlvl", &lv)) lv = local_tester_level(); g_testerLvl = lv;
    int slot = purchased_slot_now();
    if (slot < 0 && g_equipped >= 0 && (g_owned & (1 << g_equipped))) g_lastApplied = -2;   // скин не для этой команды
    if (slot < 0 && lv >= TESTER_MIN) { int tm = cur_team(); slot = tm == 1 ? TESTER_SLOT_RED : tm == 2 ? TESTER_SLOT_BLUE : -1; }
    if (slot >= 0) { set_skin_slot(self, slot); g_lastApplied = slot; }
    if (logs++ < 40) blog("SetSkin(я): id=%d team=%d tester=%d -> слот %d", id, cur_team(), lv, slot);
    if (purchased_slot_now() != g_sentVal || now_s() - g_lastSend > 60) { if (!sync_send()) g_syncDirty = true; }
}

// ---- главное меню ----
static void* type_obj(void* klass) { return il2cpp_type_get_object(il2cpp_class_get_type(klass)); }
// уровень тестера текущего игрока из LocalTesterService (работает и в меню, и в матче)
static int local_tester_level() {
    if (!kLTS || !mi_FindObj || !mi_LTSget) return -1;
    double t = now_s();
    if (g_ltsLvl >= 0 && t - g_ltsT < 5.0) return g_ltsLvl;
    void* inst = ((fn_findobj)fptr(mi_FindObj))(type_obj(kLTS), mi_FindObj);
    if (!inst) return g_ltsLvl;
    g_ltsLvl = ((fn_geti)fptr(mi_LTSget))(inst, mi_LTSget);
    g_ltsT = t;
    return g_ltsLvl;
}
// индекс скина первого SkinOffsetHandler у контроллера (для моделей без CommandSkinSetter)
static int handler_skin(void* ctrl) {
    if (!ctrl || off_Handlers < 0 || off_HSkin < 0) return -1;
    void* arr = *(void**)((char*)ctrl + off_Handlers);
    if (!arr || *(int*)((char*)arr + 0xC) <= 0) return -1;      // Il2CppArray: max_length +0xC, данные +0x10
    void* h = *(void**)((char*)arr + 0x10);
    return h ? *(uint8_t*)((char*)h + off_HSkin) : -1;
}
// Вызывается каждый кадр из любого "вечного" Update (чат/друзья); раз в 1.5 с в меню находит модели и ставит скин
static void menu_pump() {
    if (!mi_FindObjs || !mi_GetCIP || !kCtrlCls || !kSetterCls || !mi_SetAll) return;
    double t = now_s();
    if (t - g_pumpT < 1.5) return;
    g_pumpT = t;
    if (g_lastHudT > 0 && t - g_lastHudT < 3.0) { g_appN = 0; return; }   // идёт матч
    void* arr = ((fn_findobj)fptr(mi_FindObjs))(type_obj(kCtrlCls), mi_FindObjs);
    if (!arr) return;
    int n = *(int*)((char*)arr + 0xC);
    static int logs = 0;
    for (int i = 0; i < n && i < 64; i++) {
        void* ctrl = *(void**)((char*)arr + 0x10 + 4 * i);
        if (!ctrl) continue;
        bool done = false;
        for (int k = 0; k < g_appN; k++) if (g_app[k].p == ctrl) { done = true; break; }
        if (done) continue;
        void* setter = ((fn_getcip)fptr(mi_GetCIP))(ctrl, type_obj(kSetterCls), mi_GetCIP);
        int id;
        if (setter) {
            void* av = off_Awatar >= 0 ? *(void**)((char*)setter + off_Awatar) : nullptr;
            if (av) continue;                                          // аватар матча
            id = mi_getTechId ? ((fn_getTech)fptr(mi_getTechId))(setter, mi_getTechId) : -1;
        } else id = handler_skin(ctrl);
        int team = team_from_id(id), lv = -1;
        int slot = menu_slot_for_team(team, &lv);
        if (slot >= 0) ((fn_SetAll)fptr(mi_SetAll))(ctrl, (uint8_t)slot, mi_SetAll);
        if (logs++ < 40) blog("меню-скан: ctrl=%p setter=%p id=%d team=%d tester=%d -> слот %d", ctrl, setter, id, team, lv, slot);
        if ((slot >= 0 || lv >= 0) && g_appN < 32) { g_app[g_appN].p = ctrl; g_app[g_appN].slot = slot; g_appN++; }
    }
}
static void (*orig_ChatUpd)(void*, void*);
static void hook_ChatUpd(void* self, void* mi) { orig_ChatUpd(self, mi); menu_pump(); }
static void (*orig_FriendsUpd)(void*, void*);
static void hook_FriendsUpd(void* self, void* mi) { orig_FriendsUpd(self, mi); menu_pump(); }

static void (*orig_CtrlStart)(void*, void*);
static void hook_CtrlStart(void* self, void* mi) {
    orig_CtrlStart(self, mi);
    if (!self) return;
    double age = g_lastHudT > 0 ? now_s() - g_lastHudT : 1e9;      // сколько секунд нет HUD матча
    void* setter = mi_GetCIP ? ((fn_getcip)fptr(mi_GetCIP))(self, type_obj(kSetterCls), mi_GetCIP) : (void*)1;
    int apply = -1;
    if (!setter && age > 3.0 && mi_SetAll) {                        // не аватар матча => модель в меню
        apply = menu_slot_for_team(team_from_id(handler_skin(self)), nullptr);
        if (apply >= 0) ((fn_SetAll)fptr(mi_SetAll))(self, (uint8_t)apply, mi_SetAll);
    }
    static int logged = 0;
    if (logged++ < 25) blog("контроллер скинов Start: setter=%p hud_age=%.1f -> слот %d", setter, age > 1e8 ? -1.0 : age, apply);
}

// ---- IMGUI-обёртки ----
static void* S(const char* s) { return il2cpp_string_new(s); }
static bool Btn(Rect r, const std::string& s) { return ((fn_Button)fptr(mi_Button))(r, S(s.c_str()), mi_Button); }
static void Lbl(Rect r, const std::string& s) { ((fn_Label)fptr(mi_Label))(r, S(s.c_str()), mi_Label); }
static void Box(Rect r, const std::string& s) { ((fn_Label)fptr(mi_Box))(r, S(s.c_str()), mi_Box); }

static void tick_coins() {
    double t = now_s();
    if (g_lastT > 0) { double d = t - g_lastT; if (d > 0 && d < 1.0) g_acc += d; }
    g_lastT = t;
    while (g_acc >= SECONDS_PER_COIN) { g_acc -= SECONDS_PER_COIN; g_coins++; }
    if (t - g_lastSave > 30) { g_lastSave = t; save_state(); }
}

static void draw_ui() {
    float sw = (float)((fn_Scr)fptr(mi_scrW))(mi_scrW);
    float sh = (float)((fn_Scr)fptr(mi_scrH))(mi_scrH);
    float sc = sh / 720.f; if (sc < 1.f) sc = 1.f;
    float vw = sw / sc;
    Mat old = ((fn_getMat)fptr(mi_getMat))(mi_getMat);
    Mat m = {}; m.m[0] = sc; m.m[5] = sc; m.m[10] = 1; m.m[15] = 1;
    ((fn_setMat)fptr(mi_setMat))(m, mi_setMat);

    // кнопка справа от кнопки чата (слева сверху)
    if (Btn({300, 8, 170, 44}, "Магазин: " + std::to_string(g_coins))) g_open = !g_open;

    if (g_open) {
        float W = 560, H = 80 + 4 * 62 + 110, X = vw / 2 - W / 2, Y = 8;
        Box({X, Y, W, H}, "Магазин скинов  (цена " + std::to_string(PRICE) + ", 1 монета за " + std::to_string((int)SECONDS_PER_COIN) + " сек)");
        if (Btn({X + W - 54, Y + 4, 48, 30}, "X")) g_open = false;   // закрыть
        for (int i = 0; i < 4; i++) {
            float y = Y + 34 + i * 62;
            bool owned = g_owned & (1 << i);
            Lbl({X + 12, y + 8, 330, 44}, std::string(ITEMS[i].name) + (g_equipped == i ? "  [надет]" : owned ? "  [куплен]" : ""));
            std::string label = !owned ? "Купить " + std::to_string(PRICE) : (g_equipped == i ? "Снять" : "Надеть");
            if (Btn({X + W - 170, y + 6, 158, 46}, label)) {
                if (!owned) {
                    if (g_coins >= PRICE) { g_coins -= PRICE; g_owned |= (1 << i); g_msg = "Куплено!"; save_state(); }
                    else g_msg = "Не хватает монет (нужно " + std::to_string(PRICE) + ")";
                } else if (g_equipped == i) { g_equipped = -1; g_msg = "Скин снят. Применится после респауна."; save_state(); g_syncDirty = true; }
                else { g_equipped = i; g_msg = "Надето. Применится после респауна."; save_state(); g_syncDirty = true; }
            }
        }
        float y = Y + 34 + 4 * 62;
        const char* tn[] = {"авто", "красная", "синяя"};
        if (Btn({X + 12, y + 4, 250, 40}, std::string("Команда: ") + tn[g_teamMode] + " (нажми)")) g_teamMode = (g_teamMode + 1) % 3;
        Lbl({X + 280, y + 4, 270, 40}, "Определена: " + std::string(tn[g_realTeam]));
        Lbl({X + 12, y + 46, W - 24, 24}, g_msg);
        Lbl({X + 12, y + 70, W - 24, 24}, "диагностика: id=" + std::to_string(g_lastId) + " local=" + std::to_string(g_lastLocal) + " применён=" + std::to_string(g_lastApplied) + " sync=" + std::to_string(g_sentVal) + " чужой=" + std::to_string(g_remoteSeen) + " tester=" + std::to_string(g_testerLvl));
    }
    ((fn_setMat)fptr(mi_setMat))(old, mi_setMat);
}

// ---- хук: HitCross.OnGUI (рисуем поверх HUD) ----
static void (*orig_OnGUI)(void*, void*);
static void hook_OnGUI(void* self, void* mi) {
    orig_OnGUI(self, mi);
    g_lastHudT = now_s();
    tick_coins();
    sync_tick();
    draw_ui();
}

// ---- движок хуков: если в процессе есть libTGdearstudioV2, ставим хуки ЕГО MSHookFunction ----
// (один движок на процесс, без конфликта двух разных патчеров кода)
typedef void (*fn_MSHook)(void*, void*, void**);
static fn_MSHook g_msHook = nullptr;
static bool g_tgPresent = false;
static void detect_engine() {
    void* h = dlopen("libTGdearstudioV2.so", RTLD_NOW | RTLD_NOLOAD);
    g_tgPresent = h != nullptr;
    if (h) *(void**)&g_msHook = dlsym(h, "MSHookFunction");
    blog("libTGdearstudioV2: %s; движок хуков: %s", h ? "найдена" : "нет", g_msHook ? "её MSHookFunction (Substrate)" : "Dobby");
}
static bool install_hook(void* target, void* repl, void** orig) {
    if (!target) return false;
    if (g_msHook) g_msHook(target, repl, orig); else DobbyHook(target, repl, orig);
    return true;
}

static bool resolve_all() {
    static int lastStage = -1;
    auto stage = [&](int n, const char* m) { if (n != lastStage) { lastStage = n; blog("этап %d: %s", n, m); } };
    stage(0, "ждём libil2cpp.so");
    void* h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    stage(1, "libil2cpp.so найдена");
#define R(n) *(void**)&n = dlsym(h, #n); if (!n) { LOG("нет " #n); return false; }
    R(il2cpp_domain_get) R(il2cpp_domain_assembly_open) R(il2cpp_assembly_get_image)
    R(il2cpp_class_from_name) R(il2cpp_class_get_methods) R(il2cpp_class_get_field_from_name)
    R(il2cpp_field_get_offset) R(il2cpp_method_get_name) R(il2cpp_method_get_param_count)
    R(il2cpp_method_get_param) R(il2cpp_type_get_name) R(il2cpp_string_new) R(il2cpp_thread_attach)
    R(il2cpp_object_new) R(il2cpp_value_box) R(il2cpp_object_unbox) R(il2cpp_object_get_class) R(il2cpp_class_get_name) R(il2cpp_class_get_type) R(il2cpp_type_get_object)
#undef R
    stage(2, "il2cpp API найден, ждём домен");
    void* dom = il2cpp_domain_get();
    if (!dom) return false;
    stage(3, "домен есть, attach потока");
    il2cpp_thread_attach(dom);
    stage(4, "ищем классы");

    void* kSetter = find_class("Assembly-CSharp", "", "CommandSkinSetter");
    void* kCtrl   = find_class("Assembly-CSharp", "", "SkinOffsetHandlersController");
    void* kSpawn  = find_class("Assembly-CSharp", "", "SpawnObjectHandler");
    void* kHit    = find_class("Assembly-CSharp", "", "HitCross");
    void* kPlayer = find_class("PhotonRealtime", "Photon.Realtime", "Player");
    void* kGUI    = find_class("UnityEngine.IMGUIModule", "UnityEngine", "GUI");
    void* kScr    = find_class("UnityEngine.CoreModule", "UnityEngine", "Screen");
    if (!kSetter || !kCtrl || !kSpawn || !kHit || !kPlayer || !kGUI || !kScr) return false;
    stage(5, "классы найдены, ищем методы");

    mi_SetSkin   = find_method(kSetter, "SetSkin", 0);
    mi_getTechId = find_method(kSetter, "get_TechnicalSkinId", 0);
    mi_SetAll    = find_method(kCtrl, "SetAllSkinIndexes", 1);
    mi_getPhoton = find_method(kSpawn, "get_PhotonPlayer", 0);
    mi_Button = find_method(kGUI, "Button", 2, 1, "System.String");
    mi_Label  = find_method(kGUI, "Label", 2, 1, "System.String");
    mi_Box    = find_method(kGUI, "Box", 2, 1, "System.String");
    mi_getMat = find_method(kGUI, "get_matrix", 0);
    mi_setMat = find_method(kGUI, "set_matrix", 1);
    mi_scrW = find_method(kScr, "get_width", 0);
    mi_scrH = find_method(kScr, "get_height", 0);
    void* mi_OnGUI = find_method(kHit, "OnGUI", 0);

    kHashtable = find_class("Photon3Unity3D", "ExitGames.Client.Photon", "Hashtable");
    kInt32 = find_class("mscorlib", "System", "Int32");
    void* kPN = find_class("PhotonUnityNetworking", "Photon.Pun", "PhotonNetwork");
    mi_HtCtor = find_method(kHashtable, ".ctor", 0);
    mi_HtSet  = find_method(kHashtable, "set_Item", 2);
    mi_HtGet  = find_method(kHashtable, "get_Item", 1);
    mi_PlSet  = find_method(kPlayer, "SetCustomProperties", 3);
    mi_PlGet  = find_method(kPlayer, "get_CustomProperties", 0);
    mi_PNLocal = find_method(kPN, "get_LocalPlayer", 0);
    g_syncOk = kHashtable && kInt32 && kPN && mi_HtCtor && mi_HtSet && mi_HtGet && mi_PlSet && mi_PlGet && mi_PNLocal;
    blog("синхронизация скинов: %s", g_syncOk ? "включена" : "недоступна");

    kSetterCls = kSetter;
    kLTS = find_class("Assembly-CSharp", "Scripts.Backend.Tester", "LocalTesterService");
    void* kObj = find_class("UnityEngine.CoreModule", "UnityEngine", "Object");
    void* kComp = find_class("UnityEngine.CoreModule", "UnityEngine", "Component");
    mi_LTSget   = find_method(kLTS, "get_TesterLvl", 0);
    mi_FindObj  = find_method(kObj, "FindObjectOfType", 1, 0, "System.Type");
    mi_GetCIP   = find_method(kComp, "GetComponentInParent", 1, 0, "System.Type");
    mi_CtrlStart = find_method(kCtrl, "Start", 0);
    kCtrlCls = kCtrl;
    kHandlerCls = find_class("Assembly-CSharp", "", "SkinOffsetHandler");
    mi_FindObjs = find_method(kObj, "FindObjectsOfType", 1, 0, "System.Type");
    void* kChat = find_class("Assembly-CSharp", "Scripts.Chat", "ChatService");
    void* kFriends = find_class("Assembly-CSharp", "Scripts.Friends.UI", "FriendsListAdapter");
    mi_ChatUpd = find_method(kChat, "Update", 0);
    mi_FriendsUpd = find_method(kFriends, "Update", 0);
    off_Handlers = field_off(kCtrl, "AllHandlers");
    off_HSkin = field_off(kHandlerCls, "m_SkinIndex");
    blog("насос меню: findObjs=%p chatUpd=%p friendsUpd=%p handlers=%d hskin=%d", mi_FindObjs, mi_ChatUpd, mi_FriendsUpd, off_Handlers, off_HSkin);
    blog("меню: lts=%p find=%p cip=%p start=%p", mi_LTSget, mi_FindObj, mi_GetCIP, mi_CtrlStart);

    off_Awatar = field_off(kSetter, "Awatar");
    off_Ctrl   = field_off(kSetter, "m_SkinOffsetHandler");
    off_IsLocal = field_off(kPlayer, "IsLocal");

    blog("методы: SetSkin=%p tech=%p setAll=%p photon=%p btn=%p lbl=%p box=%p getM=%p setM=%p w=%p h=%p ongui=%p",
        mi_SetSkin, mi_getTechId, mi_SetAll, mi_getPhoton, mi_Button, mi_Label, mi_Box, mi_getMat, mi_setMat, mi_scrW, mi_scrH, mi_OnGUI);
    blog("поля: Awatar=%d ctrl=%d IsLocal=%d", off_Awatar, off_Ctrl, off_IsLocal);
    if (!mi_SetSkin || !mi_SetAll || !mi_Button || !mi_Label || !mi_Box || !mi_getMat || !mi_setMat
        || !mi_scrW || !mi_scrH || !mi_OnGUI || off_Awatar < 0 || off_Ctrl < 0 || off_IsLocal < 0) return false;

    stage(6, "ставим хуки");
    install_hook(fptr(mi_SetSkin), (void*)hook_SetSkin, (void**)&orig_SetSkin);
    install_hook(fptr(mi_OnGUI), (void*)hook_OnGUI, (void**)&orig_OnGUI);
    if (mi_CtrlStart && mi_GetCIP && mi_FindObj && mi_LTSget && kLTS) {
        install_hook(fptr(mi_CtrlStart), (void*)hook_CtrlStart, (void**)&orig_CtrlStart);
        blog("хук главного меню установлен");
    } else blog("главное меню: нужные методы не найдены, функция отключена");
    if (mi_FindObjs && off_Handlers >= 0 && off_HSkin >= 0) {
        if (mi_ChatUpd) install_hook(fptr(mi_ChatUpd), (void*)hook_ChatUpd, (void**)&orig_ChatUpd);
        if (mi_FriendsUpd) install_hook(fptr(mi_FriendsUpd), (void*)hook_FriendsUpd, (void**)&orig_FriendsUpd);
        blog("насос меню установлен (чат=%d друзья=%d)", mi_ChatUpd ? 1 : 0, mi_FriendsUpd ? 1 : 0);
    } else blog("насос меню не установлен");
    blog("хуки установлены");
    return true;
}

static void* worker(void*) {
    init_path();
    blog("мод загружен (pid %d)", (int)getpid());
    if (access(g_off.c_str(), F_OK) == 0) { blog("выключено: найден файл skinshop_off"); return nullptr; }
    detect_engine();
    load_state();
    if (access(g_guard.c_str(), F_OK) == 0) {
        blog("прошлый запуск завершился аварийно: мод пропущен на этот раз");
        unlink(g_guard.c_str());
        return nullptr;
    }
    sleep(g_tgPresent ? 35 : 20);         // даём игре (и libTGdearstudioV2) полностью стартовать
    FILE* gf = fopen(g_guard.c_str(), "w"); if (gf) { fputs("1", gf); fclose(gf); }
    for (int i = 0; i < 240; i++) {
        sleep(1);
        if (resolve_all()) {
            sleep(60);
            unlink(g_guard.c_str());      // 60 секунд без вылета: защита снята
            blog("работает стабильно");
            return nullptr;
        }
    }
    unlink(g_guard.c_str());
    blog("не удалось инициализироваться (последний этап выше)");
    return nullptr;
}

__attribute__((constructor)) static void on_load() {
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
}
