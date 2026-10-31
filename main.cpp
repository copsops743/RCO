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
#include <vector>
#include <math.h>
#include "dobby.h"

#define TAG "SkinShop"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ====================== НАСТРОЙКИ (можно менять) ======================
static const int   PRICE            = 100;   // цена каждого скина
static const float SECONDS_PER_COIN = 10.f;  // 1 монета за 10 секунд в матче

struct Item { const char* name; int slot; int team; }; // team: 0 любая, 1 красная, 2 синяя
static const int NITEMS = 6;
static Item ITEMS[NITEMS] = {
    {"Skeleton Skin (Any Team)", 8, 0},
    {"Zombie Skin (Any Team)", 13, 0},
    {"Military Skin (Red Team)", 2, 1},
    {"Military Skin (Blue Team)", 3, 2},
    {"Gangster Skin (Red Team)", 21, 1},
    {"Gangster Skin (Blue Team)", 22, 2},
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
static uint32_t (*il2cpp_gchandle_new)(void*, int);
static void (*il2cpp_gchandle_free)(uint32_t);
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
static void vis_scan();
static void* kVisCls = nullptr;
static void* kWCICls = nullptr;
static int off_WCIWeapon = -1;
static std::string g_visDbg = "оружие: ожидание";
static int g_visCalls = 0;
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
// ===== раскраски оружия: слот k = сдвиг текстуры на k*64 px по x (то есть k/32 по UV) =====
struct WSkin { int id; const char* name; int n; unsigned char ks[16]; };
static const int WSKIN_PRICE = 100;
static const WSkin WSKINS[] = {
    {19, "Karambit",      6,  {1, 2, 3, 4, 5, 6}},
    {12, "M9 Bayonet",    6,  {1, 2, 3, 4, 5, 6}},
    {20, "Kitchen Knife", 3,  {1, 2, 3}},
    {2,  "AK47",          11, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    {9,  "MP5",           10, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
    {14, "M4A1",          11, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20}},
    {30, "Mossberg",      11, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 19}},
    {13, "M24",           10, {2, 4, 6, 8, 10, 12, 14, 16, 18, 20}},
    {18, "AWP",           15, {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30}},
    {17, "Tec-9",         10, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
    {16, "Mac-10",        10, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
    {36, "Mac-11",        10, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
    {22, "SCAR",          3,  {1, 2, 3}},
    {3,  "Desert Eagle",  11, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 21}},
    {1,  "Beretta",       11, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};
static const int NW = (int)(sizeof(WSKINS) / sizeof(WSKINS[0]));
static unsigned g_wOwned[32];          // маска купленных раскрасок (бит 0 = оригинал, всегда)
static int g_wSel[32];                 // выбранная раскраска (0 = оригинал)
static int g_wPrev = 0, g_visW = -1;   // раскраска в предпросмотре / индекс оружия на открытой странице
static std::string g_karMsg;
static std::string g_sentWsk;
static int widx_of_id(int id) { for (int i = 0; i < NW; i++) if (WSKINS[i].id == id) return i; return -1; }
static int wslot(int wi, int p) { return p <= 0 ? 0 : WSKINS[wi].ks[p - 1]; }
// строка "id=раскраска;id=раскраска" для отправки другим игрокам (только выбранные)
static std::string build_wsk() {
    std::string r;
    for (int i = 0; i < NW; i++) if (g_wSel[i] > 0) { if (!r.empty()) r += ";"; r += std::to_string(WSKINS[i].id) + "=" + std::to_string(g_wSel[i]); }
    return r;
}
static int wsk_get(const std::string& s, int id) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find(';', pos); if (e == std::string::npos) e = s.size();
        std::string t = s.substr(pos, e - pos); size_t q = t.find('=');
        if (q != std::string::npos && atoi(t.substr(0, q).c_str()) == id) return atoi(t.substr(q + 1).c_str());
        pos = e + 1;
    }
    return 0;
}
static double g_resetT = 0;
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
    fprintf(f, "%d %d %d", g_coins, g_owned, g_equipped);
    for (int i = 0; i < NW; i++) fprintf(f, " %d %u %d", WSKINS[i].id, g_wOwned[i] | 1u, g_wSel[i]);
    fprintf(f, "\n");
    fclose(f);
}
static void load_state() {
    FILE* f = fopen(g_path.c_str(), "r");
    if (!f) return;
    std::vector<long> v; long x;
    while (fscanf(f, "%ld", &x) == 1) v.push_back(x);
    fclose(f);
    if (v.size() >= 3) { g_coins = (int)v[0]; g_owned = (int)v[1]; g_equipped = (int)v[2]; }
    if (v.size() == 5) {                                           // старый формат: только Karambit
        int wi = widx_of_id(19);
        if (wi >= 0) { g_wOwned[wi] = (unsigned)v[3]; g_wSel[wi] = (int)v[4]; }
    } else {
        for (size_t i = 3; i + 2 < v.size(); i += 3) {
            int wi = widx_of_id((int)v[i]);
            if (wi >= 0) { g_wOwned[wi] = (unsigned)v[i + 1]; g_wSel[wi] = (int)v[i + 2]; }
        }
    }
    for (int i = 0; i < NW; i++) {
        unsigned mask = (WSKINS[i].n + 1 >= 32) ? 0xffffffffu : ((1u << (WSKINS[i].n + 1)) - 1u);
        g_wOwned[i] = (g_wOwned[i] | 1u) & mask;
        if (g_wSel[i] < 0 || g_wSel[i] > WSKINS[i].n || !(g_wOwned[i] & (1u << g_wSel[i]))) g_wSel[i] = 0;
    }
    g_owned &= (1 << NITEMS) - 1;                                    // лишние биты отбрасываем
    if (g_equipped < 0 || g_equipped >= NITEMS || !(g_owned & (1 << g_equipped))) g_equipped = -1;
    blog("сохранение: монет=%d куплено(маска)=0x%x надет=%d, оружий с раскрасками: %d", g_coins, g_owned, g_equipped, NW);
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
    for (int i = 0; i < NITEMS; i++) if (ITEMS[i].slot == sl) return true;
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
// читает строковое свойство игрока Photon
static bool prop_str(void* photonPlayer, const char* key, std::string* out) {
    if (!g_syncOk || !photonPlayer) return false;
    void* props = ((fn_getPhoton)fptr(mi_PlGet))(photonPlayer, mi_PlGet);
    if (!props) return false;
    void* val = ((fn_htget)fptr(mi_HtGet))(props, il2cpp_string_new(key), mi_HtGet);
    if (!val) return false;
    const char* n = il2cpp_class_get_name(il2cpp_object_get_class(val));
    if (!n || strcmp(n, "String")) return false;
    int len = *(int*)((char*)val + 8);                              // Il2CppString: длина +8, символы UTF-16 с +12
    if (len < 0 || len > 400) return false;
    const uint16_t* c = (const uint16_t*)((char*)val + 12);
    out->clear();
    for (int i = 0; i < len; i++) out->push_back(c[i] < 128 ? (char)c[i] : '?');
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
    std::string wsk = build_wsk();                                  // выбранные раскраски оружия
    ((fn_htset)fptr(mi_HtSet))(ht, il2cpp_string_new("wsk"), il2cpp_string_new(wsk.c_str()), mi_HtSet);
    g_sentWsk = wsk;
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
    if (purchased_slot_now() != g_sentVal || build_wsk() != g_sentWsk || now_s() - g_lastSend > 60) { if (!sync_send()) g_syncDirty = true; }
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
    vis_scan();                                                           // какой предмет сейчас в магазине оружия
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

// ================= Karambit: раскраски ножа =================
struct V2 { float x, y; };
typedef void* (*fn_gcic)(void*, void*, bool, void*);
typedef bool (*fn_geb)(void*, void*);
typedef void (*fn_setV2)(void*, V2, void*);
typedef V2 (*fn_getV2)(void*, void*);
static void *kRendererCls, *mi_GOcic, *mi_GOactive, *mi_CompGO, *mi_RGetMat, *mi_MSetOff, *mi_MGetOff, *mi_OpImplicit, *mi_GetName;
static void *mi_VisItemSettings, *mi_WName, *mi_Visualize, *mi_FPWAct, *mi_FPWModel, *mi_VerGUI;
// запасные методы на случай, если основные вырезаны из игры
static void *mi_CompCIC, *mi_GOcic1, *mi_GOtransform, *mi_MSetTexOff, *mi_MSetVec, *mi_OpEq;
struct V4 { float x, y, z, w; };
typedef void* (*fn_gcic1)(void*, void*, void*);
typedef void (*fn_setTexOff)(void*, void*, V2, void*);
typedef void (*fn_setVec)(void*, void*, V4, void*);
typedef bool (*fn_opeq)(void*, void*, void*);
static std::string g_karDbg = "кар: нет данных";
static int off_VisCur = -1;
static bool g_karOk = false, g_verHooked = false;
static void* g_visItem = nullptr;
static uint32_t g_visHandle = 0;

static std::string str_ascii(void* s) {       // Il2CppString (32-бит): длина +8, символы UTF-16 с +12
    if (!s) return "";
    int n = *(int*)((char*)s + 8);
    if (n < 0 || n > 200) return "";
    const uint16_t* c = (const uint16_t*)((char*)s + 12);
    std::string r;
    for (int i = 0; i < n; i++) r.push_back(c[i] < 128 ? (char)c[i] : '?');
    return r;
}
static bool unity_alive(void* o) {
    if (!o) return false;
    if (mi_OpImplicit) return ((fn_geb)fptr(mi_OpImplicit))(o, mi_OpImplicit);
    if (mi_OpEq) return !((fn_opeq)fptr(mi_OpEq))(o, nullptr, mi_OpEq);
    return true;
}

// часть тела персонажа (руки, ноги, голова): модель или объект называется L_*/R_* (L_UpperArm, R_Hand...), Head, Spine...
// такие рендеры не трогаем. Раньше фильтровал по наличию SkinOffsetHandler, но он бывает и на деталях оружия.
static void* mi_GetCompT = nullptr;                         // Component.GetComponent(Type)
static void *kMFCls = nullptr, *mi_MFMesh = nullptr;        // MeshFilter.get_sharedMesh
static bool is_body_name(const std::string& nm) {
    if (nm.size() >= 2 && (nm[0] == 'L' || nm[0] == 'R') && nm[1] == '_') return true;
    static const char* body[] = {"Head", "Spine", "Spine1", "Spine2", "Pelvis", "Neck", "Hips"};
    for (const char* b : body) if (nm == b) return true;
    return false;
}
static bool is_body_renderer(void* r) {
    std::string gon, mesh;
    void* go = mi_CompGO ? ((fn_getPhoton)fptr(mi_CompGO))(r, mi_CompGO) : nullptr;
    if (go && mi_GetName) gon = str_ascii(((fn_getPhoton)fptr(mi_GetName))(go, mi_GetName));
    if (mi_GetCompT && kMFCls && mi_MFMesh && mi_GetName) {
        void* mf = ((fn_getcip)fptr(mi_GetCompT))(r, type_obj(kMFCls), mi_GetCompT);
        void* m = mf ? ((fn_getPhoton)fptr(mi_MFMesh))(mf, mi_MFMesh) : nullptr;
        if (m) mesh = str_ascii(((fn_getPhoton)fptr(mi_GetName))(m, mi_GetName));
    }
    bool body = is_body_name(mesh) || is_body_name(gon);
    static int logs = 0;
    if (logs++ < 120) blog("  рендер: объект='%s' модель='%s' тело=%d", gon.c_str(), mesh.c_str(), body ? 1 : 0);
    return body;
}
// сдвигает текстуру всех Renderer у объекта (GameObject или Component) на раскраску k; возвращает число затронутых
static int apply_variant(void* goOrComp, bool isGO, int k) {
    if (!g_karOk || !goOrComp) return -1;
    void* go = isGO ? goOrComp : (mi_CompGO ? ((fn_getPhoton)fptr(mi_CompGO))(goOrComp, mi_CompGO) : nullptr);
    if (!go) return -1;
    void* arr = nullptr;
    if (mi_GOcic) arr = ((fn_gcic)fptr(mi_GOcic))(go, type_obj(kRendererCls), true, mi_GOcic);
    else if (mi_GOcic1) arr = ((fn_gcic1)fptr(mi_GOcic1))(go, type_obj(kRendererCls), mi_GOcic1);
    else if (mi_CompCIC) {
        void* comp = isGO ? (mi_GOtransform ? ((fn_getPhoton)fptr(mi_GOtransform))(go, mi_GOtransform) : nullptr) : goOrComp;
        if (comp) arr = ((fn_gcic)fptr(mi_CompCIC))(comp, type_obj(kRendererCls), true, mi_CompCIC);
    }
    if (!arr) return 0;
    int n = *(int*)((char*)arr + 0xC), done = 0, skipped = 0;
    static int logs = 0;
    for (int i = 0; i < n && i < 32; i++) {
        void* r = *(void**)((char*)arr + 0x10 + 4 * i);
        if (!r) continue;
        if (is_body_renderer(r)) { skipped++; continue; }
        void* m = ((fn_getPhoton)fptr(mi_RGetMat))(r, mi_RGetMat);
        if (!m) continue;
        if (mi_MGetOff && logs < 6) { V2 b = ((fn_getV2)fptr(mi_MGetOff))(m, mi_MGetOff); blog("раскраска: исходное смещение текстуры (%.4f, %.4f)", b.x, b.y); logs++; }
        float ox = k * 64.0f / 2048.0f;
        if (mi_MSetOff) ((fn_setV2)fptr(mi_MSetOff))(m, V2{ox, 0.0f}, mi_MSetOff);
        else if (mi_MSetTexOff) ((fn_setTexOff)fptr(mi_MSetTexOff))(m, il2cpp_string_new("_MainTex"), V2{ox, 0.0f}, mi_MSetTexOff);
        else if (mi_MSetVec) ((fn_setVec)fptr(mi_MSetVec))(m, il2cpp_string_new("_MainTex_ST"), V4{1.0f, 1.0f, ox, 0.0f}, mi_MSetVec);
        else break;
        done++;
    }
    static int slogs = 0;
    if (slogs++ < 80) blog("раскраска k=%d: найдено рендеров=%d, пропущено частей тела=%d, покрашено=%d", k, n, skipped, done);
    return done;
}
static void set_vis_item(void* item) {
    if (g_visHandle) { il2cpp_gchandle_free(g_visHandle); g_visHandle = 0; }
    g_visItem = item;
    if (item) g_visHandle = il2cpp_gchandle_new(item, 0);   // удерживаем объект от сборки мусора
}
static bool vis_alive() {
    if (!g_visItem || !unity_alive(g_visItem)) return false;
    if (!mi_CompGO || !mi_GOactive) return true;
    void* go = ((fn_getPhoton)fptr(mi_CompGO))(g_visItem, mi_CompGO);
    return go && ((fn_geb)fptr(mi_GOactive))(go, mi_GOactive);
}

// ---- какое оружие описывает объект настроек (WeaponSettings) ----
static void* mi_WID = nullptr;
static int widx_from_settings(void* st) {                  // индекс в таблице раскрасок или -1
    if (!st || !mi_WID) return -1;
    return widx_of_id(((fn_geti)fptr(mi_WID))(st, mi_WID));
}
static std::string settings_name(void* st) { return (st && mi_WName) ? str_ascii(((fn_getPhoton)fptr(mi_WName))(st, mi_WName)) : std::string(); }

// ---- хук: WeaponsVisualizer.Visualize (страница оружия в магазине меню) ----
static bool (*orig_Visualize)(void*, int, void*);
static bool hook_Visualize(void* self, int id, void* mi) {
    bool r = orig_Visualize(self, id, mi);
    void* item = (self && off_VisCur >= 0) ? *(void**)((char*)self + off_VisCur) : nullptr;
    void* st = (item && mi_VisItemSettings) ? ((fn_getPhoton)fptr(mi_VisItemSettings))(item, mi_VisItemSettings) : nullptr;
    int wi = g_karOk ? widx_from_settings(st) : -1;
    std::string name = settings_name(st);
    static int logs = 0;
    g_visCalls++;
    g_visDbg = "оружие: '" + name + "' id=" + std::to_string(id) + " раскраски=" + (wi >= 0 ? "да" : "нет") + " (хук #" + std::to_string(g_visCalls) + ")";
    if (logs++ < 60) blog("магазин: Visualize(%d) -> '%s' item=%p раскраски=%d", id, name.c_str(), item, wi >= 0 ? 1 : 0);
    set_vis_item(wi >= 0 ? item : nullptr);
    g_visW = wi;
    if (wi >= 0) {
        g_wPrev = g_wSel[wi]; g_karMsg.clear();
        int n = apply_variant(item, false, wslot(wi, g_wPrev));
        blog("%s в магазине: раскраска %d применена к %d рендерам", WSKINS[wi].name, g_wPrev, n);
    }
    return r;
}

// индекс оружия с раскрасками на открытой (активной) странице каталога: -1 нет, -2 нет данных
static int active_page_weapon() {
    if (!mi_FindObjs || !kWCICls || off_WCIWeapon < 0 || !mi_WID) return -2;
    void* arr = ((fn_findobj)fptr(mi_FindObjs))(type_obj(kWCICls), mi_FindObjs);
    if (!arr) return -1;
    int n = *(int*)((char*)arr + 0xC);
    for (int i = 0; i < n && i < 64; i++) {
        void* ad = *(void**)((char*)arr + 0x10 + 4 * i);
        if (!ad) continue;
        int wi = widx_from_settings(*(void**)((char*)ad + off_WCIWeapon));
        if (wi >= 0) return wi;
    }
    return -1;
}
// находит открытый сейчас предмет магазина оружия, даже если страницу открыли до установки хуков
static void vis_scan() {
    if (!g_karOk || !mi_FindObj || !kVisCls || off_VisCur < 0 || !mi_VisItemSettings || !mi_WID) return;
    if (active_page_weapon() == -1) {                                       // магазин закрыт или открыта страница без раскрасок
        if (g_visItem) { set_vis_item(nullptr); g_visW = -1; g_visDbg = "оружие: страница закрыта"; }
        return;
    }
    void* vis = ((fn_findobj)fptr(mi_FindObj))(type_obj(kVisCls), mi_FindObj);
    if (!vis) { if (g_visItem) { set_vis_item(nullptr); g_visW = -1; } return; }
    void* item = *(void**)((char*)vis + off_VisCur);
    if (!item || !unity_alive(item)) return;
    if (item == g_visItem) return;
    void* st = ((fn_getPhoton)fptr(mi_VisItemSettings))(item, mi_VisItemSettings);
    int wi = widx_from_settings(st);
    std::string name = settings_name(st);
    g_visDbg = "оружие: '" + name + "' раскраски=" + (wi >= 0 ? "да" : "нет") + " (скан)";
    static int logs = 0;
    if (logs++ < 30) blog("скан магазина: '%s' item=%p раскраски=%d", name.c_str(), item, wi >= 0 ? 1 : 0);
    set_vis_item(wi >= 0 ? item : nullptr);
    g_visW = wi;
    if (wi >= 0) { g_wPrev = g_wSel[wi]; g_karMsg.clear(); int n = apply_variant(item, false, wslot(wi, g_wPrev)); blog("%s (скан): раскраска %d применена к %d рендерам", WSKINS[wi].name, g_wPrev, n); }
}

// ---- страница каталога закрывается: сразу убираем панель ----
static void *mi_WCIDisable;
static void (*orig_WCIDisable)(void*, void*);
static void hook_WCIDisable(void* self, void* mi) {
    orig_WCIDisable(self, mi);
    if (!self || !g_karOk || off_WCIWeapon < 0 || !mi_WID) return;
    int wi = widx_from_settings(*(void**)((char*)self + off_WCIWeapon));
    if (wi >= 0 && wi == g_visW) { set_vis_item(nullptr); g_visW = -1; g_visDbg = "оружие: страница закрыта"; }
}

// ---- оружие в руках: FPWeaponAdapter (там и настройки оружия, и модель) ----
static void *mi_FPAEnable;
static int off_FPASettings = -1, off_FPAWeapon = -1;
static void* g_fpa = nullptr;
static int g_fpaW = -1;
static uint32_t g_fpaHandle = 0;
static bool g_fpaDone = false;
static double g_fpaT = 0;
static int g_fpaCalls = 0;
static std::string g_handDbg = "рука: нет данных";
static void set_fpa(void* a, int wi) {
    if (g_fpaHandle) { il2cpp_gchandle_free(g_fpaHandle); g_fpaHandle = 0; }
    g_fpa = a; g_fpaW = wi; g_fpaDone = false;
    if (a) g_fpaHandle = il2cpp_gchandle_new(a, 0);
}
static int apply_fpa() {                    // >0 число рендеров, -1 объект исчез, -2 модели ещё нет
    if (!g_fpa || g_fpaW < 0 || !unity_alive(g_fpa)) return -1;
    void* w = off_FPAWeapon >= 0 ? *(void**)((char*)g_fpa + off_FPAWeapon) : nullptr;
    if (!w || !mi_FPWModel) return -2;
    void* go = ((fn_getPhoton)fptr(mi_FPWModel))(w, mi_FPWModel);
    if (!go) return -2;
    return apply_variant(go, true, wslot(g_fpaW, g_wSel[g_fpaW]));
}
static void (*orig_FPAEnable)(void*, void*);
static void hook_FPAEnable(void* self, void* mi) {
    orig_FPAEnable(self, mi);
    if (!self || !g_karOk) return;
    void* st = off_FPASettings >= 0 ? *(void**)((char*)self + off_FPASettings) : nullptr;
    int wi = widx_from_settings(st);
    g_fpaCalls++;
    int n = -9;
    if (wi >= 0) { set_fpa(self, wi); if (g_wSel[wi] > 0) { n = apply_fpa(); g_fpaDone = n > 0; } else g_fpaDone = true; }
    g_handDbg = "рука: '" + settings_name(st) + "' раскраски=" + (wi >= 0 ? "да" : "нет") + " выбрана=" + std::to_string(wi >= 0 ? g_wSel[wi] : -1) + " рендеров=" + std::to_string(n) + " (#" + std::to_string(g_fpaCalls) + ")";
    static int logs = 0;
    if (logs++ < 60) blog("FPWeaponAdapter.OnEnable: '%s' раскраски=%d выбрана=%d рендеров=%d", settings_name(st).c_str(), wi >= 0 ? 1 : 0, wi >= 0 ? g_wSel[wi] : -1, n);
}
// повторные попытки (модель могла появиться позже, чем сработал OnEnable); вызывается каждый кадр из HUD-хука
static void fpa_retry() {
    if (!g_fpa || g_fpaDone || g_fpaW < 0 || g_wSel[g_fpaW] == 0) return;
    double t = now_s();
    if (t - g_fpaT < 0.5) return;
    g_fpaT = t;
    int n = apply_fpa();
    if (n > 0) { g_fpaDone = true; g_handDbg += " -> применено " + std::to_string(n); blog("%s в руке: раскраска %d применена к %d рендерам (повтор)", WSKINS[g_fpaW].name, g_wSel[g_fpaW], n); }
    else if (n == -1) set_fpa(nullptr, -1);
}

// ---- оружие у аватаров (свой и чужие, вид от третьего лица) ----
static void *mi_AWHEnable, *kSpawnCls;
static int off_AWHSettings = -1;
struct TrackedKnife { void* h; uint32_t gc; int applied; int wi; };
static TrackedKnife g_tk[24];
static int g_tkN = 0;
static double g_tkT = 0;
static std::string g_remDbg = "чужое оружие: нет данных";
// какая раскраска оружия wi должна быть у аватара, которому принадлежит компонент h (-1 если владелец неизвестен)
static int owner_variant(void* h, int wi, int* isLocalOut) {
    if (isLocalOut) *isLocalOut = -1;
    if (!h || wi < 0 || !mi_GetCIP || !kSpawnCls || !mi_getPhoton) return -1;
    void* sp = ((fn_getcip)fptr(mi_GetCIP))(h, type_obj(kSpawnCls), mi_GetCIP);
    if (!sp) return -1;
    void* pl = ((fn_getPhoton)fptr(mi_getPhoton))(sp, mi_getPhoton);
    if (!pl) return -1;
    bool local = off_IsLocal >= 0 && *((uint8_t*)pl + off_IsLocal);
    if (isLocalOut) *isLocalOut = local ? 1 : 0;
    if (local) return g_wSel[wi];
    std::string ws;
    if (!prop_str(pl, "wsk", &ws)) return 0;
    int v = wsk_get(ws, WSKINS[wi].id);
    return (v >= 0 && v <= WSKINS[wi].n) ? v : 0;
}
static void track_weapon(void* h, int applied, int wi) {
    for (int i = 0; i < g_tkN; i++) if (g_tk[i].h == h) { g_tk[i].applied = applied; g_tk[i].wi = wi; return; }
    if (g_tkN >= 24) { il2cpp_gchandle_free(g_tk[0].gc); for (int i = 1; i < g_tkN; i++) g_tk[i - 1] = g_tk[i]; g_tkN--; }
    g_tk[g_tkN].h = h; g_tk[g_tkN].gc = il2cpp_gchandle_new(h, 0); g_tk[g_tkN].applied = applied; g_tk[g_tkN].wi = wi; g_tkN++;
}
static void (*orig_AWHEnable)(void*, void*);
static void hook_AWHEnable(void* self, void* mi) {
    orig_AWHEnable(self, mi);
    if (!self || !g_karOk) return;
    void* st = off_AWHSettings >= 0 ? *(void**)((char*)self + off_AWHSettings) : nullptr;
    int wi = widx_from_settings(st);
    if (wi < 0) return;
    int loc = -1;
    int v = owner_variant(self, wi, &loc);
    int n = v > 0 ? apply_variant(self, false, wslot(wi, v)) : -9;
    track_weapon(self, v > 0 ? v : 0, wi);
    g_remDbg = std::string("оружие аватара ") + WSKINS[wi].name + ": владелец " + (loc == 1 ? "я" : loc == 0 ? "чужой" : "?") + " раскраска=" + std::to_string(v) + " рендеров=" + std::to_string(n);
    static int logs = 0;
    if (logs++ < 60) blog("AvatarWeaponHandler.OnEnable: %s, владелец=%d раскраска=%d рендеров=%d", WSKINS[wi].name, loc, v, n);
}
// раз в секунду перепроверяет известные виды оружия: свойство игрока могло прийти позже
static void knives_refresh() {
    if (g_tkN == 0) return;
    double t = now_s();
    if (t - g_tkT < 1.0) return;
    g_tkT = t;
    for (int i = 0; i < g_tkN;) {
        void* h = g_tk[i].h;
        if (!unity_alive(h)) { il2cpp_gchandle_free(g_tk[i].gc); g_tk[i] = g_tk[--g_tkN]; continue; }
        int v = owner_variant(h, g_tk[i].wi, nullptr);
        if (v >= 0 && v != g_tk[i].applied) {
            int n = apply_variant(h, false, wslot(g_tk[i].wi, v));
            g_tk[i].applied = v;
            g_remDbg = std::string("оружие аватара ") + WSKINS[g_tk[i].wi].name + ": обновлено, раскраска=" + std::to_string(v) + " рендеров=" + std::to_string(n);
            blog("оружие аватара обновлено: %s раскраска=%d рендеров=%d", WSKINS[g_tk[i].wi].name, v, n);
        }
        i++;
    }
}

// ================= масштаб интерфейса: как у самой игры (CanvasScaler) =================
// Раскладка проектировалась на телефоне 2400x1080. Здесь считаем, во сколько раз интерфейс игры на ЭТОМ экране
// отличается от телефона, и масштабируем свои элементы так же. Положение привязываем к краям как у игры.
static void* kCSCls = nullptr;
static int off_CSMode = -1, off_CSRef = -1, off_CSMatch = -1, off_CSMatchMode = -1, off_CSScale = -1;
static double g_uiT = 0;
static float g_uiR = 1.f;
static int g_uiLastW = 0, g_uiLastH = 0;
static std::string g_uiDbg = "ui: нет данных";
static float scaler_scale(int mode, int matchMode, float refW, float refH, float match, float constScale, float w, float h) {
    if (mode == 1) {                                                  // Scale With Screen Size
        if (refW <= 0.f || refH <= 0.f) return 1.f;
        if (matchMode == 1) return fminf(w / refW, h / refH);         // Expand
        if (matchMode == 2) return fmaxf(w / refW, h / refH);         // Shrink
        float lw = log2f(w / refW), lh = log2f(h / refH);
        return exp2f(lw + (lh - lw) * match);                         // Match Width Or Height
    }
    return constScale > 0.f ? constScale : 1.f;                       // Constant Pixel Size: от экрана не зависит
}
static float ui_ratio(float sw, float sh) {
    double t = now_s();
    if (t - g_uiT < 2.0 && (int)sw == g_uiLastW && (int)sh == g_uiLastH) return g_uiR;
    g_uiT = t; g_uiLastW = (int)sw; g_uiLastH = (int)sh;
    float r = sh / 1080.f;                                            // запасной вариант: по высоте экрана
    int n = -1, mode = -1;
    if (kCSCls && mi_FindObjs && off_CSMode >= 0 && off_CSRef >= 0 && off_CSMatch >= 0 && off_CSMatchMode >= 0 && off_CSScale >= 0) {
        void* arr = ((fn_findobj)fptr(mi_FindObjs))(type_obj(kCSCls), mi_FindObjs);
        if (arr) {
            n = *(int*)((char*)arr + 0xC);
            void* cs = nullptr;
            for (int i = 0; i < n && i < 32; i++) {
                void* c = *(void**)((char*)arr + 0x10 + 4 * i);
                if (!c) continue;
                if (!cs) cs = c;
                if (*(int*)((char*)c + off_CSMode) == 1) { cs = c; break; }   // предпочитаем Scale With Screen Size
            }
            if (cs) {
                mode = *(int*)((char*)cs + off_CSMode);
                float rx = *(float*)((char*)cs + off_CSRef), ry = *(float*)((char*)cs + off_CSRef + 4);
                float match = *(float*)((char*)cs + off_CSMatch), cscale = *(float*)((char*)cs + off_CSScale);
                int mm = *(int*)((char*)cs + off_CSMatchMode);
                float sd = scaler_scale(mode, mm, rx, ry, match, cscale, sw, sh);
                float sp = scaler_scale(mode, mm, rx, ry, match, cscale, 2400.f, 1080.f);
                if (sd > 0.f && sp > 0.f && sd < 50.f && sp < 50.f) r = sd / sp;
                char b[260]; snprintf(b, sizeof b, "ui: экран %dx%d scaler(n=%d режим=%d ref=%.0fx%.0f match=%.2f/%d) масштаб игры=%.3f телефон=%.3f r=%.3f",
                                      (int)sw, (int)sh, n, mode, rx, ry, match, mm, sd, sp, r);
                g_uiDbg = b;
                static int logs = 0; if (logs++ < 6) blog("%s", b);
            }
        }
    }
    if (n < 0 || mode < 0) {
        char b[160]; snprintf(b, sizeof b, "ui: экран %dx%d CanvasScaler не найден, r=%.3f (по высоте)", (int)sw, (int)sh, r);
        g_uiDbg = b;
    }
    if (r < 0.3f) r = 0.3f;
    if (r > 4.f) r = 4.f;
    g_uiR = r;
    return r;
}

static void draw_ui() {
    float sw = (float)((fn_Scr)fptr(mi_scrW))(mi_scrW);
    float sh = (float)((fn_Scr)fptr(mi_scrH))(mi_scrH);
    float sc = 1.5f * ui_ratio(sw, sh);                    // пикселей на «условную единицу» (на телефоне 1.5)
    float vw = sw / sc, vh = sh / sc;                       // на телефоне 1600 x 720
    Mat old = ((fn_getMat)fptr(mi_getMat))(mi_getMat);
    Mat m = {}; m.m[0] = sc; m.m[5] = sc; m.m[10] = 1; m.m[15] = 1;
    ((fn_setMat)fptr(mi_setMat))(m, mi_setMat);

    // кнопка справа от кнопки чата (слева сверху)
    bool inMatch = g_lastHudT > 0 && now_s() - g_lastHudT < 3.0;
    if (!inMatch) g_open = false;
    float bx = 300.f; if (bx + 170.f > vw - 4.f) bx = vw - 174.f; if (bx < 4.f) bx = 4.f;      // справа от кнопки чата (слева сверху)
    if (inMatch && Btn({bx, 8, 170, 44}, "Магазин: " + std::to_string(g_coins))) g_open = !g_open;
    if (!inMatch && !g_karOk) { Lbl({300, 6, 900, 24}, g_visDbg); Lbl({300, 28, 1100, 24}, g_karDbg); }
    else if (inMatch) { Lbl({300, 56, 1000, 24}, g_handDbg); Lbl({300, 78, 1000, 24}, g_remDbg); }
    Lbl({300, 100, vw - 310, 24}, g_uiDbg);                                   // временная диагностика масштаба                       // временная диагностика в матче   // временная диагностика в меню

    if (g_open) {
        float W = 560, H = 80 + NITEMS * 62 + 154, X = vw / 2 - W / 2, Y = 8; if (X < 4.f) X = 4.f;
        Box({X, Y, W, H}, "Магазин скинов  (цена " + std::to_string(PRICE) + ", 1 монета за " + std::to_string((int)SECONDS_PER_COIN) + " сек)");
        if (Btn({X + W - 54, Y + 4, 48, 30}, "X")) g_open = false;   // закрыть
        for (int i = 0; i < NITEMS; i++) {
            float y = Y + 34 + i * 62;
            bool owned = g_owned & (1 << i);
            Lbl({X + 12, y + 8, 330, 44}, std::string(ITEMS[i].name) + (g_equipped == i ? "  [надет]" : owned ? "  [куплен]" : ""));
            std::string label = !owned ? "Купить " + std::to_string(PRICE) : (g_equipped == i ? "Снять" : "Надеть");
            if (Btn({X + W - 170, y + 6, 158, 46}, label)) {
                if (!owned) {
                    if (g_coins >= PRICE) { g_coins -= PRICE; g_owned |= (1 << i); g_msg = "Куплено!"; save_state(); blog("покупка: %s, осталось монет %d", ITEMS[i].name, g_coins); }
                    else g_msg = "Не хватает монет (нужно " + std::to_string(PRICE) + ")";
                } else if (g_equipped == i) { g_equipped = -1; g_msg = "Скин снят. Применится после респауна."; save_state(); g_syncDirty = true; }
                else { g_equipped = i; g_msg = "Надето. Применится после респауна."; save_state(); g_syncDirty = true; }
            }
        }
        float y = Y + 34 + NITEMS * 62;
        const char* tn[] = {"авто", "красная", "синяя"};
        if (Btn({X + 12, y + 4, 250, 40}, std::string("Команда: ") + tn[g_teamMode] + " (нажми)")) g_teamMode = (g_teamMode + 1) % 3;
        Lbl({X + 280, y + 4, 270, 40}, "Определена: " + std::string(tn[g_realTeam]));
        Lbl({X + 12, y + 46, W - 24, 24}, g_msg);
        if (Btn({X + 12, y + 98, 250, 36}, "Сбросить покупки")) {
            double t = now_s();
            if (t - g_resetT < 4.0) {
                int cnt = 0;
                for (int i = 0; i < NITEMS; i++) if (g_owned & (1 << i)) cnt++;
                g_coins += cnt * PRICE; g_owned = 0; g_equipped = -1; g_resetT = 0;
                g_msg = "Покупки сброшены, монеты возвращены: +" + std::to_string(cnt * PRICE);
                blog("сброс покупок: возвращено %d монет", cnt * PRICE);
                save_state(); g_syncDirty = true;
            } else { g_resetT = t; g_msg = "Нажмите ещё раз (4 сек), чтобы сбросить все покупки"; }
        }
        Lbl({X + 12, y + 70, W - 24, 24}, "диагностика: id=" + std::to_string(g_lastId) + " local=" + std::to_string(g_lastLocal) + " применён=" + std::to_string(g_lastApplied) + " sync=" + std::to_string(g_sentVal) + " чужой=" + std::to_string(g_remoteSeen) + " tester=" + std::to_string(g_testerLvl));
    }
    // панель раскрасок (когда в магазине открыта страница оружия с раскрасками)
    if (g_visItem && g_visW >= 0) {
        if (!vis_alive()) { set_vis_item(nullptr); g_visW = -1; }
        else {
            const WSkin& W = WSKINS[g_visW];
            const int N = W.n + 1;
            if (g_wPrev < 0 || g_wPrev >= N) g_wPrev = 0;
            float X = vw / 2.f + 64.f, Y = vh - 184.f;               // центр по горизонтали, низ по вертикали (как у страницы оружия)
            if (X + 520.f > vw - 4.f) X = vw - 524.f;
            if (Y < 4.f) Y = 4.f;
            bool owned = ((g_wOwned[g_visW] | 1u) & (1u << g_wPrev)) != 0;
            Box({X, Y, 520, 100}, "");
            std::string st = owned ? (g_wSel[g_visW] == g_wPrev ? "выбрано" : "куплено") : ("цена " + std::to_string(WSKIN_PRICE));
            std::string nm = std::string(W.name) + (g_wPrev == 0 ? " Original" : " Skin " + std::to_string(g_wPrev));
            Lbl({X + 10, Y + 4, 500, 28}, nm + "  (" + std::to_string(g_wPrev + 1) + "/" + std::to_string(N) + ")  - " + st);
            bool chg = false;
            if (Btn({X + 10, Y + 34, 70, 56}, "<")) { g_wPrev = (g_wPrev + N - 1) % N; chg = true; }
            if (Btn({X + 90, Y + 34, 70, 56}, ">")) { g_wPrev = (g_wPrev + 1) % N; chg = true; }
            std::string lab = !owned ? "Купить " + std::to_string(WSKIN_PRICE) : (g_wSel[g_visW] == g_wPrev ? "Выбрано" : "Выбрать");
            if (Btn({X + 170, Y + 34, 340, 56}, lab)) {
                if (!owned) {
                    if (g_coins >= WSKIN_PRICE) { g_coins -= WSKIN_PRICE; g_wOwned[g_visW] |= (1u << g_wPrev); g_karMsg = "Куплено!"; save_state();
                        blog("покупка: %s раскраска %d, осталось монет %d", W.name, g_wPrev, g_coins); }
                    else g_karMsg = "Не хватает монет";
                } else if (g_wSel[g_visW] != g_wPrev) { g_wSel[g_visW] = g_wPrev; g_karMsg = "Выбрано"; save_state(); g_syncDirty = true; blog("%s: выбрана раскраска %d", W.name, g_wPrev); }
            }
            if (!g_karMsg.empty()) Lbl({X + 10, Y + 100, 500, 24}, g_karMsg);
            if (chg) { g_karMsg.clear(); apply_variant(g_visItem, false, wslot(g_visW, g_wPrev)); }
        }
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
    fpa_retry();
    knives_refresh();
    if (!g_verHooked) draw_ui();       // если нет VersionOuter, рисуем здесь (только в матче)
}
// VersionOuter ("v1.347" в углу) существует и в меню, и в матче: через него рисуем везде
static void (*orig_VerGUI)(void*, void*);
static void hook_VerGUI(void* self, void* mi) {
    orig_VerGUI(self, mi);
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
    R(il2cpp_object_new) R(il2cpp_value_box) R(il2cpp_object_unbox) R(il2cpp_object_get_class) R(il2cpp_class_get_name) R(il2cpp_class_get_type) R(il2cpp_type_get_object) R(il2cpp_gchandle_new) R(il2cpp_gchandle_free)
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

    {   // --- Karambit / GUI в меню ---
        void* kGO = find_class("UnityEngine.CoreModule", "UnityEngine", "GameObject");
        kRendererCls = find_class("UnityEngine.CoreModule", "UnityEngine", "Renderer");
        void* kMat = find_class("UnityEngine.CoreModule", "UnityEngine", "Material");
        mi_GOcic = find_method(kGO, "GetComponentsInChildren", 2, 0, "System.Type");
        mi_GOactive = find_method(kGO, "get_activeInHierarchy", 0);
        mi_CompGO = find_method(kComp, "get_gameObject", 0);
        mi_RGetMat = find_method(kRendererCls, "get_material", 0);
        mi_MSetOff = find_method(kMat, "set_mainTextureOffset", 1);
        mi_MGetOff = find_method(kMat, "get_mainTextureOffset", 0);
        mi_OpImplicit = find_method(kObj, "op_Implicit", 1);
        mi_GetName = find_method(kObj, "get_name", 0);
        void* kVis = find_class("Assembly-CSharp", "Scripts.Weapons.Visualizer", "WeaponsVisualizer");
        void* kVisItem = find_class("Assembly-CSharp", "Scripts.Weapons.Visualizer", "WeaponsVisualizerItem");
        void* kWS = find_class("Assembly-CSharp", "", "WeaponSettings");
        void* kFPW = find_class("Assembly-CSharp", "", "vp_FPWeapon");
        void* kVer = find_class("Assembly-CSharp", "Scripts.GameVersion", "VersionOuter");
        kWCICls = find_class("Assembly-CSharp", "Scripts.Backend.Catalog.Adapters", "WeaponCatalogItemCustomDataAdapter");
        mi_WCIDisable = find_method(kWCICls, "OnDisable", 0);
        off_WCIWeapon = field_off(kWCICls, "m_Weapon");
        void* kAWH = find_class("Assembly-CSharp", "", "AvatarWeaponHandler");
        mi_AWHEnable = find_method(kAWH, "OnEnable", 0);
        off_AWHSettings = field_off(kAWH, "WeaponSettings");
        kSpawnCls = kSpawn;
        kCSCls = find_class("UnityEngine.UI", "UnityEngine.UI", "CanvasScaler");
        off_CSMode = field_off(kCSCls, "m_UiScaleMode");
        off_CSRef = field_off(kCSCls, "m_ReferenceResolution");
        off_CSMatch = field_off(kCSCls, "m_MatchWidthOrHeight");
        off_CSMatchMode = field_off(kCSCls, "m_ScreenMatchMode");
        off_CSScale = field_off(kCSCls, "m_ScaleFactor");
        blog("CanvasScaler: класс=%p поля mode=%d ref=%d match=%d matchMode=%d scale=%d", kCSCls, off_CSMode, off_CSRef, off_CSMatch, off_CSMatchMode, off_CSScale);
        void* kFPA = find_class("Assembly-CSharp", "", "FPWeaponAdapter");
        mi_FPAEnable = find_method(kFPA, "OnEnable", 0);
        off_FPASettings = field_off(kFPA, "Settings");
        off_FPAWeapon = field_off(kFPA, "Weapon");
        kVisCls = kVis;
        mi_Visualize = find_method(kVis, "Visualize", 1);
        off_VisCur = field_off(kVis, "m_CurrentItem");
        mi_VisItemSettings = find_method(kVisItem, "get_WeaponSettings", 0);
        mi_WName = find_method(kWS, "get_WeaponName", 0);
        mi_WID = find_method(kWS, "get_ID", 0);
        mi_FPWAct = find_method(kFPW, "Activate", 0);
        mi_FPWModel = find_method(kFPW, "get_WeaponModel", 0);
        mi_VerGUI = find_method(kVer, "OnGUI", 0);
        mi_GOcic1 = find_method(kGO, "GetComponentsInChildren", 1, 0, "System.Type");
        mi_GetCompT = find_method(kComp, "GetComponent", 1, 0, "System.Type");
        kMFCls = find_class("UnityEngine.CoreModule", "UnityEngine", "MeshFilter");
        mi_MFMesh = find_method(kMFCls, "get_sharedMesh", 0);
        blog("фильтр частей тела: GetComponent(Type)=%p MeshFilter=%p get_sharedMesh=%p", mi_GetCompT, kMFCls, mi_MFMesh);
        mi_CompCIC = find_method(kComp, "GetComponentsInChildren", 2, 0, "System.Type");
        mi_GOtransform = find_method(kGO, "get_transform", 0);
        mi_MSetTexOff = find_method(kMat, "SetTextureOffset", 2, 0, "System.String");
        mi_MSetVec = find_method(kMat, "SetVector", 2, 0, "System.String");
        mi_OpEq = find_method(kObj, "op_Equality", 2);
        bool hasArr = mi_GOcic || mi_GOcic1 || (mi_CompCIC && mi_GOtransform);
        bool hasOff = mi_MSetOff || mi_MSetTexOff || mi_MSetVec;
        bool hasAlive = mi_OpImplicit || mi_OpEq;
        g_karOk = kRendererCls && hasArr && mi_CompGO && mi_RGetMat && hasOff && hasAlive;
        { char b[300]; snprintf(b, sizeof b, "кар: ok=%d массив(go2/go1/comp)=%d/%d/%d материал=%d смещение(set/tex/vec)=%d/%d/%d жив(imp/eq)=%d/%d",
             g_karOk ? 1 : 0, mi_GOcic ? 1 : 0, mi_GOcic1 ? 1 : 0, mi_CompCIC ? 1 : 0, mi_RGetMat ? 1 : 0,
             mi_MSetOff ? 1 : 0, mi_MSetTexOff ? 1 : 0, mi_MSetVec ? 1 : 0, mi_OpImplicit ? 1 : 0, mi_OpEq ? 1 : 0);
          g_karDbg = b; blog("%s", b); }
        blog("Karambit: ok=%d visualize=%p visCur=%d itemSettings=%p wname=%p wid=%p fpwModel=%p verGUI=%p getName=%p",
             g_karOk ? 1 : 0, mi_Visualize, off_VisCur, mi_VisItemSettings, mi_WName, mi_WID, mi_FPWModel, mi_VerGUI, mi_GetName);
    }
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
    if (mi_VerGUI) {
        install_hook(fptr(mi_VerGUI), (void*)hook_VerGUI, (void**)&orig_VerGUI);
        g_verHooked = true;
        blog("GUI-хук VersionOuter установлен (панели работают и в меню)");
    } else blog("VersionOuter не найден: панели только в матче");
    if (g_karOk && mi_Visualize && off_VisCur >= 0 && mi_VisItemSettings && mi_WID) {
        install_hook(fptr(mi_Visualize), (void*)hook_Visualize, (void**)&orig_Visualize);
        blog("хук магазина оружия установлен");
    } else blog("хук магазина оружия НЕ установлен");
    if (g_karOk && mi_WCIDisable && off_WCIWeapon >= 0 && mi_WID) {
        install_hook(fptr(mi_WCIDisable), (void*)hook_WCIDisable, (void**)&orig_WCIDisable);
        blog("хук закрытия страницы каталога установлен");
    } else blog("хук закрытия страницы каталога НЕ установлен (disable=%p weapon=%d)", mi_WCIDisable, off_WCIWeapon);
    if (g_karOk && g_syncOk && mi_AWHEnable && off_AWHSettings >= 0 && mi_WID && mi_GetCIP && mi_getPhoton) {
        install_hook(fptr(mi_AWHEnable), (void*)hook_AWHEnable, (void**)&orig_AWHEnable);
        blog("хук AvatarWeaponHandler.OnEnable установлен (синхронизация ножа)");
    } else blog("хук AvatarWeaponHandler.OnEnable НЕ установлен (enable=%p settings=%d sync=%d)", mi_AWHEnable, off_AWHSettings, g_syncOk ? 1 : 0);
    if (g_karOk && mi_FPAEnable && mi_FPWModel && mi_WID && off_FPASettings >= 0 && off_FPAWeapon >= 0) {
        install_hook(fptr(mi_FPAEnable), (void*)hook_FPAEnable, (void**)&orig_FPAEnable);
        blog("хук FPWeaponAdapter.OnEnable установлен");
    } else blog("хук FPWeaponAdapter.OnEnable НЕ установлен (enable=%p model=%p wname=%p settings=%d weapon=%d)", mi_FPAEnable, mi_FPWModel, mi_WName, off_FPASettings, off_FPAWeapon);
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
