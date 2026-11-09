// SkinShop: локальный магазин скинов за монеты (1 монета за 10 сек в матче).
// Работает только на вашем устройстве. Сеть, PlayFab и пакеты игры не затрагиваются.
// Цель: Unity IL2CPP, armeabi-v7a. Всё ищется по именам через il2cpp API.
#include <android/log.h>
#include <jni.h>
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
#include <map>
#include <deque>
#include <math.h>
#include "dobby.h"
#include "icons.h"

#define TAG "SkinShop"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ====================== НАСТРОЙКИ (можно менять) ======================
static const int   PRICE            = 100;   // цена каждого скина
static const float SECONDS_PER_COIN = 10.f;  // 1 монета за 10 секунд в матче
static const float MUSIC_VOLUME = 0.12f;     // громкость музыки в главном меню (0..1): тихо
static const char* MUSIC_ASSET = "menu_music.m4a";   // файл внутри APK: assets/menu_music.m4a

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
static void* (*il2cpp_array_new)(void*, uintptr_t);
static void (*il2cpp_field_static_get_value)(void*, void*);
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
static double g_lastHudT = 0, g_ltsT = 0, g_lastGuiT = 0;
static int g_ltsLvl = -1;
static int local_tester_level();
static void vis_scan();
static void voice_pump();
static volatile int g_ptt = 0;                                  // 1 пока зажата кнопка микрофона
static char g_voiceDbgBuf[100] = "голос: нет данных", g_voiceRxBuf[100] = "";
static void voice_dbg(const char* fmt, ...) {
    char t[100]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    memcpy(g_voiceDbgBuf, t, sizeof t); g_voiceDbgBuf[99] = 0;
}
static void voice_rx(const char* fmt, ...) {
    char t[100]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    memcpy(g_voiceRxBuf, t, sizeof t); g_voiceRxBuf[99] = 0;
}
static std::string voice_text(const char* b) { return std::string(b, strnlen(b, 99)); }
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

static const bool DEBUG_OVERLAY = false;   // true = показывать на экране служебные надписи (диагностика). Для обычной игры - false
static const bool DAYNIGHT_ENABLED = true;   // смена времени суток (утро - день - вечер - ночь), у всех игроков с модом одинаково
static const float DAY_CYCLE_SECONDS = 360.f; // длительность полного цикла суток
static const bool WEATHER_ENABLED = true;  // снег и дождь (включает хозяин комнаты)
static const bool MINIMAP_ENABLED = true;  // мини-карта справа вверху
static const bool SMOKE_ENABLED = true;    // дым после взрывов
static const float SMOKE_TIME   = 2.5f;    // сколько секунд держится дым
static const bool LOG_TO_FILE   = false;   // true = писать лог в файлы skinshop.log (иначе только в logcat, ничего не копится в папках игры)
static void blog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    LOG("%s", buf);
    if (!LOG_TO_FILE) return;
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

// ================= голос: кнопка микрофона (касания) =================
struct V3 { float x, y, z; };
struct UTouch { int32_t fingerId; float px, py, rx, ry, dx, dy, timeDelta; int32_t tapCount, phase, type; float pressure, maxPressure, radius, radiusVar, altitude, azimuth; };
struct SendOpts { int32_t delivery; uint8_t encrypt, channel; uint16_t pad; };
typedef int (*fn_geti0)(void*);
typedef UTouch (*fn_getTouch)(int, void*);
typedef bool (*fn_getMouseBtn)(int, void*);
typedef V3 (*fn_getV3)(void*, void*);
typedef V3 (*fn_getV3s)(void*);
typedef bool (*fn_raise)(uint8_t, void*, void*, SendOpts, void*);
typedef V3 (*fn_w2s)(void*, V3, void*);                                  // Camera.WorldToScreenPoint(Vector3)
static void* mi_W2S = nullptr;
static const float VOICE_ICON_UP = 0.45f;                                // насколько выше позиции головы рисуем значок (в единицах мира)
static void *mi_TouchCount, *mi_GetTouch, *mi_MouseBtn, *mi_MousePos;
static void *mi_RaiseEvent, *g_reoField, *kByteCls, *mi_CamMain, *mi_TrPos, *mi_CompTransform;
static void *mi_EDData, *mi_EDSender, *mi_PunOnEvent, *kREOcls;
static void (*il2cpp_runtime_class_init)(void*);
static int off_EDCode = -1;
static bool g_voiceOk = false, g_voiceRecv = false, g_hookOn = false;
static int g_evAll = 0, g_evLast = -1;
static char g_recvInfo[100] = "";
static bool point_in(float x, float y, float rx, float ry, float rw, float rh) { return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh; }
// зажат ли палец (или мышь) внутри прямоугольника (пиксели экрана, начало в левом верхнем углу)
static bool mic_pressed(float sh, float rx, float ry, float rw, float rh) {
    if (mi_TouchCount && mi_GetTouch) {
        int n = ((fn_geti0)fptr(mi_TouchCount))(mi_TouchCount);
        for (int i = 0; i < n && i < 10; i++) {
            UTouch t = ((fn_getTouch)fptr(mi_GetTouch))(i, mi_GetTouch);
            if (t.phase == 3 || t.phase == 4) continue;                   // Ended / Canceled
            if (point_in(t.px, sh - t.py, rx, ry, rw, rh)) return true;
        }
        if (n > 0) return false;
    }
    if (mi_MouseBtn && mi_MousePos && ((fn_getMouseBtn)fptr(mi_MouseBtn))(0, mi_MouseBtn)) {
        V3 m = ((fn_getV3s)fptr(mi_MousePos))(mi_MousePos);
        return point_in(m.x, sh - m.y, rx, ry, rw, rh);
    }
    return false;
}

// ================= иконки: микрофон на кнопке и значок "говорит" =================
// Маски из icons.h превращаются в Texture2D (белый цвет + прозрачность) и рисуются через GUI.DrawTexture.
// Если нужные методы Unity не нашлись, интерфейс остаётся с надписями.
struct Col { float r, g, b, a; };
typedef void (*fn_ctor2)(void*, int, int, void*);
typedef void (*fn_setPix3)(void*, int, int, Col, void*);
typedef void (*fn_setPix4)(void*, int, int, Col, int, void*);
typedef void (*fn_v0)(void*, void*);
typedef void (*fn_apply2)(void*, bool, bool, void*);
typedef void (*fn_setHide)(void*, int, void*);
typedef void (*fn_drawTex)(Rect, void*, void*);
typedef void (*fn_drawTexTC)(Rect, void*, Rect, void*);
static void* mi_DrawTexTC = nullptr;
typedef void (*fn_beginGroup)(Rect, void*, void*, void*);
static void *mi_BeginGroup = nullptr, *mi_EndGroup = nullptr, *g_guiContent = nullptr, *g_guiStyle = nullptr;   // GUI.BeginGroup обрезает рисование прямоугольником
typedef void (*fn_setCol)(Col, void*);
typedef Col (*fn_getCol)(void*);
static void *mi_TexCtor, *mi_SetPix3, *mi_SetPix4, *mi_TexApply0, *mi_TexApply2, *mi_SetHide, *mi_DrawTex, *mi_GetGuiCol, *mi_SetGuiCol;
static void* g_icon[11];                                  // 0 микрофон, 1 корпус динамика, 2..4 волны, 5..6 дым, 7 точка, 8 снежинка, 9 капля дождя, 10 сплошной
static void* g_kTex = nullptr;
static bool g_iconTried = false, g_iconOk = false;
// мягкий клуб дыма 64x64: радиальный спад * шум (маска прозрачности)
static float hash2(int x, int y, int sd) { unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)sd * 2147483647u; h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16; return (h & 0xFFFF) / 65535.f; }
static float vnoise(float x, float y, int sd) {
    int ix = (int)floorf(x), iy = (int)floorf(y); float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.f - 2.f * fx); fy = fy * fy * (3.f - 2.f * fy);
    float a = hash2(ix, iy, sd), b = hash2(ix + 1, iy, sd), c = hash2(ix, iy + 1, sd), d = hash2(ix + 1, iy + 1, sd);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
static void gen_smoke(uint8_t* a, int sd) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = (x - 31.5f) / 32.f, dy = (y - 31.5f) / 32.f, r = sqrtf(dx * dx + dy * dy);
            float base = r >= 1.f ? 0.f : powf(1.f - r, 1.15f);
            float n = vnoise(x * 0.11f, y * 0.11f, sd) * 0.6f + vnoise(x * 0.27f, y * 0.27f, sd + 7) * 0.4f;
            float v = base * (0.45f + 0.9f * n); if (v > 1.f) v = 1.f;
            a[y * 64 + x] = (uint8_t)(v * 255.f);
        }
}
static void* make_icon(void* kTex, int w, int h, const uint8_t* a) {
    void* t = il2cpp_object_new(kTex);
    ((fn_ctor2)fptr(mi_TexCtor))(t, w, h, mi_TexCtor);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            Col c = {1.f, 1.f, 1.f, a[(h - 1 - y) * w + x] / 255.f};     // строка 0 маски - верх картинки, у Unity y=0 - низ
            if (mi_SetPix3) ((fn_setPix3)fptr(mi_SetPix3))(t, x, y, c, mi_SetPix3);
            else ((fn_setPix4)fptr(mi_SetPix4))(t, x, y, c, 0, mi_SetPix4);
        }
    if (mi_TexApply0) ((fn_v0)fptr(mi_TexApply0))(t, mi_TexApply0);
    else ((fn_apply2)fptr(mi_TexApply2))(t, true, false, mi_TexApply2);
    if (mi_SetHide) ((fn_setHide)fptr(mi_SetHide))(t, 61, mi_SetHide);   // HideAndDontSave: Unity не выгрузит текстуру при смене сцены
    il2cpp_gchandle_new(t, 1);                                           // и сборщик мусора IL2CPP её не съест
    return t;
}
static bool icons_ready() {
    if (g_iconOk) return true;
    if (g_iconTried) return false;
    g_iconTried = true;
    void* kTex = find_class("UnityEngine.CoreModule", "UnityEngine", "Texture2D");
    g_kTex = kTex;
    void* kObj = find_class("UnityEngine.CoreModule", "UnityEngine", "Object");
    void* kGUI = find_class("UnityEngine.IMGUIModule", "UnityEngine", "GUI");
    mi_TexCtor = find_method(kTex, ".ctor", 2, 0, "System.Int32");
    mi_SetPix3 = find_method(kTex, "SetPixel", 3);
    if (!mi_SetPix3) mi_SetPix4 = find_method(kTex, "SetPixel", 4);
    mi_TexApply0 = find_method(kTex, "Apply", 0);
    if (!mi_TexApply0) mi_TexApply2 = find_method(kTex, "Apply", 2);
    mi_SetHide = find_method(kObj, "set_hideFlags", 1);
    mi_DrawTex = find_method(kGUI, "DrawTexture", 2);
    mi_DrawTexTC = find_method(kGUI, "DrawTextureWithTexCoords", 3);               // в этой сборке Unity вырезан, оставлено на случай другой версии
    {   // обрезка прямоугольником: GUI.BeginGroup(Rect, GUIContent, GUIStyle) с пустыми объектами
        void* kGC = find_class("UnityEngine.IMGUIModule", "UnityEngine", "GUIContent");
        void* kGS = find_class("UnityEngine.IMGUIModule", "UnityEngine", "GUIStyle");
        mi_BeginGroup = find_method(kGUI, "BeginGroup", 3); mi_EndGroup = find_method(kGUI, "EndGroup", 0);
        void* cGC = find_method(kGC, ".ctor", 0); void* cGS = find_method(kGS, ".ctor", 0);
        if (mi_BeginGroup && mi_EndGroup && cGC && cGS) {
            g_guiContent = il2cpp_object_new(kGC); ((fn_v0)fptr(cGC))(g_guiContent, cGC); il2cpp_gchandle_new(g_guiContent, 0);
            g_guiStyle = il2cpp_object_new(kGS); ((fn_v0)fptr(cGS))(g_guiStyle, cGS); il2cpp_gchandle_new(g_guiStyle, 0);
        }
        blog("иконки: BeginGroup=%p EndGroup=%p content=%p style=%p", mi_BeginGroup, mi_EndGroup, g_guiContent, g_guiStyle);
    }
    mi_GetGuiCol = find_method(kGUI, "get_color", 0);
    mi_SetGuiCol = find_method(kGUI, "set_color", 1);
    blog("иконки: ctor=%p setPix3=%p setPix4=%p apply0=%p apply2=%p hide=%p draw=%p color=%p/%p", mi_TexCtor, mi_SetPix3, mi_SetPix4, mi_TexApply0, mi_TexApply2, mi_SetHide, mi_DrawTex, mi_GetGuiCol, mi_SetGuiCol);
    if (!kTex || !mi_TexCtor || !(mi_SetPix3 || mi_SetPix4) || !(mi_TexApply0 || mi_TexApply2) || !mi_DrawTex) return false;
    g_icon[0] = make_icon(kTex, ICON_MIC_W, ICON_MIC_H, ICON_MIC);
    for (int i = 0; i < 4; i++) g_icon[1 + i] = make_icon(kTex, ICON_SPK_W, ICON_SPK_H, ICON_SPK_LAYERS[i]);
    for (int i = 0; i < 2; i++) { static uint8_t sm[64 * 64]; gen_smoke(sm, 11 + i * 29); g_icon[5 + i] = make_icon(kTex, 64, 64, sm); }   // два вида клубов дыма
    { static uint8_t dot[32 * 32]; for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { float dx = (x - 15.5f) / 16.f, dy = (y - 15.5f) / 16.f, r = sqrtf(dx * dx + dy * dy); float v = (1.f - r) * 5.f; dot[y * 32 + x] = (uint8_t)(255.f * (v < 0.f ? 0.f : v > 1.f ? 1.f : v)); }
      g_icon[7] = make_icon(kTex, 32, 32, dot); }                                  // мягкий диск с чёткими краями
    { static uint8_t fl[32 * 32]; for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { float dx = (x - 15.5f) / 16.f, dy = (y - 15.5f) / 16.f, r = sqrtf(dx * dx + dy * dy); float v = 1.f - r; v = v < 0.f ? 0.f : v; fl[y * 32 + x] = (uint8_t)(255.f * v * v * (3.f - 2.f * v)); }
      g_icon[8] = make_icon(kTex, 32, 32, fl); }                                   // мягкая снежинка
    { static uint8_t st[8 * 64]; for (int y = 0; y < 64; y++) for (int x = 0; x < 8; x++) { float dx = (x - 3.5f) / 3.5f, w = expf(-dx * dx * 2.5f), f = sinf(3.14159265f * (y + 0.5f) / 64.f); st[y * 8 + x] = (uint8_t)(255.f * w * powf(f < 0.f ? 0.f : f, 0.6f)); }
      g_icon[9] = make_icon(kTex, 8, 64, st); }                                    // тонкая капля-штрих, прозрачная по краям
    { static uint8_t so[4 * 4]; for (int i = 0; i < 16; i++) so[i] = 255; g_icon[10] = make_icon(kTex, 4, 4, so); }   // сплошной (лёгкий оттенок на весь экран)
    g_iconOk = true;
    blog("иконки: текстуры созданы");
    return true;
}
static void draw_tex(void* tex, Rect r, Col tint) {
    Col old = {1.f, 1.f, 1.f, 1.f};
    bool tintable = mi_GetGuiCol && mi_SetGuiCol;
    if (tintable) { old = ((fn_getCol)fptr(mi_GetGuiCol))(mi_GetGuiCol); ((fn_setCol)fptr(mi_SetGuiCol))(tint, mi_SetGuiCol); }
    ((fn_drawTex)fptr(mi_DrawTex))(r, tex, mi_DrawTex);
    if (tintable) ((fn_setCol)fptr(mi_SetGuiCol))(old, mi_SetGuiCol);
}
static void icon_draw(int idx, Rect r, Col tint) { draw_tex(g_icon[idx], r, tint); }
// часть текстуры: uv в долях (0..1, начало снизу слева)
static void draw_tex_part(void* tex, Rect r, Rect uv, Col tint) {
    if (!mi_DrawTexTC) return;
    Col old = {1.f, 1.f, 1.f, 1.f};
    bool tintable = mi_GetGuiCol && mi_SetGuiCol;
    if (tintable) { old = ((fn_getCol)fptr(mi_GetGuiCol))(mi_GetGuiCol); ((fn_setCol)fptr(mi_SetGuiCol))(tint, mi_SetGuiCol); }
    ((fn_drawTexTC)fptr(mi_DrawTexTC))(r, tex, uv, mi_DrawTexTC);
    if (tintable) ((fn_setCol)fptr(mi_SetGuiCol))(old, mi_SetGuiCol);
}
// динамик с waves (1..3) волнами; w - ширина в условных единицах
static void speaker_icon(float x, float y, float w, int waves) {
    float h = w * ICON_SPK_H / ICON_SPK_W;
    for (int i = 0; i <= waves && i < 4; i++) icon_draw(1 + i, {x, y, w, h}, {1.f, 1.f, 1.f, 1.f});
}

// ================= дым после взрывов =================
// Хуки Exploder.MakeExplosion и vp_Explosion.DoExplode дают точку взрыва; дальше рисуется несколько мягких клубов,
// которые поднимаются, растут и тают за SMOKE_TIME секунд. Позиция переводится в экран через Camera.WorldToScreenPoint.
typedef float (*fn_getF)(void*, void*);
typedef bool (*fn_linecast)(V3, V3, void*);
static void *mi_Linecast = nullptr, *mi_FOV = nullptr, *mi_Smoke[2] = {nullptr, nullptr};
static int off_ExRadius = -1, off_VERadius = -1;
static void (*orig_Smoke[2])(void*, void*);

// ---- настоящий дым: квадраты (Quad) в мире с прозрачным материалом и проверкой глубины, поэтому блоки их закрывают ----
struct Quat { float x, y, z, w; };
typedef Quat (*fn_getQ)(void*, void*);
typedef void (*fn_setQ)(void*, Quat, void*);
typedef void (*fn_setV3)(void*, V3, void*);
typedef void* (*fn_createPrim)(int, void*);
typedef void* (*fn_getCompS)(void*, void*, void*);
typedef void (*fn_obj1)(void*, void*);
typedef void (*fn_setObj)(void*, void*, void*);
typedef void* (*fn_findShader)(void*, void*);
typedef void (*fn_setColN)(void*, void*, Col, void*);
typedef void (*fn_setI)(void*, int, void*);
static void *mi_CreatePrim, *mi_GetCompS, *mi_Destroy, *mi_MatCtor, *mi_FindShader, *mi_MatTex, *mi_MatSetColN, *mi_RendMat, *mi_RendShadow;
static void *mi_TrSetPos, *mi_TrSetRot, *mi_TrGetRot, *mi_TrSetScale;
static void* g_kMat = nullptr;
static void (*gc_free)(uint32_t) = nullptr;
static void* g_smokeShader = nullptr; static void* g_smokeProp = nullptr; static float g_smokeColMul = 1.f;
static int g_smoke3d = -1;                                                      // -1 ещё не проверяли, 0 нельзя (рисуем поверх экрана), 1 настоящий дым
struct Puff3D { void* go; void* tr; void* mat; uint32_t hgo, hmat; float off[3], vy, delay, amax, sm; int tex; };
struct SmokeCloud { float c[3]; float radius, puff; double t, lastT; float vis; unsigned seed; void* cam0; std::vector<Puff3D> pf; };
static std::vector<SmokeCloud> g_smoke;

static bool smoke3d_prepare() {
    if (g_smoke3d >= 0) return g_smoke3d == 1;
    g_smoke3d = 0;
    if (!g_kMat || !mi_CreatePrim || !mi_GetCompS || !mi_Destroy || !mi_MatCtor || !mi_FindShader || !mi_MatTex || !mi_MatSetColN || !mi_RendMat ||
        !mi_TrSetPos || !mi_TrSetRot || !mi_TrGetRot || !mi_TrSetScale || !mi_GOtransform || !mi_CompTransform || !mi_TrPos) { blog("дым: нет методов для настоящего дыма, рисуем поверх экрана"); return false; }
    static const struct { const char* name; const char* prop; float mul; } cand[] = {
        {"Sprites/Default", "_Color", 1.f}, {"Legacy Shaders/Particles/Alpha Blended", "_TintColor", 0.5f}, {"Mobile/Particles/Alpha Blended", "_TintColor", 0.5f}, {"UI/Default", "_Color", 1.f}};
    for (auto& cd : cand) {
        void* sh = ((fn_findShader)fptr(mi_FindShader))(S(cd.name), mi_FindShader);
        if (sh) { g_smokeShader = sh; g_smokeProp = S(cd.prop); il2cpp_gchandle_new(sh, 0); il2cpp_gchandle_new(g_smokeProp, 0); g_smokeColMul = cd.mul; blog("дым: шейдер %s", cd.name); g_smoke3d = 1; return true; }
    }
    blog("дым: подходящий шейдер не найден, рисуем поверх экрана");
    return false;
}
static void puff3d_destroy(Puff3D& p) {
    if (p.go) ((fn_obj1)fptr(mi_Destroy))(p.go, mi_Destroy);
    if (p.mat) ((fn_obj1)fptr(mi_Destroy))(p.mat, mi_Destroy);
    if (gc_free) { if (p.hgo) gc_free(p.hgo); if (p.hmat) gc_free(p.hmat); }
    p.go = p.mat = nullptr;
}
static bool puff3d_create(Puff3D& p) {
    p.go = p.tr = p.mat = nullptr; p.hgo = p.hmat = 0;
    void* go = ((fn_createPrim)fptr(mi_CreatePrim))(5, mi_CreatePrim);            // PrimitiveType.Quad
    if (!go) return false;
    p.go = go; p.hgo = il2cpp_gchandle_new(go, 0);
    p.tr = ((fn_getPhoton)fptr(mi_GOtransform))(go, mi_GOtransform);
    if (!p.tr) { puff3d_destroy(p); return false; }
    void* col = ((fn_getCompS)fptr(mi_GetCompS))(p.tr, S("MeshCollider"), mi_GetCompS);   // у примитива есть коллайдер - убираем, чтобы пули и игроки дым не цепляли
    if (col) ((fn_obj1)fptr(mi_Destroy))(col, mi_Destroy);
    void* rend = ((fn_getCompS)fptr(mi_GetCompS))(p.tr, S("MeshRenderer"), mi_GetCompS);
    if (!col || !rend) { puff3d_destroy(p); return false; }                       // не смогли убрать коллайдер или найти рендерер - безопаснее не использовать
    void* mat = il2cpp_object_new(g_kMat);
    ((fn_setObj)fptr(mi_MatCtor))(mat, g_smokeShader, mi_MatCtor);
    p.mat = mat; p.hmat = il2cpp_gchandle_new(mat, 0);
    ((fn_setObj)fptr(mi_MatTex))(mat, g_icon[5 + (p.tex & 1)], mi_MatTex);
    ((fn_setColN)fptr(mi_MatSetColN))(mat, g_smokeProp, Col{0.f, 0.f, 0.f, 0.f}, mi_MatSetColN);
    ((fn_setObj)fptr(mi_RendMat))(rend, mat, mi_RendMat);
    if (mi_RendShadow) ((fn_setI)fptr(mi_RendShadow))(rend, 0, mi_RendShadow);    // без теней
    ((fn_setV3)fptr(mi_TrSetScale))(p.tr, V3{0.f, 0.f, 0.f}, mi_TrSetScale);       // пока не нужен - невидим
    return true;
}
static void cloud_free(SmokeCloud& c, bool touchObjects) {
    if (touchObjects) for (auto& p : c.pf) puff3d_destroy(p);                      // если сцена сменилась, объекты уже уничтожены игрой - их не трогаем
    c.pf.clear();
}

static void smoke_spawn(void* self, int roff) {
    if (!SMOKE_ENABLED || !self || !mi_CompTransform || !mi_TrPos) return;
    void* tr = ((fn_getPhoton)fptr(mi_CompTransform))(self, mi_CompTransform);
    if (!tr) return;
    V3 p = ((fn_getV3)fptr(mi_TrPos))(tr, mi_TrPos);
    if (isnan(p.x) || isnan(p.y) || isnan(p.z)) return;
    float radius = 4.f;
    if (roff >= 0) {
        int raw; memcpy(&raw, (const char*)self + roff, 4); float fv; memcpy(&fv, &raw, 4);
        float v = (raw > 0 && raw < 1000) ? (float)raw : fv;                    // поле может быть int или float
        if (v > 0.5f && v < 100.f) radius = v;
    }
    if (radius < 2.f) radius = 2.f; if (radius > 12.f) radius = 12.f;
    double t = now_s();
    for (auto& c : g_smoke) {                                                    // два скрипта на одном взрыве - дым один
        float dx = c.c[0] - p.x, dy = c.c[1] - p.y, dz = c.c[2] - p.z;
        if (t - c.t < 0.5 && dx * dx + dy * dy + dz * dz < 4.f) return;
    }
    SmokeCloud c; c.c[0] = p.x; c.c[1] = p.y; c.c[2] = p.z; c.radius = radius;
    c.puff = radius * 0.9f; if (c.puff < 2.5f) c.puff = 2.5f; if (c.puff > 7.f) c.puff = 7.f;
    c.t = c.lastT = t; c.vis = 1.f; c.seed = (unsigned)(t * 1000.0) ^ (unsigned)(p.x * 977.f) ^ (unsigned)(p.z * 1543.f);
    c.cam0 = mi_CamMain ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    if (icons_ready() && smoke3d_prepare() && c.cam0) {                          // настоящие клубы: параметры считаются один раз при взрыве
        unsigned rs = c.seed;
        auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 16777216.f; };
        bool ok = true;
        for (int i = 0; i < 8 && ok; i++) {
            Puff3D q = {};
            q.off[0] = (rnd() - 0.5f) * radius * 0.9f; q.off[1] = rnd() * radius * 0.5f; q.off[2] = (rnd() - 0.5f) * radius * 0.9f;
            q.vy = 0.4f + rnd() * 0.7f; q.delay = rnd() * 0.25f; q.amax = 0.5f + rnd() * 0.35f; q.sm = 0.8f + rnd() * 0.5f; q.tex = rnd() < 0.5f ? 0 : 1;
            if (puff3d_create(q)) c.pf.push_back(q); else ok = false;
        }
        if (!ok) { cloud_free(c, true); blog("дым: не удалось создать объекты, отключаю настоящий дым"); g_smoke3d = 0; }
    }
    if (g_smoke.size() >= 6) { cloud_free(g_smoke[0], true); g_smoke.erase(g_smoke.begin()); }
    g_smoke.push_back(c);
}
static void hook_Smoke0(void* self, void* mi) { smoke_spawn(self, off_ExRadius); orig_Smoke[0](self, mi); }
static void hook_Smoke1(void* self, void* mi) { smoke_spawn(self, off_VERadius); orig_Smoke[1](self, mi); }

// настоящий дым: каждый кадр двигаем, увеличиваем и делаем прозрачнее квадраты, поворачивая их лицом к камере
static void smoke_update_3d(SmokeCloud& c, double t, V3 cp, Quat cr) {
    float age = (float)(t - c.t);
    for (auto& p : c.pf) {
        float la = age - p.delay;
        if (la < 0.f || la > SMOKE_TIME - p.delay) { ((fn_setV3)fptr(mi_TrSetScale))(p.tr, V3{0.f, 0.f, 0.f}, mi_TrSetScale); continue; }
        float k = la / (SMOKE_TIME - p.delay);
        float alpha = p.amax * fminf(1.f, la / 0.2f) * (k < 0.6f ? 1.f : (1.f - k) / 0.4f);   // быстро появляется, в конце тает
        float dia = c.puff * p.sm * (1.f + 0.6f * k);                            // растёт
        V3 w = {c.c[0] + p.off[0], c.c[1] + p.off[1] + p.vy * la, c.c[2] + p.off[2]};         // и поднимается
        float dx = w.x - cp.x, dy = w.y - cp.y, dz = w.z - cp.z, dist = sqrtf(dx * dx + dy * dy + dz * dz);
        alpha *= fminf(1.f, fmaxf(0.f, (dist - dia * 0.3f) / (dia * 0.8f)));      // камера внутри клуба - не заслоняем весь экран
        ((fn_setV3)fptr(mi_TrSetPos))(p.tr, w, mi_TrSetPos);
        ((fn_setQ)fptr(mi_TrSetRot))(p.tr, cr, mi_TrSetRot);
        ((fn_setV3)fptr(mi_TrSetScale))(p.tr, V3{dia, dia, 1.f}, mi_TrSetScale);
        ((fn_setColN)fptr(mi_MatSetColN))(p.mat, g_smokeProp, Col{0.56f * g_smokeColMul, 0.56f * g_smokeColMul, 0.58f * g_smokeColMul, alpha * g_smokeColMul}, mi_MatSetColN);
    }
}

// запасной вариант (если настоящий дым недоступен): мягкие клубы рисуются поверх экрана, стена между вами и взрывом прячет дым целиком
static void smoke_draw_overlay(SmokeCloud& c, double t, float sw, float sh, float sc, void* cam, V3 cp, float focal) {
    float age = (float)(t - c.t);
    float dxc = c.c[0] - cp.x, dyc = c.c[1] - cp.y, dzc = c.c[2] - cp.z;
    float dist = sqrtf(dxc * dxc + dyc * dyc + dzc * dzc);
    if (dist > 80.f) return;
    float target = 1.f;
    if (mi_Linecast && dist > 1.5f) {
        float inv = 1.f / dist;
        V3 to = {c.c[0] - dxc * inv, c.c[1] + 0.8f - dyc * inv, c.c[2] - dzc * inv};   // чуть ближе к камере и выше точки взрыва
        target = ((fn_linecast)fptr(mi_Linecast))(cp, to, mi_Linecast) ? 0.f : 1.f;
    }
    float dt = (float)(t - c.lastT); c.lastT = t; if (dt > 0.2f) dt = 0.2f;
    float step = 6.f * dt;
    if (target > c.vis) c.vis += fminf(step, target - c.vis); else c.vis -= fminf(step, c.vis - target);
    if (c.vis <= 0.01f) return;
    unsigned rs = c.seed;
    auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 16777216.f; };
    for (int i = 0; i < 8; i++) {
        float ox = (rnd() - 0.5f) * c.radius * 0.9f, oy = rnd() * c.radius * 0.5f, oz = (rnd() - 0.5f) * c.radius * 0.9f;
        float vy = 0.4f + rnd() * 0.7f, delay = rnd() * 0.25f, amax = 0.5f + rnd() * 0.35f, sm = 0.8f + rnd() * 0.5f;
        int tex = rnd() < 0.5f ? 5 : 6;
        float la = age - delay;
        if (la < 0.f) continue;
        float k = la / (SMOKE_TIME - delay);
        if (k > 1.f) continue;
        float alpha = amax * c.vis * fminf(1.f, la / 0.2f) * (k < 0.6f ? 1.f : (1.f - k) / 0.4f);
        float dia = c.puff * sm * (1.f + 0.6f * k);
        V3 w = {c.c[0] + ox, c.c[1] + oy + vy * la, c.c[2] + oz};
        V3 s = ((fn_w2s)fptr(mi_W2S))(cam, w, mi_W2S);
        if (s.z < 0.3f) continue;
        alpha *= fminf(1.f, fmaxf(0.f, (s.z - dia * 0.3f) / (dia * 0.8f)));
        float px = dia * focal / s.z; if (px > sh * 1.2f) px = sh * 1.2f;
        float cx = s.x, cy = sh - s.y;
        if (cx < -px || cx > sw + px || cy < -px || cy > sh + px) continue;
        icon_draw(tex, {(cx - px * 0.5f) / sc, (cy - px * 0.5f) / sc, px / sc, px / sc}, Col{0.56f, 0.56f, 0.58f, alpha});
    }
}
static void smoke_draw(float sw, float sh, float sc) {
    if (!SMOKE_ENABLED || g_smoke.empty()) return;
    double t = now_s();
    void* cam = mi_CamMain ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    for (size_t i = 0; i < g_smoke.size();) {                                    // закончился срок, или сцена сменилась (другая камера) - убираем
        SmokeCloud& c = g_smoke[i];
        bool sceneChanged = !c.pf.empty() && (!cam || cam != c.cam0);
        if (sceneChanged || t - c.t > SMOKE_TIME + 0.3) { cloud_free(c, !sceneChanged); g_smoke.erase(g_smoke.begin() + i); }
        else i++;
    }
    if (g_smoke.empty() || !cam || !mi_CompTransform || !mi_TrPos) return;
    void* ctr = ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform);
    if (!ctr) return;
    V3 cp = ((fn_getV3)fptr(mi_TrPos))(ctr, mi_TrPos);
    Quat cr = {0.f, 0.f, 0.f, 1.f};
    bool need3d = false, needOv = false;
    for (auto& c : g_smoke) { if (!c.pf.empty()) need3d = true; else needOv = true; }
    if (need3d) {
        cr = ((fn_getQ)fptr(mi_TrGetRot))(ctr, mi_TrGetRot);
        for (auto& c : g_smoke) if (!c.pf.empty()) smoke_update_3d(c, t, cp, cr);
    }
    if (needOv && icons_ready() && mi_W2S) {
        float fov = 60.f;
        if (mi_FOV) { fov = ((fn_getF)fptr(mi_FOV))(cam, mi_FOV); if (!(fov > 10.f && fov < 170.f)) fov = 60.f; }
        float focal = (sh * 0.5f) / tanf(fov * 0.5f * 3.14159265f / 180.f);
        for (auto& c : g_smoke) if (c.pf.empty()) smoke_draw_overlay(c, t, sw, sh, sc, cam, cp, focal);
    }
}

// ================= мини-карта: вид сверху, точки союзников =================
// Карта строится один раз за матч: игра сканируется лучами сверху вниз (Physics.Raycast), высоты красятся в цвета.
// Союзники: все SpawnObjectHandler (аватары) с тем же свойством Photon "team", что и у вас.
struct Bnds { V3 c, e; };
typedef Bnds (*fn_getBounds)(void*, void*);
typedef bool (*fn_getBool)(void*, void*);
typedef bool (*fn_ray6)(V3, V3, void*, float, int, int, void*);
static void *kColCls = nullptr, *g_kSpawnObj = nullptr, *mi_ColBounds = nullptr, *mi_ColTrigger = nullptr, *mi_Ray6 = nullptr, *mi_HandIsDead = nullptr;
static const int MM_N = 128;                                                      // клеток по стороне карты
static const float MM_PANEL = 0.235f;                                             // размер панели (доля высоты экрана)
static const float MM_VIEW  = 0.42f;                                              // какую долю карты видно в окне вокруг вас
struct MMDot { float x, z; };
static struct {
    int state;                                                                    // 0 ждём карту, 1 сканируем, 3 готова, 4 не удалось
    void* cam0; void* tex; double nextT;
    float minX, minZ, span, minY, maxY; int idx;
    float h[MM_N * MM_N];
    uint8_t rgb[MM_N * MM_N * 3], has[MM_N * MM_N];                              // настоящий цвет верхней грани блока в клетке (если удалось узнать)
} g_mm;
// ---- настоящие цвета блоков: луч -> коллайдер -> VoxelColliderHandler.m_Voxel -> Voxel.Textures[0] (верхняя грань) -> средний цвет текстуры ----
typedef Col (*fn_getPixel)(void*, int, int, void*);
static void *mi_RHcollider = nullptr, *mi_VoxTextures = nullptr, *mi_TexReadable = nullptr, *mi_TexGetPixel = nullptr, *mi_TexW = nullptr, *mi_TexH = nullptr, *g_vchStr = nullptr;
static int off_VCHVoxel = -1;
static std::map<void*, Col> g_colCollider, g_colVoxel;                              // кэш: коллайдер -> цвет, воксель -> цвет (a=0 - не удалось)
static int g_mmHit = 0, g_mmHandler = 0, g_mmVoxel = 0, g_mmColor = 0, g_mmUnread = 0;
static char g_mmTexInfo[80] = "";
static bool voxel_color(void* voxel, Col* out) {
    auto it = g_colVoxel.find(voxel);
    if (it != g_colVoxel.end()) { *out = it->second; return it->second.a > 0.5f; }
    Col res = {0.f, 0.f, 0.f, 0.f};
    void* arr = mi_VoxTextures ? ((fn_getPhoton)fptr(mi_VoxTextures))(voxel, mi_VoxTextures) : nullptr;
    if (arr) {
        const char* cn = il2cpp_class_get_name(il2cpp_object_get_class(arr));
        snprintf(g_mmTexInfo, sizeof g_mmTexInfo, "%s", cn ? cn : "?");
        int n = *(int*)((char*)arr + 0xC);
        if (cn && strstr(cn, "Texture2D") && n > 0 && mi_TexReadable && mi_TexGetPixel && mi_TexW && mi_TexH) {
            void* tex = *(void**)((char*)arr + 0x10);                              // индекс 0 - верхняя грань (Facing.Up)
            if (tex && ((fn_getBool)fptr(mi_TexReadable))(tex, mi_TexReadable)) {
                int w = ((fn_geti)fptr(mi_TexW))(tex, mi_TexW), h = ((fn_geti)fptr(mi_TexH))(tex, mi_TexH);
                if (w > 0 && h > 0) {
                    float sr = 0, sg = 0, sb = 0; int cnt = 0;
                    for (int yi = 0; yi < 4; yi++) for (int xi = 0; xi < 4; xi++) {
                        Col c = ((fn_getPixel)fptr(mi_TexGetPixel))(tex, (xi * 2 + 1) * w / 8, (yi * 2 + 1) * h / 8, mi_TexGetPixel);
                        sr += c.r; sg += c.g; sb += c.b; cnt++;
                    }
                    res = {sr / cnt, sg / cnt, sb / cnt, 1.f};
                }
            } else g_mmUnread++;
        }
    }
    g_colVoxel[voxel] = res;
    *out = res;
    return res.a > 0.5f;
}
static std::vector<MMDot> g_allies;
static int g_mmMyTeam = -1, g_mmHandlers = 0, g_mmCols = 0;
static double g_mmDotsT = 0;

static void mm_reset(void* cam) { g_mm.state = 0; g_mm.cam0 = cam; g_mm.tex = nullptr; g_mm.nextT = 0; g_mm.idx = 0; g_allies.clear(); g_colCollider.clear(); g_colVoxel.clear(); g_mmHit = g_mmHandler = g_mmVoxel = g_mmColor = g_mmUnread = 0; }
static void* type_obj(void* klass);
// границы карты: объединение границ всех обычных (не триггеров) коллайдеров
static bool mm_collect_bounds() {
    if (!mi_FindObjs || !kColCls || !mi_ColBounds) return false;
    void* arr = ((fn_findobj)fptr(mi_FindObjs))(type_obj(kColCls), mi_FindObjs);
    if (!arr) return false;
    int n = *(int*)((char*)arr + 0xC);
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f, y0 = 1e9f, y1 = -1e9f; int cnt = 0;
    for (int i = 0; i < n && i < 30000; i++) {
        void* col = *(void**)((char*)arr + 0x10 + 4 * i);
        if (!col) continue;
        if (mi_ColTrigger && ((fn_getBool)fptr(mi_ColTrigger))(col, mi_ColTrigger)) continue;
        Bnds b = ((fn_getBounds)fptr(mi_ColBounds))(col, mi_ColBounds);
        if (isnan(b.c.x) || isnan(b.c.y) || isnan(b.c.z)) continue;
        if (b.e.x > 120.f || b.e.z > 120.f || b.e.y > 120.f) continue;           // огромные служебные коллайдеры не считаем
        x0 = fminf(x0, b.c.x - b.e.x); x1 = fmaxf(x1, b.c.x + b.e.x);
        z0 = fminf(z0, b.c.z - b.e.z); z1 = fmaxf(z1, b.c.z + b.e.z);
        y0 = fminf(y0, b.c.y - b.e.y); y1 = fmaxf(y1, b.c.y + b.e.y);
        cnt++;
    }
    g_mmCols = cnt;
    if (cnt < 30) return false;                                                  // карта ещё не загрузилась
    float sx = x1 - x0, sz = z1 - z0, S = fmaxf(sx, sz) * 1.04f;
    if (S < 8.f || S > 1500.f || y1 - y0 < 0.5f) { g_mm.state = 4; return false; }
    g_mm.minX = (x0 + x1) * 0.5f - S * 0.5f; g_mm.minZ = (z0 + z1) * 0.5f - S * 0.5f; g_mm.span = S;
    g_mm.minY = y0; g_mm.maxY = y1; g_mm.idx = 0;
    blog("мини-карта: границы x[%.1f %.1f] z[%.1f %.1f] y[%.1f %.1f] коллайдеров=%d", x0, x1, z0, z1, y0, y1, cnt);
    return true;
}
static void mm_build_texture() {
    if (!g_kTex || !mi_TexCtor || !(mi_SetPix3 || mi_SetPix4) || !(mi_TexApply0 || mi_TexApply2)) { g_mm.state = 4; return; }
    void* t = il2cpp_object_new(g_kTex);
    ((fn_ctor2)fptr(mi_TexCtor))(t, MM_N, MM_N, mi_TexCtor);
    float range = g_mm.maxY - g_mm.minY + 0.001f;
    for (int gz = 0; gz < MM_N; gz++)
        for (int gx = 0; gx < MM_N; gx++) {
            int i = gz * MM_N + gx; float h = g_mm.h[i];
            Col c = {0.f, 0.f, 0.f, 0.f};                                          // пусто (луч ничего не встретил)
            if (!isnan(h)) {
                float k = (h - g_mm.minY) / range; if (k < 0.f) k = 0.f; if (k > 1.f) k = 1.f;
                float r = 0.20f + 0.64f * k, g = 0.25f + 0.55f * k, b = 0.33f + 0.36f * k;   // запасной цвет по высоте: низ - тёмно-синий, верх - песочный
                if (g_mm.has[i]) {                                                   // настоящий цвет блока, чуть светлее на высоте
                    float sh = 0.80f + 0.20f * k;
                    r = g_mm.rgb[i * 3] / 255.f * sh; g = g_mm.rgb[i * 3 + 1] / 255.f * sh; b = g_mm.rgb[i * 3 + 2] / 255.f * sh;
                }
                float d = 0.f;
                if (gx > 0 && !isnan(g_mm.h[i - 1])) d = fmaxf(d, fabsf(h - g_mm.h[i - 1]));
                if (gz > 0 && !isnan(g_mm.h[i - MM_N])) d = fmaxf(d, fabsf(h - g_mm.h[i - MM_N]));
                if (d > 1.4f) { r *= 0.6f; g *= 0.6f; b *= 0.6f; }               // край стены/уступа темнее
                c = {r, g, b, 1.f};
            }
            if (mi_SetPix3) ((fn_setPix3)fptr(mi_SetPix3))(t, gx, gz, c, mi_SetPix3);   // gz растёт к северу, у Unity y=0 снизу - совпадает
            else ((fn_setPix4)fptr(mi_SetPix4))(t, gx, gz, c, 0, mi_SetPix4);
        }
    if (mi_TexApply0) ((fn_v0)fptr(mi_TexApply0))(t, mi_TexApply0); else ((fn_apply2)fptr(mi_TexApply2))(t, true, false, mi_TexApply2);
    if (mi_SetHide) ((fn_setHide)fptr(mi_SetHide))(t, 61, mi_SetHide);
    il2cpp_gchandle_new(t, 1);
    g_mm.tex = t; g_mm.state = 3;
    blog("мини-карта: текстура готова");
}
// порция лучей сверху вниз за один вызов
static void mm_sample_batch() {
    if (!mi_Ray6) { g_mm.state = 4; return; }
    alignas(8) static char hit[128];
    for (int k = 0; k < 250 && g_mm.idx < MM_N * MM_N; k++, g_mm.idx++) {
        int i = g_mm.idx, gx = i % MM_N, gz = i / MM_N;
        V3 o = {g_mm.minX + (gx + 0.5f) / MM_N * g_mm.span, g_mm.maxY + 1.f, g_mm.minZ + (gz + 0.5f) / MM_N * g_mm.span};
        memset(hit, 0, sizeof hit);
        bool ok = ((fn_ray6)fptr(mi_Ray6))(o, V3{0.f, -1.f, 0.f}, hit, (g_mm.maxY - g_mm.minY) + 3.f, -5, 1, mi_Ray6);   // 1 = триггеры игнорируем
        float y = ok ? ((float*)hit)[1] : NAN;                                   // RaycastHit.point.y
        g_mm.h[i] = (ok && !isnan(y)) ? y : NAN;
        g_mm.has[i] = 0;
        if (ok && mi_RHcollider && mi_GetCompS && off_VCHVoxel >= 0) {            // узнаём цвет блока, в который попал луч
            if (!g_vchStr) { g_vchStr = S("VoxelColliderHandler"); il2cpp_gchandle_new(g_vchStr, 0); }
            g_mmHit++;
            void* col = ((fn_getPhoton)fptr(mi_RHcollider))(hit, mi_RHcollider);
            if (col) {
                Col c = {0.f, 0.f, 0.f, 0.f}; bool got = false;
                auto it = g_colCollider.find(col);
                if (it != g_colCollider.end()) { c = it->second; got = c.a > 0.5f; }
                else {
                    void* hd = ((fn_getCompS)fptr(mi_GetCompS))(col, g_vchStr, mi_GetCompS);
                    if (hd) { g_mmHandler++; void* vox = *(void**)((char*)hd + off_VCHVoxel); if (vox) { g_mmVoxel++; got = voxel_color(vox, &c); } }
                    g_colCollider[col] = got ? c : Col{0.f, 0.f, 0.f, 0.f};
                }
                if (got) {
                    g_mm.rgb[i * 3] = (uint8_t)(fminf(1.f, c.r) * 255.f + 0.5f); g_mm.rgb[i * 3 + 1] = (uint8_t)(fminf(1.f, c.g) * 255.f + 0.5f); g_mm.rgb[i * 3 + 2] = (uint8_t)(fminf(1.f, c.b) * 255.f + 0.5f);
                    g_mm.has[i] = 1; g_mmColor++;
                }
            }
        }
    }
    if (g_mm.idx >= MM_N * MM_N) mm_build_texture();
}
// раз в 0.25 с: ищем союзников (аватары с той же командой, что у вас)
static void mm_update_dots() {
    g_allies.clear();
    if (!g_kSpawnObj || !mi_FindObjs || !mi_getPhoton || off_IsLocal < 0 || !mi_CompTransform || !mi_TrPos) return;
    void* arr = ((fn_findobj)fptr(mi_FindObjs))(type_obj(g_kSpawnObj), mi_FindObjs);
    if (!arr) return;
    int n = *(int*)((char*)arr + 0xC);
    g_mmHandlers = n;
    struct Rec { void* pl; float x, z; bool local, dead; } recs[64]; int rn = 0;
    for (int i = 0; i < n && i < 64; i++) {
        void* hd = *(void**)((char*)arr + 0x10 + 4 * i);
        if (!hd) continue;
        void* pl = ((fn_getPhoton)fptr(mi_getPhoton))(hd, mi_getPhoton);
        if (!pl) continue;
        void* tr = ((fn_getPhoton)fptr(mi_CompTransform))(hd, mi_CompTransform);
        if (!tr) continue;
        V3 p = ((fn_getV3)fptr(mi_TrPos))(tr, mi_TrPos);
        Rec r; r.pl = pl; r.x = p.x; r.z = p.z; r.local = *((uint8_t*)pl + off_IsLocal) != 0;
        r.dead = mi_HandIsDead ? ((fn_getBool)fptr(mi_HandIsDead))(hd, mi_HandIsDead) : false;
        recs[rn++] = r;
    }
    int myTeam = -1;
    for (int i = 0; i < rn; i++) if (recs[i].local) prop_int(recs[i].pl, "team", &myTeam);
    g_mmMyTeam = myTeam;
    if (myTeam < 0) return;
    for (int i = 0; i < rn; i++) {
        if (recs[i].local || recs[i].dead) continue;
        int t = -1;
        if (prop_int(recs[i].pl, "team", &t) && t == myTeam) g_allies.push_back({recs[i].x, recs[i].z});
    }
}
static void minimap_draw(float sw, float sh, float sc) {
    if (!MINIMAP_ENABLED || !icons_ready()) return;
    void* cam = mi_CamMain ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    if (!cam || !mi_CompTransform || !mi_TrPos) return;
    if (cam != g_mm.cam0) mm_reset(cam);                                          // новая сцена/матч - строим карту заново
    double t = now_s();
    if (g_mm.state == 0 && t >= g_mm.nextT) { if (mm_collect_bounds()) g_mm.state = 1; else if (g_mm.state == 0) g_mm.nextT = t + 1.0; }
    if (g_mm.state == 1) mm_sample_batch();
    if (t - g_mmDotsT > 0.25) { g_mmDotsT = t; mm_update_dots(); }
    // положение панели: справа вверху, левее значка здоровья и правее счёта, чтобы ничего не закрывать
    float size = sh * MM_PANEL, right = sw * 0.893f, top = sh * 0.012f, left = right - size;
    float pad = sh * 0.005f;
    Box({(left - pad) / sc, (top - pad) / sc, (size + 2 * pad) / sc, (size + 2 * pad) / sc}, "");
    if (DEBUG_OVERLAY) {
        char b[160]; snprintf(b, sizeof b, "мини-карта: состояние=%d коллайдеров=%d аватаров=%d моя команда=%d союзников=%d", g_mm.state, g_mmCols, g_mmHandlers, g_mmMyTeam, (int)g_allies.size());
        Lbl({194, 420, 900, 22}, std::string(b));
        char c2[200]; snprintf(c2, sizeof c2, "цвета блоков: лучей=%d обработчик=%d воксель=%d цвет=%d нечитаемых=%d тип=%s", g_mmHit, g_mmHandler, g_mmVoxel, g_mmColor, g_mmUnread, g_mmTexInfo);
        Lbl({194, 440, 900, 22}, std::string(c2));
    }
    if (g_mm.state != 3 || !g_mm.tex) return;
    // карта показывается окном вокруг вас и двигается вместе с вами: вы всегда в центре, север сверху
    void* ctr = ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform);
    if (!ctr) return;
    V3 cp = ((fn_getV3)fptr(mi_TrPos))(ctr, mi_TrPos);
    bool win = mi_BeginGroup && mi_EndGroup && g_guiContent && g_guiStyle;       // умеем обрезать - показываем окно вокруг игрока, иначе всю карту целиком
    float S = g_mm.span, view = win ? fminf(S, fmaxf(32.f, S * MM_VIEW)) : S, h = view * 0.5f;
    float vx0 = win ? cp.x - h : g_mm.minX, vz1 = win ? cp.z + h : g_mm.minZ + S;   // левая и верхняя (по z) границы показанной области в мире
    if (win) {
        // вся текстура рисуется в масштабе и со сдвигом, а лишнее срезает группа размером с панель
        ((fn_beginGroup)fptr(mi_BeginGroup))(Rect{left / sc, top / sc, size / sc, size / sc}, g_guiContent, g_guiStyle, mi_BeginGroup);
        draw_tex(g_mm.tex, Rect{(g_mm.minX - vx0) / view * size / sc, (vz1 - (g_mm.minZ + S)) / view * size / sc, S / view * size / sc, S / view * size / sc}, Col{1.f, 1.f, 1.f, 0.95f});
        ((void (*)(void*))fptr(mi_EndGroup))(mi_EndGroup);                     // статический метод без параметров
    } else draw_tex(g_mm.tex, Rect{left / sc, top / sc, size / sc, size / sc}, Col{1.f, 1.f, 1.f, 0.95f});
    int team = g_mmMyTeam >= 0 ? cur_team() : 0;                                  // 1 красная, 2 синяя (как определил мод по скину)
    Col tc = team == 1 ? Col{1.f, 0.30f, 0.30f, 1.f} : team == 2 ? Col{0.30f, 0.55f, 1.f, 1.f} : Col{0.35f, 1.f, 0.45f, 1.f};
    float r = size * 0.035f;
    for (auto& a : g_allies) {
        float u = (a.x - vx0) / view, v = (vz1 - a.z) / view;                    // доли окна: u вправо, v вниз
        if (u < 0.02f || u > 0.98f || v < 0.02f || v > 0.98f) continue;           // вне окна - не рисуем
        float px = left + u * size, py = top + v * size;
        icon_draw(7, {(px - r) / sc, (py - r) / sc, 2 * r / sc, 2 * r / sc}, Col{0.f, 0.f, 0.f, 0.8f});        // тёмная обводка
        icon_draw(7, {(px - r * 0.78f) / sc, (py - r * 0.78f) / sc, 1.56f * r / sc, 1.56f * r / sc}, tc);
    }
    // вы: белая точка в центре и жёлтая точка по направлению взгляда
    {
        float px = left + size * 0.5f, py = top + size * 0.5f;
        if (mi_TrGetRot) {
            Quat q = ((fn_getQ)fptr(mi_TrGetRot))(ctr, mi_TrGetRot);
            float fx = 2.f * (q.x * q.z + q.w * q.y), fz = 1.f - 2.f * (q.x * q.x + q.y * q.y);   // вектор взгляда на плоскости XZ
            float len = sqrtf(fx * fx + fz * fz);
            if (len > 0.001f) { fx /= len; fz /= len; icon_draw(7, {(px + fx * r * 2.3f - r * 0.6f) / sc, (py - fz * r * 2.3f - r * 0.6f) / sc, 1.2f * r / sc, 1.2f * r / sc}, Col{1.f, 0.9f, 0.2f, 1.f}); }
        }
        icon_draw(7, {(px - r * 1.05f) / sc, (py - r * 1.05f) / sc, 2.1f * r / sc, 2.1f * r / sc}, Col{0.f, 0.f, 0.f, 0.8f});
        icon_draw(7, {(px - r * 0.85f) / sc, (py - r * 0.85f) / sc, 1.7f * r / sc, 1.7f * r / sc}, Col{1.f, 1.f, 1.f, 1.f});
    }
}

// ================= погода: снег и дождь =================
// Включает хозяин комнаты (кнопка под мини-картой), событие 152 уходит остальным игрокам с модом. Эффект рисуется поверх экрана:
// частицы падают, а при повороте камеры сдвигаются как настоящие (параллакс).
static int g_weather = 0;                                                       // 0 ясно, 1 снег, 2 дождь
static double g_weatherSentT = 0;
static void* g_weatherRoom = nullptr; static void* mi_PNRoom = nullptr;       // текущая комната: при выходе/входе в другую погода сбрасывается
static const uint8_t WEATHER_EVENT = 152;
static void *mi_PNIsMaster = nullptr, *mi_PNMaster = nullptr, *mi_PlActor = nullptr, *mi_EvCurrent = nullptr, *mi_EvType = nullptr;
static bool pn_is_master() { return mi_PNIsMaster && ((bool (*)(void*))fptr(mi_PNIsMaster))(mi_PNIsMaster); }
static bool is_repaint() {                                                      // OnGUI вызывается несколько раз за кадр, рисуем только при Repaint
    if (!mi_EvCurrent || !mi_EvType) return true;
    void* ev = ((fn_stat0)fptr(mi_EvCurrent))(mi_EvCurrent);
    if (!ev) return true;
    return ((fn_geti)fptr(mi_EvType))(ev, mi_EvType) == 7;
}
static void weather_send(int mode, bool reliable) {
    if (!mi_RaiseEvent || !g_reoField || !kByteCls) return;
    static bool reoInit = false;
    if (!reoInit && kREOcls) { il2cpp_runtime_class_init(kREOcls); reoInit = true; }
    void* opts = nullptr;
    il2cpp_field_static_get_value(g_reoField, &opts);                           // RaiseEventOptions.Default (получатели: остальные)
    if (!opts) return;
    void* arr = il2cpp_array_new(kByteCls, 2);
    if (!arr) return;
    uint8_t* d = (uint8_t*)arr + 4 * sizeof(void*);
    d[0] = 0x57; d[1] = (uint8_t)mode;
    SendOpts so = {reliable ? 2 : 0, 0, 0, 0};                                  // 2 = Reliable
    bool ok = ((fn_raise)fptr(mi_RaiseEvent))(WEATHER_EVENT, arr, opts, so, mi_RaiseEvent);
    blog("погода: отправлено режим=%d ok=%d", mode, ok ? 1 : 0);
}
// приём: принимаем только от хозяина комнаты
static void weather_receive(void* ed) {
    if (!WEATHER_ENABLED || !mi_EDData || !mi_EDSender) return;
    void* data = ((fn_getPhoton)fptr(mi_EDData))(ed, mi_EDData);
    if (!data) return;
    const char* cn = il2cpp_class_get_name(il2cpp_object_get_class(data));
    if (!cn || strcmp(cn, "Byte[]")) return;
    size_t len = *(size_t*)((char*)data + 3 * sizeof(void*));
    const uint8_t* b = (const uint8_t*)data + 4 * sizeof(void*);
    if (len < 2 || b[0] != 0x57 || b[1] > 2) return;
    int sender = ((fn_geti)fptr(mi_EDSender))(ed, mi_EDSender);
    if (mi_PNMaster && mi_PlActor) {
        void* master = ((fn_stat0)fptr(mi_PNMaster))(mi_PNMaster);
        if (!master || ((fn_geti)fptr(mi_PlActor))(master, mi_PlActor) != sender) return;   // не от хозяина - игнорируем
    }
    if (g_weather != b[1]) blog("погода: хозяин включил режим %d", (int)b[1]);
    g_weather = b[1];
}

struct WPart { float x, y, z, ph; };
static const int WN = 170;
static WPart g_wp[WN];
static bool g_wpInit = false;
static double g_wpT = 0;
static float g_wpYaw = 0.f, g_wpPitch = 0.f; static bool g_wpRot = false;
static unsigned g_wpRs = 12345u;
static float wrnd() { g_wpRs = g_wpRs * 1664525u + 1013904223u; return (float)(g_wpRs >> 8) / 16777216.f; }
// ---- осадки не идут под крышей: для каждой клетки 0.75x0.75 запоминаем высоту самой верхней поверхности (луч сверху вниз) ----
struct RoofCell { float top; double t; };
static std::map<long long, RoofCell> g_roof;
static float roof_top(float x, float z, double now, int& budget, float camY) {
    int ix = (int)floorf(x / 0.75f), iz = (int)floorf(z / 0.75f);
    long long key = ((long long)ix << 32) ^ (long long)(unsigned)iz;
    auto it = g_roof.find(key);
    if (it != g_roof.end() && now - it->second.t < 4.0) return it->second.top;      // свежие данные
    if (!mi_Ray6 || budget <= 0) return it != g_roof.end() ? it->second.top : -1e9f;   // нет данных - считаем небо открытым
    budget--;
    alignas(8) static char hit[128];
    memset(hit, 0, sizeof hit);
    V3 o = {(ix + 0.5f) * 0.75f, camY + 60.f, (iz + 0.5f) * 0.75f};
    bool ok = ((fn_ray6)fptr(mi_Ray6))(o, V3{0.f, -1.f, 0.f}, hit, 220.f, -5, 1, mi_Ray6);
    float top = (ok && !isnan(((float*)hit)[1])) ? ((float*)hit)[1] : -1e9f;
    g_roof[key] = RoofCell{top, now};
    return top;
}
// ---- осадки в мире: частицы живут в объёме вокруг камеры и проецируются на экран с перспективой ----
// Смотришь вверх - летят на тебя и расходятся от точки над головой, вниз - уходят вдаль к точке под ногами.
struct WP3 { float p[3]; float ph, rn; };
static const int WN3 = 520;
static WP3 g_w3[WN3];
static bool g_w3Init = false;
static void qrot(const Quat& q, float vx, float vy, float vz, float* o) {         // поворот вектора кватернионом
    float tx = 2.f * (q.y * vz - q.z * vy), ty = 2.f * (q.z * vx - q.x * vz), tz = 2.f * (q.x * vy - q.y * vx);
    o[0] = vx + q.w * tx + (q.y * tz - q.z * ty);
    o[1] = vy + q.w * ty + (q.z * tx - q.x * tz);
    o[2] = vz + q.w * tz + (q.x * ty - q.y * tx);
}
static inline float wrapf(float v, float h) { return v - 2.f * h * floorf((v + h) / (2.f * h)); }   // в диапазон [-h, h)
static inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
static void weather_draw_world(float sw, float sh, float sc, void* cam, void* ctr) {
    V3 cp = ((fn_getV3)fptr(mi_TrPos))(ctr, mi_TrPos);
    Quat q = ((fn_getQ)fptr(mi_TrGetRot))(ctr, mi_TrGetRot);
    Quat qi = {-q.x, -q.y, -q.z, q.w};                                              // обратный поворот: мир -> камера
    float fov = 60.f;
    if (mi_FOV) { fov = ((fn_getF)fptr(mi_FOV))(cam, mi_FOV); if (!(fov > 10.f && fov < 170.f)) fov = 60.f; }
    float focal = (sh * 0.5f) / tanf(fov * 0.5f * 3.14159265f / 180.f);
    float u = sh / 720.f;
    const float RH = 15.f, HV = 12.f, YC = 3.f;                                    // полуразмеры объёма вокруг камеры; центр чуть выше камеры
    double t = now_s();
    float dt = g_wpT > 0 ? (float)(t - g_wpT) : 0.016f; g_wpT = t; if (dt > 0.1f) dt = 0.1f; if (dt < 0.f) dt = 0.f;
    if (!g_w3Init) {
        for (auto& w : g_w3) { w.p[0] = cp.x + (wrnd() * 2.f - 1.f) * RH; w.p[1] = cp.y + YC + (wrnd() * 2.f - 1.f) * HV; w.p[2] = cp.z + (wrnd() * 2.f - 1.f) * RH; w.ph = wrnd() * 6.2831853f; w.rn = wrnd(); }
        g_w3Init = true;
    }
    bool rain = g_weather == 2;
    draw_tex(g_icon[10], Rect{0.f, 0.f, sw / sc, sh / sc}, rain ? Col{0.40f, 0.50f, 0.65f, 0.12f} : Col{0.90f, 0.95f, 1.00f, 0.05f});   // лёгкий оттенок
    Mat base = ((fn_getMat)fptr(mi_getMat))(mi_getMat);
    int drawn = 0, roofBudget = 120;                                                 // не больше 120 новых лучей за кадр
    for (auto& w : g_w3) {
        if (rain) { w.p[1] -= 20.f * dt; w.p[0] += 2.5f * dt; }
        else { w.p[1] -= (1.6f + 0.8f * w.rn) * dt; w.p[0] += sinf((float)t * 0.9f + w.ph) * 0.7f * dt; w.p[2] += cosf((float)t * 0.7f + w.ph) * 0.5f * dt; }
        float rx = wrapf(w.p[0] - cp.x, RH), ry = wrapf(w.p[1] - cp.y - YC, HV) + YC, rz = wrapf(w.p[2] - cp.z, RH);   // частица, ушедшая за границу, появляется с другой стороны
        w.p[0] = cp.x + rx; w.p[1] = cp.y + ry; w.p[2] = cp.z + rz;
        float v[3]; qrot(qi, rx, ry, rz, v);                                        // в системе камеры: x вправо, y вверх, z вперёд
        if (v[2] < 0.5f || drawn >= 240) continue;
        float sx = sw * 0.5f + v[0] / v[2] * focal, sy = sh * 0.5f - v[1] / v[2] * focal;
        if (sx < -40.f || sx > sw + 40.f || sy < -140.f || sy > sh + 140.f) continue;
        float fade = clamp01((v[2] - 0.5f) / 1.0f) * clamp01(1.f - v[2] / (RH * 1.3f));   // у самой камеры и вдали прозрачнее
        if (fade <= 0.02f) continue;
        if (cp.y + ry < roof_top(cp.x + rx, cp.z + rz, t, roofBudget, cp.y) + 0.1f) continue;   // над частицей есть крыша/объект - здесь осадков нет
        if (rain) {
            float tv[3]; qrot(qi, rx - 0.1f, ry + 0.8f, rz, tv);                    // хвост капли выше (капля летит вниз и чуть вбок)
            if (tv[2] < 0.3f) continue;
            float tx = sw * 0.5f + tv[0] / tv[2] * focal, ty = sh * 0.5f - tv[1] / tv[2] * focal;
            float dx = sx - tx, dy = sy - ty, L = sqrtf(dx * dx + dy * dy);
            if (L < 2.f) { dx = 0.f; dy = 1.f; L = 2.f; } else { dx /= L; dy /= L; }
            if (L > sh * 0.4f) L = sh * 0.4f;
            float wd = fminf(3.2f * u, fmaxf(1.0f * u, 0.035f * focal / v[2]));
            float phi = atan2f(-dx, dy), cs = cosf(phi), sn = sinf(phi);           // повернуть вертикальный штрих вдоль направления на экране
            float cx = (sx + tx) * 0.5f, cy = (sy + ty) * 0.5f;
            Mat m = {}; m.m[0] = sc * cs; m.m[1] = sc * sn; m.m[4] = -sc * sn; m.m[5] = sc * cs; m.m[10] = 1; m.m[15] = 1; m.m[12] = cx; m.m[13] = cy;
            ((fn_setMat)fptr(mi_setMat))(m, mi_setMat);
            icon_draw(9, Rect{-wd * 0.5f / sc, -L * 0.5f / sc, wd / sc, L / sc}, Col{0.78f, 0.86f, 1.f, 0.55f * fade});
        } else {
            float sz = fminf(16.f * u, fmaxf(2.f * u, 0.06f * focal / v[2] * (0.7f + 0.6f * w.rn)));
            icon_draw(8, Rect{(sx - sz * 0.5f) / sc, (sy - sz * 0.5f) / sc, sz / sc, sz / sc}, Col{1.f, 1.f, 1.f, (0.5f + 0.5f * w.rn) * fade});
        }
        drawn++;
    }
    if (rain) ((fn_setMat)fptr(mi_setMat))(base, mi_setMat);
}
static void weather_draw(float sw, float sh, float sc, bool inMatch) {
    if (!WEATHER_ENABLED) return;
    void* cam = mi_CamMain ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    if (!inMatch || !cam) return;
    if (mi_PNRoom) {                                                                // другая комната - погода сбрасывается (смена камеры при смерти её не трогает)
        void* room = ((fn_stat0)fptr(mi_PNRoom))(mi_PNRoom);
        if (room != g_weatherRoom) { g_weatherRoom = room; g_weather = 0; g_wpRot = false; g_roof.clear(); }
    }
    if (g_weather == 0 || !icons_ready() || !is_repaint()) return;
    {   // нормальный путь: частицы в мире с перспективой
        void* ctr0 = (mi_CompTransform && mi_TrPos && mi_TrGetRot) ? ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform) : nullptr;
        if (ctr0) { weather_draw_world(sw, sh, sc, cam, ctr0); return; }
    }
    double t = now_s();                                                              // запасной путь (нет данных камеры): плоский эффект по экрану
    float dt = g_wpT > 0 ? (float)(t - g_wpT) : 0.016f; g_wpT = t; if (dt > 0.1f) dt = 0.1f; if (dt < 0.f) dt = 0.f;
    if (!g_wpInit) { for (auto& p : g_wp) { p.x = wrnd() * sw; p.y = wrnd() * sh; p.z = 0.15f + 0.85f * wrnd(); p.ph = wrnd() * 6.2831853f; } g_wpInit = true; }
    float u = sh / 720.f;
    // поворот камеры -> все частицы сдвигаются в обратную сторону (как будто они висят в мире)
    float dx = 0.f, dy = 0.f;
    void* ctr = mi_CompTransform ? ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform) : nullptr;
    if (ctr && mi_TrGetRot) {
        Quat q = ((fn_getQ)fptr(mi_TrGetRot))(ctr, mi_TrGetRot);
        float fx = 2.f * (q.x * q.z + q.w * q.y), fy = 2.f * (q.y * q.z - q.w * q.x), fz = 1.f - 2.f * (q.x * q.x + q.y * q.y);
        float yaw = atan2f(fx, fz), pitch = asinf(fy < -1.f ? -1.f : fy > 1.f ? 1.f : fy);
        if (g_wpRot) {
            float dyaw = yaw - g_wpYaw; if (dyaw > 3.14159265f) dyaw -= 6.2831853f; if (dyaw < -3.14159265f) dyaw += 6.2831853f;
            float fov = 60.f;
            if (mi_FOV) { fov = ((fn_getF)fptr(mi_FOV))(cam, mi_FOV); if (!(fov > 10.f && fov < 170.f)) fov = 60.f; }
            float focal = (sh * 0.5f) / tanf(fov * 0.5f * 3.14159265f / 180.f);     // пикселей на радиан
            dx = -dyaw * focal; dy = (pitch - g_wpPitch) * focal;
        }
        g_wpYaw = yaw; g_wpPitch = pitch; g_wpRot = true;
        if (fabsf(dx) > sw * 0.5f || fabsf(dy) > sh * 0.5f) { dx = dy = 0.f; }      // резкий телепорт камеры - не дёргаем частицы
    }
    bool rain = g_weather == 2;
    // лёгкий оттенок на весь экран
    draw_tex(g_icon[10], Rect{0.f, 0.f, sw / sc, sh / sc}, rain ? Col{0.40f, 0.50f, 0.65f, 0.12f} : Col{0.90f, 0.95f, 1.00f, 0.05f});
    Mat base = ((fn_getMat)fptr(mi_getMat))(mi_getMat);
    if (rain) {                                                                     // дождь наклонён: матрица с поворотом вокруг центра экрана
        float th = -0.20f, cs = cosf(th), sn = sinf(th), cx = sw * 0.5f, cy = sh * 0.5f;
        Mat m = {}; m.m[0] = sc * cs; m.m[1] = sc * sn; m.m[4] = -sc * sn; m.m[5] = sc * cs; m.m[10] = 1; m.m[15] = 1;
        m.m[12] = cx - (cs * cx - sn * cy); m.m[13] = cy - (sn * cx + cs * cy);
        ((fn_setMat)fptr(mi_setMat))(m, mi_setMat);
    }
    float pad = 60.f * u;
    for (auto& p : g_wp) {
        if (rain) { p.y += (1100.f + 900.f * p.z) * u * dt; p.x += 0.f; }
        else { p.y += (55.f + 130.f * p.z) * u * dt; p.x += sinf((float)t * 1.2f + p.ph) * (14.f + 22.f * p.z) * u * dt; }
        p.x += dx; p.y += dy;
        float w = rain ? (2.0f + 2.6f * p.z) * u : (4.f + 12.f * p.z) * u, h = rain ? (26.f + 60.f * p.z) * u : w;
        if (p.y > sh + h + pad) { p.y -= sh + 2 * (h + pad); p.x = wrnd() * sw; }
        if (p.y < -h - pad - sh * 0.2f) p.y += sh + 2 * (h + pad);
        if (p.x < -pad) p.x += sw + 2 * pad; if (p.x > sw + pad) p.x -= sw + 2 * pad;
        float a = rain ? 0.22f + 0.40f * p.z : 0.45f + 0.5f * p.z;
        icon_draw(rain ? 9 : 8, Rect{(p.x - w * 0.5f) / sc, (p.y - h * 0.5f) / sc, w / sc, h / sc}, rain ? Col{0.78f, 0.86f, 1.f, a} : Col{1.f, 1.f, 1.f, a});
    }
    if (rain) ((fn_setMat)fptr(mi_setMat))(base, mi_setMat);
}
// кнопка хозяина комнаты и периодическая рассылка текущей погоды (чтобы зашедшие позже тоже получили)
static void weather_ui(float sw, float sh, float sc, bool inMatch) {
    if (!WEATHER_ENABLED || !inMatch) return;
    double t = now_s();
    bool master = pn_is_master();
    if (!master) return;
    if (g_weather != 0 && t - g_weatherSentT > 5.0) { g_weatherSentT = t; weather_send(g_weather, false); }
    float size = sh * MM_PANEL, left = sw * 0.893f - size, y = sh * 0.012f + size + sh * 0.02f;
    static const char* const names[3] = {"ясно", "снег", "дождь"};
    if (Btn(Rect{left / sc, y / sc, size / sc, sh * 0.07f / sc}, std::string("Погода: ") + names[g_weather])) {
        g_weather = (g_weather + 1) % 3;                                            // ясно -> снег -> дождь -> ясно
        g_weatherSentT = t;
        weather_send(g_weather, true);
    }
}

// ================= смена времени суток =================
// Утро -> день -> вечер -> ночь по кругу, плавно. Время берётся из серверных часов Photon, поэтому у всех в комнате оно одинаковое.
// Эффект - прозрачный цветной фильтр (Quad) перед камерой в мире, а не рисунок поверх экрана, так что кнопки и счёт не темнеют.
// ---- ночное небо: градиент, звёзды, луна (равнопромежуточная развёртка; строка 0 - низ, как у текстур Unity) ----
static inline float sk_hash(int x, int y, int sd) { unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)sd * 2147483647u; h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16; return (h & 0xFFFF) / 65535.f; }
static inline float sk_sstep(float a, float b, float v) { float t = (v - a) / (b - a); t = t < 0.f ? 0.f : t > 1.f ? 1.f : t; return t * t * (3.f - 2.f * t); }
static void sky_rows(float* out, int W, int H, int y0, int rows) {                // rows строк с y0 -> RGBA float
    const float PI = 3.14159265f;
    const float mphi = 0.30f * 2.f * PI, mlat = 0.58f * PI / 2.f;                 // направление на луну
    float mdx = cosf(mlat) * cosf(mphi), mdy = sinf(mlat), mdz = cosf(mlat) * sinf(mphi);
    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        float lat = ((y + 0.5f) / H - 0.5f) * PI, cl = cosf(lat), sl = sinf(lat);
        for (int x = 0; x < W; x++) {
            float phi = (x + 0.5f) / W * 2.f * PI;
            float dx = cl * cosf(phi), dy = sl, dz = cl * sinf(phi);
            float R, G, B;
            if (sl > 0.f) { float k = powf(sl, 0.55f); R = 0.060f + (0.008f - 0.060f) * k; G = 0.090f + (0.016f - 0.090f) * k; B = 0.200f + (0.060f - 0.200f) * k; }
            else { R = 0.03f; G = 0.04f; B = 0.08f; }
            float thr = 0.0060f * cl * (0.25f + 0.75f * sk_sstep(0.0f, 0.35f, sl));   // звёзд поровну на единицу телесного угла, у горизонта реже
            if (sl > -0.02f && sk_hash(x, y, 3) < thr) {
                float b = 0.35f + 0.65f * powf(sk_hash(y, x, 7), 2.f), tint = sk_hash(x, y, 11);
                R += b * (0.85f + 0.15f * tint); G += b * (0.88f + 0.08f * tint); B += b * (1.0f - 0.18f * tint);
            }
            float cd = dx * mdx + dy * mdy + dz * mdz; cd = cd > 1.f ? 1.f : cd < -1.f ? -1.f : cd;
            if (cd > 0.825f) {                                                    // рядом с луной: ореол и диск с «морями»
                float ang = acosf(cd);
                float glow = expf(-powf(ang / 0.20f, 2.f)) * 0.30f;
                R += 0.30f * glow; G += 0.42f * glow; B += 0.70f * glow;
                const float mr = 0.060f;
                if (ang < mr) {
                    float d = sk_sstep(mr, mr * 0.88f, ang), sea = 0.90f + 0.20f * (sk_hash(x / 3, y / 3, 5) - 0.5f);
                    R += (0.96f * sea - R) * d; G += (0.95f * sea - G) * d; B += (0.86f * sea - B) * d;
                }
            }
            float* q = out + (r * W + x) * 4;
            q[0] = R > 1.f ? 1.f : R; q[1] = G > 1.f ? 1.f : G; q[2] = B > 1.f ? 1.f : B; q[3] = 1.f;
        }
    }
}
static void *kColorCls = nullptr, *mi_TexSetPix6 = nullptr, *mi_CamFar = nullptr;
typedef void (*fn_setPix6)(void*, int, int, int, int, void*, int, void*);
static void *g_domeTr = nullptr, *g_domeMat = nullptr, *g_domeTex = nullptr;
static int g_domeState = 0, g_domeRow = 0;                                        // 0 не начали, 1 рисуем текстуру кусками, 2 готово, 3 нельзя
static const int SKY_W = 1024, SKY_H = 512, SKY_CHUNK = 32;
static const float DOME_K[][2] = {{0.00f, 0.00f}, {0.50f, 0.00f}, {0.60f, 0.30f}, {0.70f, 0.80f}, {0.78f, 1.00f}, {0.90f, 0.95f}, {0.97f, 0.30f}, {1.00f, 0.00f}};   // {доля цикла, непрозрачность купола}
static float dome_alpha(float ph) {
    int n = (int)(sizeof(DOME_K) / sizeof(DOME_K[0]));
    for (int i = 0; i + 1 < n; i++)
        if (ph >= DOME_K[i][0] && ph <= DOME_K[i + 1][0]) { float s = (ph - DOME_K[i][0]) / (DOME_K[i + 1][0] - DOME_K[i][0]); s = s * s * (3.f - 2.f * s); return DOME_K[i][1] + (DOME_K[i + 1][1] - DOME_K[i][1]) * s; }
    return 0.f;
}
// текстура неба строится по 32 строки за кадр, чтобы не было подвисаний
static void dome_build_step() {
    if (!kColorCls || !mi_TexSetPix6 || !g_kTex || !mi_TexCtor || !(mi_TexApply0 || mi_TexApply2)) { g_domeState = 3; return; }
    if (!g_domeTex) {
        void* t = il2cpp_object_new(g_kTex);
        ((fn_ctor2)fptr(mi_TexCtor))(t, SKY_W, SKY_H, mi_TexCtor);
        if (mi_SetHide) ((fn_setHide)fptr(mi_SetHide))(t, 61, mi_SetHide);
        il2cpp_gchandle_new(t, 1);
        g_domeTex = t; g_domeRow = 0;
    }
    void* arr = il2cpp_array_new(kColorCls, SKY_W * SKY_CHUNK);
    if (!arr) { g_domeState = 3; return; }
    sky_rows((float*)((char*)arr + 4 * sizeof(void*)), SKY_W, SKY_H, g_domeRow, SKY_CHUNK);
    ((fn_setPix6)fptr(mi_TexSetPix6))(g_domeTex, 0, g_domeRow, SKY_W, SKY_CHUNK, arr, 0, mi_TexSetPix6);
    g_domeRow += SKY_CHUNK;
    if (g_domeRow >= SKY_H) {
        if (mi_TexApply0) ((fn_v0)fptr(mi_TexApply0))(g_domeTex, mi_TexApply0); else ((fn_apply2)fptr(mi_TexApply2))(g_domeTex, true, false, mi_TexApply2);
        g_domeState = 2; blog("ночное небо: текстура готова");
    }
}
static bool dome_create() {
    void* go = ((fn_createPrim)fptr(mi_CreatePrim))(0, mi_CreatePrim);            // PrimitiveType.Sphere
    if (!go) return false;
    il2cpp_gchandle_new(go, 0);
    void* tr = ((fn_getPhoton)fptr(mi_GOtransform))(go, mi_GOtransform);
    if (!tr) return false;
    void* col = ((fn_getCompS)fptr(mi_GetCompS))(tr, S("SphereCollider"), mi_GetCompS);   // без коллайдера
    if (col) ((fn_obj1)fptr(mi_Destroy))(col, mi_Destroy);
    void* rend = ((fn_getCompS)fptr(mi_GetCompS))(tr, S("MeshRenderer"), mi_GetCompS);
    if (!col || !rend) { ((fn_obj1)fptr(mi_Destroy))(go, mi_Destroy); return false; }
    void* mat = il2cpp_object_new(g_kMat);
    ((fn_setObj)fptr(mi_MatCtor))(mat, g_smokeShader, mi_MatCtor);
    il2cpp_gchandle_new(mat, 0);
    ((fn_setObj)fptr(mi_MatTex))(mat, g_domeTex, mi_MatTex);
    ((fn_setColN)fptr(mi_MatSetColN))(mat, g_smokeProp, Col{0.f, 0.f, 0.f, 0.f}, mi_MatSetColN);
    ((fn_setObj)fptr(mi_RendMat))(rend, mat, mi_RendMat);
    if (mi_RendShadow) ((fn_setI)fptr(mi_RendShadow))(rend, 0, mi_RendShadow);
    if (mi_SetHide) ((fn_setHide)fptr(mi_SetHide))(go, 61, mi_SetHide);
    ((fn_setV3)fptr(mi_TrSetScale))(tr, V3{0.f, 0.f, 0.f}, mi_TrSetScale);
    g_domeTr = tr; g_domeMat = mat;
    blog("ночное небо: купол создан");
    return true;
}
static void *mi_CamNear = nullptr, *mi_PNTime = nullptr, *mi_TrSetParent = nullptr, *mi_TrSetLPos = nullptr, *mi_TrSetLRot = nullptr;
static void* g_dnCam = nullptr; static int g_dnRecreate = 0; static float g_dnNear = 0.42f;
typedef void (*fn_setParent)(void*, void*, bool, void*);
static void *g_dnGO = nullptr, *g_dnTr = nullptr, *g_dnMat = nullptr;
static int g_dnState = 0;                                                         // 0 не пробовали, 1 фильтр в мире, 2 нельзя (запасной: оттенок поверх экрана)
static const float DN_K[][5] = {                                                  // {доля цикла, R, G, B, прозрачность}
    {0.00f, 1.00f, 0.72f, 0.55f, 0.20f},                                          // утро (розово-золотистое)
    {0.12f, 1.00f, 0.92f, 0.80f, 0.06f},
    {0.25f, 1.00f, 1.00f, 1.00f, 0.00f},                                          // день
    {0.48f, 1.00f, 1.00f, 1.00f, 0.00f},
    {0.58f, 1.00f, 0.58f, 0.28f, 0.24f},                                          // вечер (оранжевый закат)
    {0.68f, 0.40f, 0.22f, 0.42f, 0.34f},                                          // сумерки
    {0.80f, 0.02f, 0.05f, 0.17f, 0.48f},                                          // ночь
    {0.90f, 0.05f, 0.08f, 0.24f, 0.40f},                                          // перед рассветом
    {1.00f, 1.00f, 0.72f, 0.55f, 0.20f},                                          // снова утро (замыкает цикл)
};
static float dn_phase() {
    double tm = mi_PNTime ? ((double (*)(void*))fptr(mi_PNTime))(mi_PNTime) : 0.0;
    if (tm <= 0.0) tm = now_s();                                                  // нет серверных часов - местные
    return (float)(fmod(tm, (double)DAY_CYCLE_SECONDS) / (double)DAY_CYCLE_SECONDS);
}
static Col dn_color(float ph) {
    int n = (int)(sizeof(DN_K) / sizeof(DN_K[0]));
    for (int i = 0; i + 1 < n; i++) {
        if (ph >= DN_K[i][0] && ph <= DN_K[i + 1][0]) {
            float s = (ph - DN_K[i][0]) / (DN_K[i + 1][0] - DN_K[i][0]); s = s * s * (3.f - 2.f * s);   // плавный переход
            return Col{DN_K[i][1] + (DN_K[i + 1][1] - DN_K[i][1]) * s, DN_K[i][2] + (DN_K[i + 1][2] - DN_K[i][2]) * s,
                       DN_K[i][3] + (DN_K[i + 1][3] - DN_K[i][3]) * s, DN_K[i][4] + (DN_K[i + 1][4] - DN_K[i][4]) * s};
        }
    }
    return Col{1.f, 1.f, 1.f, 0.f};
}
static bool dn_create(void* cam) {
    if (!icons_ready() || !smoke3d_prepare() || !mi_TrSetParent || !mi_TrSetLPos || !mi_TrSetLRot || !mi_CompTransform) return false;   // тот же материал/шейдер, что у дыма
    void* go = ((fn_createPrim)fptr(mi_CreatePrim))(5, mi_CreatePrim);            // Quad
    if (!go) return false;
    il2cpp_gchandle_new(go, 0);
    void* tr = ((fn_getPhoton)fptr(mi_GOtransform))(go, mi_GOtransform);
    if (!tr) return false;
    void* col = ((fn_getCompS)fptr(mi_GetCompS))(tr, S("MeshCollider"), mi_GetCompS);   // коллайдер убираем: пули и игроки фильтр цеплять не должны
    if (col) ((fn_obj1)fptr(mi_Destroy))(col, mi_Destroy);
    void* rend = ((fn_getCompS)fptr(mi_GetCompS))(tr, S("MeshRenderer"), mi_GetCompS);
    if (!col || !rend) { ((fn_obj1)fptr(mi_Destroy))(go, mi_Destroy); return false; }
    void* mat = il2cpp_object_new(g_kMat);
    ((fn_setObj)fptr(mi_MatCtor))(mat, g_smokeShader, mi_MatCtor);
    il2cpp_gchandle_new(mat, 0);
    ((fn_setObj)fptr(mi_MatTex))(mat, g_icon[10], mi_MatTex);                     // сплошной белый, цвет и прозрачность задаются материалом
    ((fn_setColN)fptr(mi_MatSetColN))(mat, g_smokeProp, Col{0.f, 0.f, 0.f, 0.f}, mi_MatSetColN);
    ((fn_setObj)fptr(mi_RendMat))(rend, mat, mi_RendMat);
    if (mi_RendShadow) ((fn_setI)fptr(mi_RendShadow))(rend, 0, mi_RendShadow);
    if (mi_SetHide) ((fn_setHide)fptr(mi_SetHide))(go, 61, mi_SetHide);          // HideAndDontSave: фильтр переживает смену сцен
    ((fn_setV3)fptr(mi_TrSetScale))(tr, V3{0.f, 0.f, 0.f}, mi_TrSetScale);
    // фильтр - ребёнок камеры: ездит и поворачивается вместе с ней без отставания (раньше положение ставилось после отрисовки кадра и дёргалось)
    void* ctr = ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform);
    if (!ctr) return false;
    float nearp = mi_CamNear ? ((fn_getF)fptr(mi_CamNear))(cam, mi_CamNear) : 0.3f;
    if (!(nearp > 0.001f && nearp < 5.f)) nearp = 0.3f;
    ((fn_setParent)fptr(mi_TrSetParent))(tr, ctr, false, mi_TrSetParent);
    ((fn_setV3)fptr(mi_TrSetLPos))(tr, V3{0.f, 0.f, nearp + 0.12f}, mi_TrSetLPos);       // чуть дальше ближней плоскости камеры
    ((fn_setQ)fptr(mi_TrSetLRot))(tr, Quat{0.f, 0.f, 0.f, 1.f}, mi_TrSetLRot);
    g_dnGO = go; g_dnTr = tr; g_dnMat = mat; g_dnCam = cam; g_dnNear = nearp + 0.12f;
    blog("время суток: фильтр создан и закреплён на камере");
    return true;
}
static void dn_hide() {
    if (g_dnState == 1 && g_dnTr) ((fn_setV3)fptr(mi_TrSetScale))(g_dnTr, V3{0.f, 0.f, 0.f}, mi_TrSetScale);
}
static void daynight_update(float sw, float sh, float sc, bool inMatch) {
    if (!DAYNIGHT_ENABLED) return;
    void* cam = mi_CamMain ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    // старый фильтр трогаем только пока камера та же: если камеру уничтожили, фильтр уничтожился вместе с ней
    if (!inMatch || !cam) { if (g_dnState == 1 && cam && cam == g_dnCam) dn_hide(); return; }
    if (g_dnState == 1 && cam != g_dnCam) { g_dnState = 0; if (++g_dnRecreate > 12) g_dnState = 2; }   // камера сменилась - делаем новый фильтр, старый не трогаем
    if (g_dnState == 0) { g_dnState = dn_create(cam) ? 1 : 2; if (g_dnState == 2) blog("время суток: фильтр в мире недоступен, оттенок поверх экрана"); }
    float ph = dn_phase();
    Col c = dn_color(ph);
    if (g_dnState == 1 && is_repaint()) {                                         // звёздный купол: текстура строится заранее, потом купол плавно проявляется ночью
        if (g_domeState == 0) g_domeState = 1;
        if (g_domeState == 1) dome_build_step();
        if (g_domeState == 2) {
            float da = dome_alpha(ph);
            if (da > 0.01f && !g_domeTr && !dome_create()) g_domeState = 3;
            if (g_domeTr) {
                if (da <= 0.01f) ((fn_setV3)fptr(mi_TrSetScale))(g_domeTr, V3{0.f, 0.f, 0.f}, mi_TrSetScale);
                else if (mi_CompTransform && mi_TrPos) {
                    void* ctr1 = ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform);
                    if (ctr1) {
                        V3 cp1 = ((fn_getV3)fptr(mi_TrPos))(ctr1, mi_TrPos);
                        float far1 = mi_CamFar ? ((fn_getF)fptr(mi_CamFar))(cam, mi_CamFar) : 500.f;
                        if (!(far1 > 60.f)) far1 = 60.f; if (far1 > 3000.f) far1 = 3000.f;
                        float dia = 2.f * 0.96f * far1;                           // купол почти до дальней плоскости: геометрию карты он не закрывает
                        float ang = ph * 6.2831853f * 0.8f, sa = sinf(ang * 0.5f), ca = cosf(ang * 0.5f);   // звёзды и луна медленно поворачиваются (ось наклонена)
                        const float ay = 0.9063f, az = 0.4226f;
                        ((fn_setV3)fptr(mi_TrSetPos))(g_domeTr, cp1, mi_TrSetPos);
                        ((fn_setQ)fptr(mi_TrSetRot))(g_domeTr, Quat{0.f, ay * sa, az * sa, ca}, mi_TrSetRot);
                        ((fn_setV3)fptr(mi_TrSetScale))(g_domeTr, V3{dia, dia, dia}, mi_TrSetScale);
                        ((fn_setColN)fptr(mi_MatSetColN))(g_domeMat, g_smokeProp, Col{g_smokeColMul, g_smokeColMul, g_smokeColMul, da * g_smokeColMul}, mi_MatSetColN);
                    }
                }
            }
        }
    }
    if (g_dnState == 2) {                                                         // запасной вариант: слабее, чтобы не темнить интерфейс
        if (is_repaint() && icons_ready() && c.a > 0.01f) draw_tex(g_icon[10], Rect{0.f, 0.f, sw / sc, sh / sc}, Col{c.r, c.g, c.b, c.a * 0.5f});
        return;
    }
    if (!is_repaint()) return;
    if (c.a < 0.01f) { dn_hide(); return; }                                       // днём фильтра нет вообще
    float fov = 60.f;
    if (mi_FOV) { fov = ((fn_getF)fptr(mi_FOV))(cam, mi_FOV); if (!(fov > 10.f && fov < 170.f)) fov = 60.f; }
    float hgt = 2.f * g_dnNear * tanf(fov * 0.5f * 3.14159265f / 180.f) * 1.4f, wid = hgt * (sw / sh) * 1.1f;   // с запасом по краям
    ((fn_setV3)fptr(mi_TrSetScale))(g_dnTr, V3{wid, hgt, 1.f}, mi_TrSetScale);   // локальный размер (меняется при зуме прицела)
    ((fn_setColN)fptr(mi_MatSetColN))(g_dnMat, g_smokeProp, Col{c.r * g_smokeColMul, c.g * g_smokeColMul, c.b * g_smokeColMul, c.a * g_smokeColMul}, mi_MatSetColN);
}

// ================= цифры урона: всплывают там, где вы попали по игроку =================
// Хук HitsController.MakeHit(HitPacket): из пакета берутся урон, точка попадания, признаки "моё попадание"/"в голову".
static int off_HPDmg = -1, off_HPHit = -1, off_HPHead = -1, off_HPDeath = -1, off_HPLocal = -1, off_HCLocal = -1;
struct DmgPop { float p[3]; int dmg; bool head, kill; double t; };
static std::vector<DmgPop> g_pops;
static int g_hitShown = 0, g_hitCnt[3] = {0, 0, 0};
static const char* const HIT_TAG[3] = {"MakeHit", "MakeHitRaw", "OnHitColliderHit"};
static char g_hitDbg[260] = "урон: хук не установлен";
static char g_hitLast[260] = "";
static char g_popDbg[200] = "";
static void (*orig_Hit[3])(void*, void*, void*);
static void hit_common(int which, void* self, void* packet) {
    if (!packet || off_HPDmg < 0 || off_HPLocal < 0 || off_HPHit < 0) return;
    const char* b = (const char*)packet;
    int raw; memcpy(&raw, b + off_HPDmg, 4);
    float fv; memcpy(&fv, &raw, 4);
    float dmg = (raw > 0 && raw < 100000) ? (float)raw : fv;                  // поле может быть int или float
    bool isLocal = b[off_HPLocal] != 0;                                       // пакет создан мной (я стрелял)?
    bool head = off_HPHead >= 0 && b[off_HPHead] != 0;
    bool death = off_HPDeath >= 0 && b[off_HPDeath] != 0;
    bool victimLocal = off_HCLocal >= 0 && self && ((const char*)self)[off_HCLocal] != 0;   // цель - я сам
    float hp[3]; memcpy(hp, b + off_HPHit, 12);
    g_hitCnt[which]++;
    snprintf(g_hitLast, sizeof g_hitLast, "%s: dmg=%.1f(raw=%d) мой=%d цель-я=%d голова=%d смерть=%d точка=(%.1f %.1f %.1f)", HIT_TAG[which], fv, raw, isLocal ? 1 : 0, victimLocal ? 1 : 0, head ? 1 : 0, death ? 1 : 0, hp[0], hp[1], hp[2]);
    snprintf(g_hitDbg, sizeof g_hitDbg, "урон: вызовов MakeHit=%d Raw=%d Collider=%d показано=%d", g_hitCnt[0], g_hitCnt[1], g_hitCnt[2], g_hitShown);
    if (isLocal && !victimLocal && dmg >= 0.5f && dmg < 100000.f) {
        double t = now_s();
        for (auto& d : g_pops) if (t - d.t < 0.05 && d.dmg == (int)(dmg + 0.5f)) return;   // тот же удар из другого метода
        DmgPop d; d.dmg = (int)(dmg + 0.5f); d.head = head; d.kill = death; d.t = t;
        bool zero = hp[0] == 0.f && hp[1] == 0.f && hp[2] == 0.f;
        for (int i = 0; i < 3; i++) d.p[i] = zero || isnan(hp[i]) ? NAN : hp[i];   // NAN = позиция неизвестна, рисуем у прицела
        if (g_pops.size() >= 40) g_pops.erase(g_pops.begin());
        g_pops.push_back(d);
        g_hitShown++;
    }
}
static void hook_Hit0(void* self, void* packet, void* mi) { hit_common(0, self, packet); orig_Hit[0](self, packet, mi); }
static void hook_Hit1(void* self, void* packet, void* mi) { hit_common(1, self, packet); orig_Hit[1](self, packet, mi); }
static void hook_Hit2(void* self, void* packet, void* mi) { hit_common(2, self, packet); orig_Hit[2](self, packet, mi); }
static void* mi_Hit[3] = {nullptr, nullptr, nullptr};
// крупный текст с тенью: sx/sy - пиксели экрана (начало сверху), scale - масштаб GUI
static void big_text(float sx, float sy, const std::string& s, float scale, Col c) {
    if (!mi_getMat || !mi_setMat) return;
    Mat old = ((fn_getMat)fptr(mi_getMat))(mi_getMat);
    Mat m = {}; m.m[0] = scale; m.m[5] = scale; m.m[10] = 1; m.m[15] = 1;
    ((fn_setMat)fptr(mi_setMat))(m, mi_setMat);
    float vx = sx / scale - s.size() * 3.8f, vy = sy / scale - 10.f;
    bool tint = mi_GetGuiCol && mi_SetGuiCol;
    Col oc = {1.f, 1.f, 1.f, 1.f};
    if (tint) { oc = ((fn_getCol)fptr(mi_GetGuiCol))(mi_GetGuiCol); ((fn_setCol)fptr(mi_SetGuiCol))(Col{0.f, 0.f, 0.f, c.a * 0.85f}, mi_SetGuiCol); }
    Lbl({vx + 1.3f, vy + 1.3f, 220, 30}, s);
    if (tint) ((fn_setCol)fptr(mi_SetGuiCol))(c, mi_SetGuiCol);
    Lbl({vx, vy, 220, 30}, s);
    if (tint) ((fn_setCol)fptr(mi_SetGuiCol))(oc, mi_SetGuiCol);
    ((fn_setMat)fptr(mi_setMat))(old, mi_setMat);
}
static void dmg_draw(float sw, float sh) {
    double t = now_s();
    for (size_t i = 0; i < g_pops.size();) { if (t - g_pops[i].t > 1.5) g_pops.erase(g_pops.begin() + i); else i++; }
    if (g_pops.empty()) return;
    icons_ready();                                                               // заодно находит GUI.color (для прозрачности)
    void* cam = (mi_CamMain && mi_W2S) ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    float u = sh / 720.f;
    int idx = 0, n = (int)g_pops.size();
    for (auto& d : g_pops) {
        idx++;
        float k = (float)((t - d.t) / 1.4);
        if (k > 1.f) k = 1.f;
        float sx = sw * 0.5f, sy = sh * 0.5f - 40.f * u;                        // запасной вариант: над прицелом
        bool onPoint = false;
        float pz = 0.f, px = 0.f, py = 0.f;
        if (!isnan(d.p[0]) && cam) {
            V3 w = {d.p[0], d.p[1], d.p[2]};
            V3 s = ((fn_w2s)fptr(mi_W2S))(cam, w, mi_W2S);
            px = s.x; py = sh - s.y; pz = s.z;
            if (s.z > 0.f && s.x > 0.f && s.x < sw && s.y > 0.f && s.y < sh) { sx = s.x; sy = sh - s.y; onPoint = true; }   // точка перед камерой и на экране
        }
        if (idx == n) snprintf(g_popDbg, sizeof g_popDbg, "позиция: мир=(%.1f %.1f %.1f) экран=(%.0f %.0f z=%.1f) cam=%d %s", d.p[0], d.p[1], d.p[2], px, py, pz, cam ? 1 : 0, onPoint ? "в точке попадания" : "у прицела (точка не в кадре)");
        sx += (float)((idx * 37) % 41 - 20) * u;                                 // небольшой разброс, чтобы цифры не слипались
        sy -= (30.f + 90.f * k) * u;                                             // всплывает вверх
        float a = k < 0.5f ? 1.f : 1.f - (k - 0.5f) / 0.5f;
        float pop = 1.f + 0.5f * (1.f - (k * 8.f > 1.f ? 1.f : k * 8.f));       // в начале цифра чуть крупнее
        Col c = d.head ? Col{0.35f, 0.8f, 1.f, a} : d.kill ? Col{1.f, 0.25f, 0.25f, a} : Col{1.f, 1.f, 1.f, a};   // в голову - голубой, убийство - красный, обычный - белый
        char tb[24]; snprintf(tb, sizeof tb, "-%d", d.dmg);
        big_text(sx, sy, std::string(tb), 2.4f * u * pop * (d.head ? 1.25f : 1.f), c);
    }
}

static void voice_draw_speakers(float sw, float sh, float sc, float vw);
static void voice_draw_stats(float x, float y);
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
    if (inMatch && g_voiceOk) {                                              // кнопка микрофона под кнопкой чата
        const float mx = 200.f, my = 100.f, mw = 86.f, mh = 86.f;
        bool pr = mic_pressed(sh, mx * sc, my * sc, mw * sc, mh * sc);
        g_ptt = pr ? 1 : 0;
        if (icons_ready()) {
            Box({mx, my, mw, mh}, "");
            icon_draw(0, {mx + 16, my + 16, mw - 32, mh - 32}, pr ? Col{0.45f, 1.f, 0.45f, 1.f} : Col{1.f, 1.f, 1.f, 1.f});   // зелёный, пока зажато
        } else Box({mx, my, mw, mh}, pr ? "ГОВОРИТЕ" : "МИК");
        if (DEBUG_OVERLAY) {
            Lbl({mx - 6, my + mh + 2, 420, 22}, voice_text(g_voiceDbgBuf));
            if (g_voiceRxBuf[0]) Lbl({mx - 6, my + mh + 22, 420, 22}, voice_text(g_voiceRxBuf));
            { char ib[100]; snprintf(ib, sizeof ib, "приём: хук=%s событий=%d послед.код=%d", g_hookOn ? "да" : "НЕТ", g_evAll, g_evLast);
              Lbl({mx - 6, my + mh + 42, 480, 22}, voice_text(ib));
              if (!g_hookOn && g_recvInfo[0]) Lbl({mx - 6, my + mh + 62, 480, 22}, voice_text(g_recvInfo)); }
            voice_draw_stats(mx - 6, my + mh + 64);                          // статистика приёма по каждому игроку
        }
        voice_draw_speakers(sw, sh, sc, vw);                                 // значок "говорит" над игроками
    } else {
        g_ptt = 0;
        if (DEBUG_OVERLAY && inMatch) Lbl({194, 100, 520, 22}, voice_text(g_voiceDbgBuf));
    }
    float bx = 300.f; if (bx + 170.f > vw - 4.f) bx = vw - 174.f; if (bx < 4.f) bx = 4.f;      // справа от кнопки чата (слева сверху)
    if (inMatch && Btn({bx, 8, 170, 44}, "Магазин: " + std::to_string(g_coins))) g_open = !g_open;
    if (DEBUG_OVERLAY) {
        if (!inMatch && !g_karOk) { Lbl({300, 6, 900, 24}, g_visDbg); Lbl({300, 28, 1100, 24}, g_karDbg); }
        else if (inMatch) { Lbl({300, 56, 1000, 24}, g_handDbg); Lbl({300, 78, 1000, 24}, g_remDbg); }
        Lbl({300, 100, vw - 310, 24}, g_uiDbg);
    }                                   // временная диагностика масштаба                       // временная диагностика в матче   // временная диагностика в меню

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
        if (DEBUG_OVERLAY) Lbl({X + 12, y + 70, W - 24, 24}, "диагностика: id=" + std::to_string(g_lastId) + " local=" + std::to_string(g_lastLocal) + " применён=" + std::to_string(g_lastApplied) + " sync=" + std::to_string(g_sentVal) + " чужой=" + std::to_string(g_remoteSeen) + " tester=" + std::to_string(g_testerLvl));
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
    daynight_update(sw, sh, sc, inMatch);                                    // смена времени суток
    weather_draw(sw, sh, sc, inMatch);                                       // снег / дождь
    smoke_draw(sw, sh, sc);                                                  // дым после взрывов
    dmg_draw(sw, sh);                                                        // цифры урона
    if (inMatch) minimap_draw(sw, sh, sc);                                  // мини-карта
    weather_ui(sw, sh, sc, inMatch);                                         // кнопка погоды (только хозяину комнаты)
    if (DEBUG_OVERLAY && inMatch) { Lbl({194, 360, 900, 22}, std::string(g_hitDbg)); if (g_hitLast[0]) Lbl({194, 380, 900, 22}, std::string(g_hitLast)); if (g_popDbg[0]) Lbl({194, 400, 900, 22}, std::string(g_popDbg)); }   // отладка: что приходит в хуки попаданий
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
    voice_pump();
    if (!g_verHooked) draw_ui();       // если нет VersionOuter, рисуем здесь (только в матче)
}
// VersionOuter ("v1.347" в углу) существует и в меню, и в матче: через него рисуем везде
static void (*orig_VerGUI)(void*, void*);
static void hook_VerGUI(void* self, void* mi) {
    orig_VerGUI(self, mi);
    g_lastGuiT = now_s();                // игра сейчас рисуется (нужно для музыки)
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

// ================= музыка в главном меню =================
// assets/menu_music.m4a из APK копируется в папку приложения и играется системным MediaPlayer (через JNI):
// тихо, по кругу, только в меню; в матче и при сворачивании игры плавно затихает и ставится на паузу.
static JavaVM* g_jvm = nullptr;
extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM* vm, void*) { g_jvm = vm; return JNI_VERSION_1_6; }
static bool g_musicStarted = false;
static bool g_jvmReady() {
    if (g_jvm) return true;
    typedef jint (*fn_gcvm)(JavaVM**, jsize, jsize*);
    fn_gcvm g = (fn_gcvm)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    JavaVM* vm = nullptr; jsize n = 0;
    return g && g(&vm, 1, &n) == 0 && vm;
}
static bool jex(JNIEnv* e, const char* where) {
    if (!e->ExceptionCheck()) return false;
    e->ExceptionClear();
    blog("музыка: ошибка Java в %s", where);
    return true;
}
static bool music_extract(JNIEnv* e, const std::string& dst) {
    jclass cAT = e->FindClass("android/app/ActivityThread");
    if (!cAT || jex(e, "ActivityThread")) return false;
    jmethodID mCur = e->GetStaticMethodID(cAT, "currentApplication", "()Landroid/app/Application;");
    if (!mCur || jex(e, "currentApplication")) return false;
    jobject app = e->CallStaticObjectMethod(cAT, mCur);
    if (!app || jex(e, "application")) return false;
    jclass cCtx = e->FindClass("android/content/Context");
    jmethodID mGA = e->GetMethodID(cCtx, "getAssets", "()Landroid/content/res/AssetManager;");
    if (!mGA || jex(e, "getAssets")) return false;
    jobject am = e->CallObjectMethod(app, mGA);
    if (!am || jex(e, "assets")) return false;
    jclass cAM = e->FindClass("android/content/res/AssetManager");
    jmethodID mOpen = e->GetMethodID(cAM, "open", "(Ljava/lang/String;)Ljava/io/InputStream;");
    if (!mOpen || jex(e, "open*")) return false;
    jstring nm = e->NewStringUTF(MUSIC_ASSET);
    jobject is = e->CallObjectMethod(am, mOpen, nm);
    if (!is || jex(e, "open")) return false;                           // файла в APK нет
    jclass cIS = e->FindClass("java/io/InputStream");
    jmethodID mRead = e->GetMethodID(cIS, "read", "([B)I");
    jmethodID mClose = e->GetMethodID(cIS, "close", "()V");
    jbyteArray buf = e->NewByteArray(65536);
    FILE* f = fopen(dst.c_str(), "wb");
    if (!f) { e->CallVoidMethod(is, mClose); jex(e, "close"); return false; }
    long total = 0;
    for (;;) {
        jint n = e->CallIntMethod(is, mRead, buf);
        if (jex(e, "read") || n <= 0) break;
        jbyte* pb = e->GetByteArrayElements(buf, nullptr);
        fwrite(pb, 1, (size_t)n, f);
        total += n;
        e->ReleaseByteArrayElements(buf, pb, JNI_ABORT);
    }
    fclose(f);
    e->CallVoidMethod(is, mClose);
    jex(e, "close");
    blog("музыка: файл извлечён, %ld байт", total);
    return total > 0;
}
static void* music_thread(void*) {
    JavaVM* vm = g_jvm;
    if (!vm) {
        typedef jint (*fn_gcvm)(JavaVM**, jsize, jsize*);
        fn_gcvm g = (fn_gcvm)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        jsize n = 0;
        if (g) g(&vm, 1, &n);
    }
    if (!vm) { blog("музыка: JavaVM не найден"); return nullptr; }
    JNIEnv* e = nullptr;
    if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK || !e) { blog("музыка: не удалось подключить поток к Java"); return nullptr; }
    std::string dst = g_path.substr(0, g_path.rfind('/')) + "/menu_music.m4a";
    if (!music_extract(e, dst)) {
        blog("музыка: нет assets/%s в APK, музыка отключена", MUSIC_ASSET);
        vm->DetachCurrentThread();
        return nullptr;
    }
    jclass cMP = e->FindClass("android/media/MediaPlayer");
    jmethodID mNew = e->GetMethodID(cMP, "<init>", "()V");
    jmethodID mSet = e->GetMethodID(cMP, "setDataSource", "(Ljava/lang/String;)V");
    jmethodID mPrep = e->GetMethodID(cMP, "prepare", "()V");
    jmethodID mStart = e->GetMethodID(cMP, "start", "()V");
    jmethodID mPause = e->GetMethodID(cMP, "pause", "()V");
    jmethodID mLoop = e->GetMethodID(cMP, "setLooping", "(Z)V");
    jmethodID mVol = e->GetMethodID(cMP, "setVolume", "(FF)V");
    if (jex(e, "MediaPlayer методы")) { vm->DetachCurrentThread(); return nullptr; }
    jobject mp = e->NewObject(cMP, mNew);
    if (!mp || jex(e, "new MediaPlayer")) { vm->DetachCurrentThread(); return nullptr; }
    jstring js = e->NewStringUTF(dst.c_str());
    e->CallVoidMethod(mp, mSet, js);
    if (jex(e, "setDataSource")) { vm->DetachCurrentThread(); return nullptr; }
    e->CallVoidMethod(mp, mLoop, JNI_TRUE);
    e->CallVoidMethod(mp, mVol, 0.0, 0.0);
    e->CallVoidMethod(mp, mPrep);
    if (jex(e, "prepare")) { vm->DetachCurrentThread(); return nullptr; }
    blog("музыка: готова, громкость %.2f", MUSIC_VOLUME);
    float cur = 0.f, lastSet = -1.f;
    bool playing = false;
    for (;;) {
        usleep(100000);
        double t = now_s();
        bool inMatch = g_lastHudT > 0 && t - g_lastHudT < 3.0;      // идёт матч
        bool active = g_lastGuiT > 0 && t - g_lastGuiT < 2.0;       // игра на экране (не свёрнута)
        bool want = active && !inMatch;
        float target = want ? MUSIC_VOLUME : 0.f, step = MUSIC_VOLUME / 15.f;   // плавно, около 1.5 с
        if (cur < target) cur = fminf(target, cur + step); else if (cur > target) cur = fmaxf(target, cur - step);
        if (want && !playing) { e->CallVoidMethod(mp, mStart); jex(e, "start"); playing = true; blog("музыка: играет"); }
        if (cur != lastSet) { e->CallVoidMethod(mp, mVol, (double)cur, (double)cur); jex(e, "setVolume"); lastSet = cur; }
        if (!want && playing && cur <= 0.0005f) { e->CallVoidMethod(mp, mPause); jex(e, "pause"); playing = false; blog("музыка: пауза"); }
    }
    return nullptr;
}
static void start_music() {
    if (g_musicStarted || !g_verHooked) return;       // без VersionOuter не знаем, когда игра на экране
    g_musicStarted = true;
    pthread_t t;
    pthread_create(&t, nullptr, music_thread, nullptr);
    pthread_detach(t);
}

// ================= голосовой чат рядом (зажмите кнопку микрофона) =================
// Запись: AudioRecord (JNI) 16 кГц -> 8 кГц -> IMA ADPCM (~4 КБ/с) -> события Photon (код 151, получатели: остальные в комнате).
// Приём: хук PunCallbacksService.OnEvent: свой код перехватывается, остальное идёт игре как обычно.
// Слышно в радиусе VOICE_RADIUS (по позиции камеры), громкость падает с расстоянием. Только у игроков с этим модом.
static const uint8_t VOICE_EVENT = 151;
static const bool VOICE_PROXIMITY = false;    // true = слышно только рядом (по позиции камеры); false = слышно всем в комнате
static const float VOICE_RADIUS = 150.f;      // слышно в этом радиусе (единицы мира)
static const float VOICE_GAIN = 1.0f;         // общая громкость чужого голоса
static const float VOICE_MIC_GAIN = 2.0f;     // усиление своего микрофона
static const int VOICE_CAP_RATE = 16000, VOICE_FRAME = 800, VOICE_RATE = 8000;   // кадр = 100 мс при 8 кГц

// ---- ADPCM ----
// IMA ADPCM, 4 бита на отсчёт. Пакет независим: заголовок (предсказание int16 + индекс шага) + полубайты.
static const int8_t IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static inline int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
// мягкое ограничение: без жёсткого среза пиков (он даёт хрип)
static inline int softclip(int v) { const int T = 24000; if (v > T) v = T + (v - T) / 4; else if (v < -T) v = -T + (v + T) / 4; return clamp16(v); }
// кодирует n (чётное) отсчётов в out: 3 байта заголовка + n/2 байт данных
static int adpcm_encode(const int16_t* in, int n, uint8_t* out, int& pred, int& idx) {   // pred/idx - состояние кодера, переходит из пакета в пакет
    out[0] = (uint8_t)(pred & 0xff); out[1] = (uint8_t)((pred >> 8) & 0xff); out[2] = (uint8_t)idx;
    int o = 3; uint8_t cur = 0;
    for (int i = 0; i < n; i++) {
        int step = IMA_STEP[idx], diff = in[i] - pred, code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        int vp = step >> 3;
        if (diff >= step) { code |= 4; diff -= step; vp += step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; vp += step; }
        step >>= 1;
        if (diff >= step) { code |= 1; vp += step; }
        pred = clamp16(pred + ((code & 8) ? -vp : vp));
        idx += IMA_INDEX[code]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
        if (i & 1) { out[o++] = (uint8_t)(cur | (code << 4)); } else cur = (uint8_t)code;
    }
    return o;
}
// декодирует: in = заголовок + данные, n отсчётов -> out
static void adpcm_decode(const uint8_t* in, int n, int16_t* out) {
    int pred = (int16_t)(in[0] | (in[1] << 8)), idx = in[2];
    if (idx > 88) idx = 88;
    const uint8_t* d = in + 3;
    for (int i = 0; i < n; i++) {
        int code = (i & 1) ? (d[i >> 1] >> 4) : (d[i >> 1] & 15);
        int step = IMA_STEP[idx], vp = step >> 3;
        if (code & 4) vp += step;
        if (code & 2) vp += step >> 1;
        if (code & 1) vp += step >> 2;
        pred = clamp16(pred + ((code & 8) ? -vp : vp));
        idx += IMA_INDEX[code]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
        out[i] = (int16_t)pred;
    }
}
// ---- /ADPCM ----

static JNIEnv* jni_attach() {
    JavaVM* vm = g_jvm;
    if (!vm) {
        typedef jint (*fn_gcvm)(JavaVM**, jsize, jsize*);
        fn_gcvm g = (fn_gcvm)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        jsize n = 0;
        if (g) g(&vm, 1, &n);
    }
    if (!vm) return nullptr;
    JNIEnv* e = nullptr;
    if (vm->GetEnv((void**)&e, JNI_VERSION_1_6) == JNI_OK && e) return e;
    if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
    return e;
}
static jobject cur_application(JNIEnv* e) {
    jclass cAT = e->FindClass("android/app/ActivityThread");
    if (!cAT || jex(e, "ActivityThread")) return nullptr;
    jmethodID m = e->GetStaticMethodID(cAT, "currentApplication", "()Landroid/app/Application;");
    if (!m || jex(e, "currentApplication")) return nullptr;
    jobject a = e->CallStaticObjectMethod(cAT, m);
    if (jex(e, "application")) return nullptr;
    return a;
}
static bool mic_granted(JNIEnv* e) {
    jobject app = cur_application(e);
    if (!app) return false;
    jclass cCtx = e->FindClass("android/content/Context");
    jmethodID m = e->GetMethodID(cCtx, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (!m || jex(e, "checkSelfPermission")) { e->DeleteLocalRef(app); return true; }       // старый Android: выдано при установке
    jstring js = e->NewStringUTF("android.permission.RECORD_AUDIO");
    jint r = e->CallIntMethod(app, m, js);
    bool bad = jex(e, "check");
    e->DeleteLocalRef(js);
    e->DeleteLocalRef(app);
    return !bad && r == 0;
}
static void mic_request(JNIEnv* e) {
    jobject app = cur_application(e);
    if (!app) return;
    jclass cCtx = e->FindClass("android/content/Context");
    jmethodID mGCL = e->GetMethodID(cCtx, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject cl = e->CallObjectMethod(app, mGCL);
    if (!cl || jex(e, "getClassLoader")) return;
    jclass cCL = e->FindClass("java/lang/ClassLoader");
    jmethodID mLoad = e->GetMethodID(cCL, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring nm = e->NewStringUTF("com.unity3d.player.UnityPlayer");
    jclass cUP = (jclass)e->CallObjectMethod(cl, mLoad, nm);
    if (!cUP || jex(e, "UnityPlayer")) return;
    jfieldID fid = e->GetStaticFieldID(cUP, "currentActivity", "Landroid/app/Activity;");
    if (!fid || jex(e, "currentActivity")) return;
    jobject act = e->GetStaticObjectField(cUP, fid);
    if (!act || jex(e, "activity")) return;
    jclass cAct = e->FindClass("android/app/Activity");
    jmethodID mReq = e->GetMethodID(cAct, "requestPermissions", "([Ljava/lang/String;I)V");
    jclass cStr = e->FindClass("java/lang/String");
    jobjectArray arr = e->NewObjectArray(1, cStr, nullptr);
    jstring js = e->NewStringUTF("android.permission.RECORD_AUDIO");
    e->SetObjectArrayElement(arr, 0, js);
    e->CallVoidMethod(act, mReq, arr, 4711);
    jex(e, "requestPermissions");
    blog("микрофон: запрошено разрешение");
}

static pthread_mutex_t g_outMu = PTHREAD_MUTEX_INITIALIZER;
static std::vector<std::vector<uint8_t>> g_outQ;
static float g_cam[3] = {0, 0, 0};
static bool g_camOk = false;
static double g_camT = 0;
static int g_voiceTx = 0, g_voiceRxN = 0;

static void* capture_thread(void*) {
    JNIEnv* e = jni_attach();
    if (!e) { voice_dbg("микрофон: нет доступа к Java"); return nullptr; }
    jclass cAR = e->FindClass("android/media/AudioRecord");
    jmethodID mMin = e->GetStaticMethodID(cAR, "getMinBufferSize", "(III)I");
    jmethodID mNew = e->GetMethodID(cAR, "<init>", "(IIIII)V");
    jmethodID mState = e->GetMethodID(cAR, "getState", "()I");
    jmethodID mStart = e->GetMethodID(cAR, "startRecording", "()V");
    jmethodID mStop = e->GetMethodID(cAR, "stop", "()V");
    jmethodID mRead = e->GetMethodID(cAR, "read", "([SII)I");
    jmethodID mRel = e->GetMethodID(cAR, "release", "()V");
    if (jex(e, "AudioRecord методы")) { voice_dbg("микрофон: нет AudioRecord"); return nullptr; }
    voice_dbg("микрофон: готов, зажмите кнопку");
    jobject rec = nullptr;
    bool recording = false;
    double lastReq = 0;
    uint8_t seq = 0;
    std::vector<int16_t> acc;
    jshortArray arr = e->NewShortArray(VOICE_CAP_RATE / 10);
    const int CAP = VOICE_CAP_RATE / 10;                                   // 1600 отсчётов = 100 мс
    int encPred = 0, encIdx = 0; bool encFresh = true;
    int16_t prev[3] = {0, 0, 0};
    auto emit = [&]() {                                                    // один пакет = 100 мс; недостающее добивается тишиной
        while ((int)acc.size() < CAP) acc.push_back(0);
        int16_t pcm[VOICE_FRAME];
        auto X = [&](int k) -> int { return k < 0 ? (int)prev[3 + k] : (int)acc[k]; };
        for (int i = 0; i < VOICE_FRAME; i++) {                            // 16 -> 8 кГц: НЧ-фильтр 1-4-6-4-1 (меньше шума наложения) + усиление
            int v = (X(2 * i - 3) + 4 * X(2 * i - 2) + 6 * X(2 * i - 1) + 4 * X(2 * i) + X(2 * i + 1)) / 16;
            pcm[i] = (int16_t)softclip((int)(v * VOICE_MIC_GAIN));
        }
        prev[0] = acc[CAP - 3]; prev[1] = acc[CAP - 2]; prev[2] = acc[CAP - 1];
        acc.erase(acc.begin(), acc.begin() + CAP);
        if (encFresh) { encPred = pcm[0]; encIdx = 0; encFresh = false; }
        std::vector<uint8_t> pk(14 + 3 + VOICE_FRAME / 2);
        pk[0] = 0x56; pk[1] = seq++;
        float pos[3] = {NAN, NAN, NAN};                                    // позиция говорящего (NaN = неизвестна, расстояние не считаем)
        if (g_camOk) { pos[0] = g_cam[0]; pos[1] = g_cam[1]; pos[2] = g_cam[2]; }
        memcpy(&pk[2], pos, 12);
        adpcm_encode(pcm, VOICE_FRAME, &pk[14], encPred, encIdx);
        pthread_mutex_lock(&g_outMu);
        if (g_outQ.size() > 8) g_outQ.erase(g_outQ.begin());
        g_outQ.push_back(std::move(pk));
        pthread_mutex_unlock(&g_outMu);
    };
    for (;;) {
        if (!g_ptt) {
            if (recording) {
                e->CallVoidMethod(rec, mStop); jex(e, "stop"); recording = false;
                if ((int)acc.size() > 400) emit();                         // не теряем последние слова
                acc.clear(); encFresh = true; prev[0] = prev[1] = prev[2] = 0;
                voice_dbg("микрофон: готов, зажмите кнопку");
            }
            usleep(20000);
            continue;
        }
        if (!rec) {
            if (!mic_granted(e)) {
                double t = now_s();
                if (t - lastReq > 15.0) { lastReq = t; mic_request(e); }
                voice_dbg("микрофон: разрешите доступ в окне");
                usleep(300000);
                continue;
            }
            jint minb = e->CallStaticIntMethod(cAR, mMin, VOICE_CAP_RATE, 16, 2);
            int bytes = minb > 0 ? minb * 2 : 8192;
            if (bytes < 8192) bytes = 8192;
            jobject r = nullptr; int usedSrc = 0;
            for (int src : {7, 1}) {                                              // 7 = VOICE_COMMUNICATION (эхо/шумоподавление), 1 = MIC
                jobject c = e->NewObject(cAR, mNew, src, VOICE_CAP_RATE, 16, 2, bytes);   // 16 кГц, моно, PCM16
                if (c && !jex(e, "new AudioRecord")) {
                    if (e->CallIntMethod(c, mState) == 1) { r = c; usedSrc = src; break; }
                    e->CallVoidMethod(c, mRel); jex(e, "release");
                } else jex(e, "new AudioRecord");
            }
            if (!r) { voice_dbg("микрофон: ошибка создания"); usleep(1000000); continue; }
            rec = e->NewGlobalRef(r);
            blog("микрофон: AudioRecord создан (источник %d, буфер %d)", usedSrc, bytes);
        }
        if (!recording) {
            e->CallVoidMethod(rec, mStart);
            if (jex(e, "startRecording")) { voice_dbg("микрофон: не удалось начать запись"); usleep(500000); continue; }
            recording = true; acc.clear(); encFresh = true; prev[0] = prev[1] = prev[2] = 0;
            voice_dbg("микрофон: идёт передача");
        }
        jint n = e->CallIntMethod(rec, mRead, arr, 0, CAP - (jint)acc.size());
        if (jex(e, "read") || n <= 0) { usleep(10000); continue; }
        jshort buf[1600];
        e->GetShortArrayRegion(arr, 0, n, buf);
        acc.insert(acc.end(), buf, buf + n);
        if ((int)acc.size() >= CAP) emit();
    }
    return nullptr;
}

struct Speaker { int id; pthread_mutex_t mu; std::deque<std::vector<int16_t>> q; double last; bool alive; double failUntil; int lastSeq; float pos[3]; bool posOk; int rx, lost, dups; };
static pthread_mutex_t g_spkMu = PTHREAD_MUTEX_INITIALIZER;
static std::map<int, Speaker*> g_spk;
static void* speaker_thread(void* arg) {
    Speaker* sp = (Speaker*)arg;
    JNIEnv* e = jni_attach();
    auto fail = [&](const char* why) { blog("голос: динамик %d: %s", sp->id, why); pthread_mutex_lock(&sp->mu); sp->alive = false; sp->q.clear(); sp->failUntil = now_s() + 5.0; pthread_mutex_unlock(&sp->mu); };
    if (!e) { fail("нет Java"); return nullptr; }
    jclass cAT = e->FindClass("android/media/AudioTrack");
    jmethodID mMin = e->GetStaticMethodID(cAT, "getMinBufferSize", "(III)I");
    jmethodID mNew = e->GetMethodID(cAT, "<init>", "(IIIIII)V");
    jmethodID mPlay = e->GetMethodID(cAT, "play", "()V");
    jmethodID mWrite = e->GetMethodID(cAT, "write", "([SII)I");
    jmethodID mStop = e->GetMethodID(cAT, "stop", "()V");
    jmethodID mRel = e->GetMethodID(cAT, "release", "()V");
    if (jex(e, "AudioTrack методы")) { fail("нет AudioTrack"); return nullptr; }
    jint minb = e->CallStaticIntMethod(cAT, mMin, VOICE_RATE, 4, 2);       // 8 кГц, моно, PCM16
    int bytes = minb > 0 ? minb : 2048;
    if (bytes < 3840) bytes = 3840;                                        // ~240 мс: запас от дрожания сети, но без большой задержки
    jobject tr = e->NewObject(cAT, mNew, 3, VOICE_RATE, 4, 2, bytes, 1);   // STREAM_MUSIC, MODE_STREAM
    if (!tr || jex(e, "new AudioTrack")) { fail("не создан AudioTrack"); return nullptr; }
    e->CallVoidMethod(tr, mPlay);
    jex(e, "play");
    jshortArray arr = e->NewShortArray(VOICE_FRAME * 2);
    jshortArray sil = e->NewShortArray(160);                               // 20 мс тишины (массив создаётся нулевым)
    for (;;) {
        pthread_mutex_lock(&sp->mu);
        if (sp->q.empty()) {
            if (now_s() - sp->last > 4.0) { sp->alive = false; pthread_mutex_unlock(&sp->mu); break; }
            pthread_mutex_unlock(&sp->mu);
            // пока пакетов нет - подкладываем тишину: иначе при провале сети некоторые устройства зацикливают
            // последний кусок буфера (эхо/повторы). Запись блокирует поток, так что это не нагружает процессор.
            jint w = e->CallIntMethod(tr, mWrite, sil, 0, 160);
            bool ex = jex(e, "write");
            if (ex || w < 0) { blog("голос: динамик %d: ошибка записи (%d), пересоздаю", sp->id, (int)w); pthread_mutex_lock(&sp->mu); sp->alive = false; pthread_mutex_unlock(&sp->mu); break; }
            if (w == 0) usleep(10000);
            continue;
        }
        std::vector<int16_t> fr = std::move(sp->q.front());
        sp->q.pop_front();
        pthread_mutex_unlock(&sp->mu);
        int n = (int)fr.size();
        if (n > VOICE_FRAME * 2) n = VOICE_FRAME * 2;
        e->SetShortArrayRegion(arr, 0, n, (const jshort*)fr.data());
        jint w = e->CallIntMethod(tr, mWrite, arr, 0, n);
        bool ex = jex(e, "write");
        if (ex || w < 0) { blog("голос: динамик %d: ошибка записи (%d), пересоздаю", sp->id, (int)w); pthread_mutex_lock(&sp->mu); sp->alive = false; sp->q.clear(); pthread_mutex_unlock(&sp->mu); break; }
    }
    e->CallVoidMethod(tr, mStop); jex(e, "stop");
    e->CallVoidMethod(tr, mRel); jex(e, "release");
    return nullptr;
}
static void push_speaker(int sender, int seq, const int16_t* pcm, int n, const float* pos, bool posOk) {
    pthread_mutex_lock(&g_spkMu);
    Speaker* sp;
    auto it = g_spk.find(sender);
    if (it == g_spk.end()) {
        sp = new Speaker(); sp->id = sender; pthread_mutex_init(&sp->mu, nullptr); sp->alive = false; sp->last = 0; sp->failUntil = 0; sp->lastSeq = -1; sp->posOk = false; sp->rx = sp->lost = sp->dups = 0;
        g_spk[sender] = sp;
    } else sp = it->second;
    pthread_mutex_unlock(&g_spkMu);
    double t = now_s();
    pthread_mutex_lock(&sp->mu);
    if (!sp->alive && t < sp->failUntil) { pthread_mutex_unlock(&sp->mu); return; }   // динамик недавно не создался - не пробуем каждый пакет
    if (sp->lastSeq >= 0 && t - sp->last < 1.0) {                          // повторный или пришедший с опозданием пакет - пропускаем (иначе слышны повторы)
        int dlt = (int8_t)(uint8_t)(seq - sp->lastSeq);
        if (dlt <= 0) { sp->dups++; pthread_mutex_unlock(&sp->mu); return; }
        if (dlt > 1) sp->lost += dlt - 1;                                  // пропущенные по дороге пакеты
    }
    sp->rx++;
    sp->lastSeq = seq;
    sp->posOk = posOk; if (posOk) { sp->pos[0] = pos[0]; sp->pos[1] = pos[1]; sp->pos[2] = pos[2]; }
    while (sp->q.size() >= 5) sp->q.pop_front();                           // держим задержку небольшой
    sp->q.emplace_back(pcm, pcm + n);
    sp->last = t;
    bool start = !sp->alive;
    if (start) sp->alive = true;
    pthread_mutex_unlock(&sp->mu);
    if (start) { pthread_t th; pthread_create(&th, nullptr, speaker_thread, sp); pthread_detach(th); }
}

// значок "МИК )))" над говорящим игроком (позиция берётся из его голосовых пакетов); если игрок не в кадре - список сверху по центру
static void voice_draw_speakers(float sw, float sh, float sc, float vw) {
    double t = now_s();
    void* cam = (mi_CamMain && mi_W2S) ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    int waves = 1 + (int)(t * 4.0) % 3;                                      // волны бегут 1-2-3, пока человек говорит
    std::string dots((size_t)waves, ')');
    bool ic = icons_ready();
    int row = 0;
    pthread_mutex_lock(&g_spkMu);
    for (auto& kv : g_spk) {
        Speaker* sp = kv.second;
        pthread_mutex_lock(&sp->mu);
        bool talking = sp->last > 0 && t - sp->last < 0.6;
        bool posOk = sp->posOk;
        float p[3] = {sp->pos[0], sp->pos[1], sp->pos[2]};
        pthread_mutex_unlock(&sp->mu);
        if (!talking) continue;
        bool shown = false;
        if (cam && posOk) {
            V3 w = {p[0], p[1] + VOICE_ICON_UP, p[2]};
            V3 s = ((fn_w2s)fptr(mi_W2S))(cam, w, mi_W2S);
            if (s.z > 0.f && s.x > 0.f && s.x < sw && s.y > 0.f && s.y < sh) {   // перед камерой и в пределах экрана
                float vx = s.x / sc, vy = (sh - s.y) / sc;                        // Unity: начало снизу, IMGUI: сверху
                if (ic) { Box({vx - 25, vy - 19, 50, 38}, ""); speaker_icon(vx - 17.f, vy - 15.f, 34.f, waves); }
                else Box({vx - 40, vy - 13, 80, 26}, "МИК " + dots);
                shown = true;
            }
        }
        if (!shown) {
            float ry = 64.f + row * 36.f;
            if (ic) { Box({vw * 0.5f - 80, ry, 160, 32}, ""); Lbl({vw * 0.5f - 70, ry + 5, 90, 22}, "Игрок " + std::to_string(kv.first)); speaker_icon(vw * 0.5f + 32.f, ry + 4.f, 30.f, waves); }
            else Box({vw * 0.5f - 70, ry, 140, 26}, "Игрок " + std::to_string(kv.first) + " " + dots);
            row++;
        }
    }
    pthread_mutex_unlock(&g_spkMu);
}

// по строке на каждого игрока, от которого идут голосовые пакеты: сколько принято/потеряно и как давно был последний
static void voice_draw_stats(float x, float y) {
    double t = now_s(); int row = 0;
    pthread_mutex_lock(&g_spkMu);
    for (auto& kv : g_spk) {
        if (row >= 5) break;
        Speaker* sp = kv.second;
        pthread_mutex_lock(&sp->mu);
        char b[200]; snprintf(b, sizeof b, "от %d: принято %d, потеряно %d, повторов %d, %.1f с назад%s", kv.first, sp->rx, sp->lost, sp->dups, sp->last > 0 ? t - sp->last : -1.0, sp->alive ? "" : " (динамик выкл)");
        pthread_mutex_unlock(&sp->mu);
        Lbl({x, y + row * 20.f, 520, 22}, std::string(b));
        row++;
    }
    pthread_mutex_unlock(&g_spkMu);
}

static void voice_receive(void* ed) {
    if (!mi_EDData || !mi_EDSender) return;
    void* data = ((fn_getPhoton)fptr(mi_EDData))(ed, mi_EDData);
    if (!data) { voice_rx("голос: пустые данные"); return; }
    const char* cn = il2cpp_class_get_name(il2cpp_object_get_class(data));
    if (!cn || strcmp(cn, "Byte[]")) { voice_rx("голос: данные не Byte[] (%s)", cn ? cn : "?"); return; }
    size_t len = *(size_t*)((char*)data + 3 * sizeof(void*));               // Il2CppArray: max_length
    const uint8_t* b = (const uint8_t*)data + 4 * sizeof(void*);            // данные
    if (len < 17 + 2 || len > 2000 || b[0] != 0x56) { voice_rx("голос: плохой пакет len=%d b0=%d", (int)len, (int)b[0]); return; }
    int sender = ((fn_geti)fptr(mi_EDSender))(ed, mi_EDSender);
    float p[3]; memcpy(p, b + 2, 12);
    bool known = g_camOk && !isnan(p[0]) && !isnan(p[1]) && !isnan(p[2]);
    float d = -1.f;
    if (known) { float dx = p[0] - g_cam[0], dy = p[1] - g_cam[1], dz = p[2] - g_cam[2]; d = sqrtf(dx * dx + dy * dy + dz * dz); }
    g_voiceRxN++;
    static int logs = 0;
    if (logs++ < 12) blog("голос: пакет от %d, %d байт, расстояние %.1f", sender, (int)len, d);
    voice_rx("голос: принято %d (от %d, дистанция %.0f)", g_voiceRxN, sender, d);
    if (VOICE_PROXIMITY && known && d >= VOICE_RADIUS) return;              // слишком далеко (только в режиме "рядом")
    int n = (int)(len - 14 - 3) * 2;
    if (n <= 0 || n > VOICE_FRAME * 2) return;
    int16_t pcm[VOICE_FRAME * 2];
    adpcm_decode(b + 14, n, pcm);
    float gain = VOICE_GAIN * ((VOICE_PROXIMITY && known) ? (1.f - 0.7f * d / VOICE_RADIUS) : 1.f);   // в режиме "рядом" вдалеке тише
    for (int i = 0; i < n; i++) pcm[i] = (int16_t)clamp16((int)(pcm[i] * gain));
    push_speaker(sender, (int)b[1], pcm, n, p, !isnan(p[0]) && !isnan(p[1]) && !isnan(p[2]));
}
static void (*orig_PunOnEvent)(void*, void*, void*);
static void hook_PunOnEvent(void* self, void* ed, void* mi) {
    if (ed && off_EDCode >= 0) { g_evAll++; g_evLast = *(uint8_t*)((char*)ed + off_EDCode); }
    if (ed && off_EDCode >= 0 && *(uint8_t*)((char*)ed + off_EDCode) == VOICE_EVENT) { voice_receive(ed); return; }
    if (ed && off_EDCode >= 0 && *(uint8_t*)((char*)ed + off_EDCode) == WEATHER_EVENT) { weather_receive(ed); return; }
    orig_PunOnEvent(self, ed, mi);
}
static void voice_send(const std::vector<uint8_t>& pk) {
    static bool reoInit = false;
    if (!reoInit && kREOcls) { il2cpp_runtime_class_init(kREOcls); reoInit = true; }
    void* opts = nullptr;
    il2cpp_field_static_get_value(g_reoField, &opts);                       // RaiseEventOptions.Default (получатели: остальные)
    if (!opts) return;
    void* arr = il2cpp_array_new(kByteCls, (uintptr_t)pk.size());
    if (!arr) return;
    memcpy((char*)arr + 4 * sizeof(void*), pk.data(), pk.size());
    SendOpts so = {0, 0, 0, 0};                                             // Unreliable
    bool ok = ((fn_raise)fptr(mi_RaiseEvent))(VOICE_EVENT, arr, opts, so, mi_RaiseEvent);
    g_voiceTx++;
    static int logs = 0;
    if (logs++ < 6) blog("голос: отправлено %d байт, ok=%d", (int)pk.size(), ok ? 1 : 0);
    if (g_voiceTx % 10 == 0) voice_dbg("передача: отправлено %d (ok=%d)", g_voiceTx, ok ? 1 : 0);
}
// вызывается каждый кадр в матче (поток Unity): позиция камеры и отправка накопленных пакетов
static void voice_pump() {
    if (!g_voiceOk) return;
    double t = now_s();
    if (t - g_camT > 0.05) {
        g_camT = t;
        void* cam = mi_CamMain ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
        void* tr = (cam && mi_CompTransform) ? ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform) : nullptr;
        if (tr && mi_TrPos) { V3 pos = ((fn_getV3)fptr(mi_TrPos))(tr, mi_TrPos); g_cam[0] = pos.x; g_cam[1] = pos.y; g_cam[2] = pos.z; g_camOk = true; }
    }
    std::vector<std::vector<uint8_t>> out;
    pthread_mutex_lock(&g_outMu);
    out.swap(g_outQ);
    pthread_mutex_unlock(&g_outMu);
    for (auto& pk : out) voice_send(pk);
}
static bool g_voiceStarted = false;
static void start_voice() {
    if (g_voiceStarted || !g_voiceOk) return;
    g_voiceStarted = true;
    pthread_t t;
    pthread_create(&t, nullptr, capture_thread, nullptr);
    pthread_detach(t);
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
    R(il2cpp_object_new) R(il2cpp_value_box) R(il2cpp_object_unbox) R(il2cpp_object_get_class) R(il2cpp_class_get_name) R(il2cpp_class_get_type) R(il2cpp_type_get_object) R(il2cpp_gchandle_new) R(il2cpp_gchandle_free) R(il2cpp_array_new) R(il2cpp_field_static_get_value) R(il2cpp_runtime_class_init)
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
    {   // --- голосовой чат: события Photon, камера, касания ---
        void* kPNv = find_class("PhotonUnityNetworking", "Photon.Pun", "PhotonNetwork");
        void* kREO = find_class("PhotonRealtime", "Photon.Realtime", "RaiseEventOptions");
        void* kED = find_class("Photon3Unity3D", "ExitGames.Client.Photon", "EventData");
        void* kPunCB = find_class("UnityUtils", "Scripts.Pun", "PunCallbacksService");   // класс лежит в UnityUtils.dll
        if (!kPunCB) kPunCB = find_class("Assembly-CSharp", "Scripts.Pun", "PunCallbacksService");
        void* kCam = find_class("UnityEngine.CoreModule", "UnityEngine", "Camera");
        void* kTr = find_class("UnityEngine.CoreModule", "UnityEngine", "Transform");
        void* kIn = find_class("UnityEngine.InputLegacyModule", "UnityEngine", "Input");
        if (!kIn) kIn = find_class("UnityEngine.InputModule", "UnityEngine", "Input");
        kByteCls = find_class("mscorlib", "System", "Byte");
        mi_RaiseEvent = find_method(kPNv, "RaiseEvent", 4);
        mi_PNTime = find_method(kPNv, "get_Time", 0);
        mi_PNRoom = find_method(kPNv, "get_CurrentRoom", 0); mi_PNIsMaster = find_method(kPNv, "get_IsMasterClient", 0); mi_PNMaster = find_method(kPNv, "get_MasterClient", 0);
        mi_PlActor = find_method(find_class("PhotonRealtime", "Photon.Realtime", "Player"), "get_ActorNumber", 0);
        { void* kEv = find_class("UnityEngine.IMGUIModule", "UnityEngine", "Event"); mi_EvCurrent = find_method(kEv, "get_current", 0); mi_EvType = find_method(kEv, "get_type", 0); }
        blog("погода: isMaster=%p master=%p actor=%p evCurrent=%p evType=%p", mi_PNIsMaster, mi_PNMaster, mi_PlActor, mi_EvCurrent, mi_EvType);
        kREOcls = kREO;
        g_reoField = kREO ? il2cpp_class_get_field_from_name(kREO, "Default") : nullptr;
        off_EDCode = field_off(kED, "Code");
        mi_EDData = find_method(kED, "get_CustomData", 0);
        mi_EDSender = find_method(kED, "get_Sender", 0);
        mi_PunOnEvent = find_method(kPunCB, "OnEvent", 1);
        mi_CamMain = find_method(kCam, "get_main", 0);
        mi_W2S = find_method(kCam, "WorldToScreenPoint", 1);
        mi_TrPos = find_method(kTr, "get_position", 0);
        mi_CompTransform = find_method(kComp, "get_transform", 0);
        mi_TouchCount = find_method(kIn, "get_touchCount", 0);
        mi_GetTouch = find_method(kIn, "GetTouch", 1);
        mi_MouseBtn = find_method(kIn, "GetMouseButton", 1);
        mi_MousePos = find_method(kIn, "get_mousePosition", 0);
        bool input = (mi_TouchCount && mi_GetTouch) || (mi_MouseBtn && mi_MousePos);
        g_voiceOk = mi_RaiseEvent && g_reoField && kByteCls && mi_CamMain && mi_TrPos && mi_CompTransform && input && g_jvmReady();
        g_voiceRecv = mi_PunOnEvent && off_EDCode >= 0 && mi_EDData && mi_EDSender;
        blog("голос: отправка=%d приём=%d [raise=%p reoDefault=%p byte=%p cam=%p pos=%p transform=%p touch=%p/%p mouse=%p/%p] [onEvent=%p edCode=%d data=%p sender=%p]",
             g_voiceOk ? 1 : 0, g_voiceRecv ? 1 : 0, mi_RaiseEvent, g_reoField, kByteCls, mi_CamMain, mi_TrPos, mi_CompTransform,
             mi_TouchCount, mi_GetTouch, mi_MouseBtn, mi_MousePos, mi_PunOnEvent, off_EDCode, mi_EDData, mi_EDSender);
        snprintf(g_recvInfo, sizeof g_recvInfo, "onEvent=%d code=%d data=%d sender=%d", mi_PunOnEvent ? 1 : 0, off_EDCode >= 0 ? 1 : 0, mi_EDData ? 1 : 0, mi_EDSender ? 1 : 0);
        if (!g_voiceOk) voice_dbg("голос: недоступен (не найдены функции)");
    }
    {   // --- цифры урона: пакет попадания и контроллер попаданий ---
        void* kHP = find_class("Assembly-CSharp", "", "HitPacket");
        void* kHC = find_class("Assembly-CSharp", "", "HitsController");
        mi_Hit[0] = find_method(kHC, "MakeHit", 1); mi_Hit[1] = find_method(kHC, "MakeHitRaw", 1); mi_Hit[2] = find_method(kHC, "OnHitColliderHit", 1);
        off_HPDmg = field_off(kHP, "Damage"); off_HPHit = field_off(kHP, "HitPoint"); off_HPHead = field_off(kHP, "IsHeadShot");
        off_HPDeath = field_off(kHP, "Death"); off_HPLocal = field_off(kHP, "IsLocal"); off_HCLocal = field_off(kHC, "IsLocal");
        bool isStruct = false;
        { int (*isVT)(void*) = nullptr; *(void**)&isVT = dlsym(h, "il2cpp_class_is_valuetype"); if (isVT && kHP) isStruct = isVT(kHP) != 0; }
        blog("урон: HitPacket=%p HitsController=%p MakeHit=%p смещения dmg=%d hit=%d head=%d death=%d local=%d ctrlLocal=%d struct=%d",
             kHP, kHC, mi_Hit[0], off_HPDmg, off_HPHit, off_HPHead, off_HPDeath, off_HPLocal, off_HCLocal, isStruct ? 1 : 0);
        blog("урон: MakeHitRaw=%p OnHitColliderHit=%p", mi_Hit[1], mi_Hit[2]);
        if (isStruct) mi_Hit[0] = mi_Hit[1] = mi_Hit[2] = nullptr;                    // структуру по значению не разбираем
    }
    {   // --- дым после взрывов ---
        void* kEx = find_class("Assembly-CSharp", "", "Exploder");
        void* kVE = find_class("Assembly-CSharp", "", "vp_Explosion");
        void* kPh = find_class("UnityEngine.PhysicsModule", "UnityEngine", "Physics");
        void* kCm = find_class("UnityEngine.CoreModule", "UnityEngine", "Camera");
        mi_Smoke[0] = find_method(kEx, "MakeExplosion", 0); mi_Smoke[1] = find_method(kVE, "DoExplode", 0);
        off_ExRadius = field_off(kEx, "Radius"); off_VERadius = field_off(kVE, "Radius");
        mi_Linecast = find_method(kPh, "Linecast", 2);
        mi_FOV = find_method(kCm, "get_fieldOfView", 0);
        {   // методы для настоящего дыма (квадраты в мире)
            void* kGO2 = find_class("UnityEngine.CoreModule", "UnityEngine", "GameObject");
            void* kCo2 = find_class("UnityEngine.CoreModule", "UnityEngine", "Component");
            void* kOb2 = find_class("UnityEngine.CoreModule", "UnityEngine", "Object");
            void* kTr2 = find_class("UnityEngine.CoreModule", "UnityEngine", "Transform");
            void* kSh2 = find_class("UnityEngine.CoreModule", "UnityEngine", "Shader");
            void* kRe2 = find_class("UnityEngine.CoreModule", "UnityEngine", "Renderer");
            g_kMat = find_class("UnityEngine.CoreModule", "UnityEngine", "Material");
            mi_CreatePrim = find_method(kGO2, "CreatePrimitive", 1);
            mi_GetCompS = find_method(kCo2, "GetComponent", 1, 0, "System.String");
            mi_Destroy = find_method(kOb2, "Destroy", 1);
            mi_MatCtor = find_method(g_kMat, ".ctor", 1, 0, "UnityEngine.Shader");
            mi_FindShader = find_method(kSh2, "Find", 1);
            mi_MatTex = find_method(g_kMat, "set_mainTexture", 1);
            mi_MatSetColN = find_method(g_kMat, "SetColor", 2, 0, "System.String");
            mi_RendMat = find_method(kRe2, "set_material", 1);
            mi_RendShadow = find_method(kRe2, "set_shadowCastingMode", 1);
            mi_CamNear = find_method(find_class("UnityEngine.CoreModule", "UnityEngine", "Camera"), "get_nearClipPlane", 0);
            mi_CamFar = find_method(find_class("UnityEngine.CoreModule", "UnityEngine", "Camera"), "get_farClipPlane", 0);
            kColorCls = find_class("UnityEngine.CoreModule", "UnityEngine", "Color");
            mi_TexSetPix6 = find_method(find_class("UnityEngine.CoreModule", "UnityEngine", "Texture2D"), "SetPixels", 6);
            mi_TrSetParent = find_method(kTr2, "SetParent", 2); mi_TrSetLPos = find_method(kTr2, "set_localPosition", 1); mi_TrSetLRot = find_method(kTr2, "set_localRotation", 1);
            mi_TrSetPos = find_method(kTr2, "set_position", 1); mi_TrSetRot = find_method(kTr2, "set_rotation", 1);
            mi_TrGetRot = find_method(kTr2, "get_rotation", 0); mi_TrSetScale = find_method(kTr2, "set_localScale", 1);
            *(void**)&gc_free = dlsym(h, "il2cpp_gchandle_free");
            // мини-карта: коллайдеры, лучи вниз, признак смерти аватара
            kColCls = find_class("UnityEngine.PhysicsModule", "UnityEngine", "Collider");
            g_kSpawnObj = find_class("Assembly-CSharp", "", "SpawnObjectHandler");
            mi_ColBounds = find_method(kColCls, "get_bounds", 0); mi_ColTrigger = find_method(kColCls, "get_isTrigger", 0);
            mi_Ray6 = find_method(kPh, "Raycast", 6, 2, "UnityEngine.RaycastHit&");
            mi_HandIsDead = find_method(g_kSpawnObj, "get_IsDead", 0);
            mi_RHcollider = find_method(find_class("UnityEngine.PhysicsModule", "UnityEngine", "RaycastHit"), "get_collider", 0);
            off_VCHVoxel = field_off(find_class("PixelCombats.VoxelMapsUnity", "PixelCombats.VoxelMapsUnity.Colliders", "VoxelColliderHandler"), "m_Voxel");
            mi_VoxTextures = find_method(find_class("PixelCombats.VoxelMapsUnity", "PixelCombats.VoxelMapsUnity.Voxels", "Voxel"), "get_Textures", 0);
            { void* kTx = find_class("UnityEngine.CoreModule", "UnityEngine", "Texture"); void* kT2 = find_class("UnityEngine.CoreModule", "UnityEngine", "Texture2D");
              mi_TexReadable = find_method(kTx, "get_isReadable", 0); mi_TexW = find_method(kTx, "get_width", 0); mi_TexH = find_method(kTx, "get_height", 0);
              mi_TexGetPixel = find_method(kT2, "GetPixel", 2); }
            blog("цвета блоков: getCollider=%p vchVoxelOff=%d textures=%p readable=%p getPixel=%p", mi_RHcollider, off_VCHVoxel, mi_VoxTextures, mi_TexReadable, mi_TexGetPixel);
            blog("мини-карта: collider=%p spawnHandler=%p bounds=%p trigger=%p ray6=%p isDead=%p", kColCls, g_kSpawnObj, mi_ColBounds, mi_ColTrigger, mi_Ray6, mi_HandIsDead);
            blog("дым3D: prim=%p getComp=%p destroy=%p matCtor=%p find=%p tex=%p setCol=%p rendMat=%p pos=%p rot=%p scale=%p", mi_CreatePrim, mi_GetCompS, mi_Destroy, mi_MatCtor, mi_FindShader, mi_MatTex, mi_MatSetColN, mi_RendMat, mi_TrSetPos, mi_TrSetRot, mi_TrSetScale);
        }
        blog("дым: Exploder=%p vp_Explosion=%p MakeExplosion=%p DoExplode=%p linecast=%p fov=%p", kEx, kVE, mi_Smoke[0], mi_Smoke[1], mi_Linecast, mi_FOV);
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
    if (SMOKE_ENABLED) {
        void* sh2[2] = {(void*)hook_Smoke0, (void*)hook_Smoke1};
        int n = 0;
        for (int i = 0; i < 2; i++) if (mi_Smoke[i]) { install_hook(fptr(mi_Smoke[i]), sh2[i], (void**)&orig_Smoke[i]); n++; }
        blog("дым: установлено хуков взрыва %d", n);
    }
    if (off_HPDmg >= 0 && off_HPHit >= 0 && off_HPLocal >= 0) {
        void* hooks[3] = {(void*)hook_Hit0, (void*)hook_Hit1, (void*)hook_Hit2};
        int n = 0;
        for (int i = 0; i < 3; i++) if (mi_Hit[i]) { install_hook(fptr(mi_Hit[i]), hooks[i], (void**)&orig_Hit[i]); n++; }
        snprintf(g_hitDbg, sizeof g_hitDbg, "урон: хуков=%d (MakeHit=%d Raw=%d Collider=%d), попаданий пока нет", n, mi_Hit[0] ? 1 : 0, mi_Hit[1] ? 1 : 0, mi_Hit[2] ? 1 : 0);
        blog("цифры урона: установлено хуков %d", n);
    } else {
        snprintf(g_hitDbg, sizeof g_hitDbg, "урон: отключено, нет полей (dmg=%d hit=%d local=%d)", off_HPDmg, off_HPHit, off_HPLocal);
        blog("цифры урона: нужные поля не найдены, функция отключена");
    }
    if (g_voiceRecv) { install_hook(fptr(mi_PunOnEvent), (void*)hook_PunOnEvent, (void**)&orig_PunOnEvent); g_hookOn = true; blog("хук приёма голоса установлен"); }
    start_voice();
    blog("хуки установлены");
    start_music();
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
