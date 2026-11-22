// ============================================================================
//  bots.h - клиентские боты для режимов с командами (TDM).
//  Бот существует, только пока в комнате один настоящий игрок. Зашёл второй - бот исчезает,
//  остался один - бот снова появляется. Убили бота - через пару секунд появляется новый.
//
//  Как это работает: создаём "поддельного" Photon-игрока и вызываем события входа игрока
//  в комнату (PunCallbacksService.OnPlayerEnteredRoom). Игра сама делает для него PlayerHandler,
//  PlayerApi, команду и модель из префаба. Двигаем бота через поля PlayerSynchronizer
//  (CurrentplayerPosition / CurrentplayerRotation), как будто это сетевой игрок.
//  Урон по боту ловим в хуках попаданий, урон по игроку наносим через SpawnObjectHandler.MakeHit.
//
//  Защита: перед первым появлением бота создаётся файл bots.guard; если игра упала - при
//  следующем запуске боты будут выключены один раз (как guard самого мода).
//  Подключается в main.cpp внутри одной единицы трансляции (использует её статические функции).
// ============================================================================
#pragma once
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

static const bool  BOTS_ENABLED     = true;
static const int   BOT_ACTOR_BASE   = 9000;   // номера Photon-игроков для ботов (настоящие - маленькие)
static const float BOT_HP           = 100.f;
static const float BOT_SPEED        = 4.2f;   // м/с
static const float BOT_RANGE        = 40.f;   // дальше - не стреляет
static const float BOT_FIRE_EVERY   = 0.65f;  // секунд между выстрелами
static const float BOT_HIT_CHANCE   = 0.30f;  // шанс попадания по вам
static const float BOT_DMG_MIN      = 6.f, BOT_DMG_MAX = 12.f;
static const float BOT_RESPAWN_WAIT = 3.f;    // пауза до появления нового бота
static const float BOT_EYE          = 1.6f;   // высота глаз над землёй
static const bool  BOT_FIRE_FX      = true;   // показывать вспышку/анимацию стрельбы (событие OnFire)

struct BotState {
    bool active = false, placed = false, spawned = false;
    int actor = 0; uint32_t gc = 0;
    void *player = nullptr, *soh = nullptr, *sync = nullptr;
    float hp = BOT_HP, phase = 0.f;
    V3 pos{0, 0, 0};
    double bornT = 0, lastAiT = 0, nextFireT = 0, fxOffT = 0, spawnT = 0, resolveT = 0;
    void *ips = nullptr, *sis = nullptr, *sps = nullptr;
    int attempt = 0;
    void *ctx = nullptr, *papi = nullptr;
};
static BotState g_bot;
static int g_botSeq = 0;
static double g_botMgrT = 0, g_botNextSpawnT = 0;
static bool g_botsOk = false, g_botsDisabled = false, g_botKick = false, g_botDead = false;
static bool g_botGuardArmed = false, g_botGuardCleared = false;
static std::string g_botGuard, g_botWhy;
static int g_botFails = 0;
static void* g_botLastPacket = nullptr;

static void *kBPlayer, *kBHP, *kBSOH, *kBSync, *kBHC, *kBRoomInfo, *kBPunCB;
static void *mi_BPlCtor4, *mi_BPlSetUid, *mi_BPlProps, *mi_BRoomCount, *mi_BRoomProps;
static void* kBRoom;
static void *kBPC, *kBDI, *kBSIS, *kBIPS, *kBPS;
static void *mi_BPCInst, *mi_BDITry, *mi_BSISSpawn, *mi_BPSGet, *mi_BPSTryGet;
static int off_BPCDi = -1, off_BSISPlayers = -1, off_BSOHPlayer = -1, off_BPApiSpawnsSvc = -1, off_BPApiSpawnsCtx = -1, off_BSPSPlayers = -1;
static void *kBPApi, *kBSPS, *kBPSC, *mi_BPApiGetSpawns, *mi_BSPSGetCtx;
static std::string g_botDiag;
static void *mi_BSOHGet, *mi_BSOHMakeHit, *mi_BSOHPid, *mi_BSOHDead, *mi_BSyncFire;
static void *mi_BPunEnter, *mi_BPunLeft, *mi_BPunJoined;
static int off_BSync = -1, off_BSyncPos = -1, off_BSyncRot = -1, off_BSyncVel = -1, off_BSyncWpn = -1, off_BHCAvatar = -1;
static int off_BHPPid = -1, off_BHPDmg = -1, off_BHPWpn = -1, off_BHPShoot = -1, off_BHPHit = -1, off_BHPCol = -1, off_BHPLocal = -1;

typedef void (*fn_bplctor)(void*, void*, int, bool, void*, void*);
typedef void (*fn_bvp1)(void*, void*, void*);          // (self, arg, mi)
typedef void (*fn_bvp0)(void*, void*);                 // (self, mi)
typedef void* (*fn_bget)(int, void*);                  // static Get(int, mi)
typedef int (*fn_bi0)(void*, void*);                   // (self, mi) -> int
typedef bool (*fn_bb0)(void*, void*);
typedef uint8_t (*fn_bu80)(void*, void*);
typedef void* (*fn_btry)(void*, void*, void*);          // DiContainer.TryResolve(self, Type, mi)
typedef void (*fn_bspawn)(void*, void*, V3, Quat, void*);   // SpawnInstancesService.Spawn(self, IPlayerApi, Vector3, Quaternion, mi)
typedef void* (*fn_bpsget)(void*, int, void*);          // PlayersService.Get(self, int, mi)                  // (self, mi) -> bool
typedef void (*fn_bfire)(void*, void*, void*, uint8_t, void*);

static void* (*bots_rtinvoke)(void*, void*, void**, void**) = nullptr;     // il2cpp_runtime_invoke: ловит исключения игры
static void (*bots_fmtexc)(void*, char*, int) = nullptr;                    // il2cpp_format_exception
static const char* g_botStageLast = nullptr;

// Записывает название шага в bots.guard. Если игра упадёт, при следующем запуске на экране будет видно, на каком шаге.
static void bots_stage(const char* s) {
    g_botStageLast = s;                                  // дёшево: только запоминаем; файл с деталями запишет обработчик сигнала при вылете
    if (!g_botGuardArmed && !g_botGuard.empty()) {
        FILE* f = fopen(g_botGuard.c_str(), "w"); if (f) { fputs(s, f); fclose(f); }
        g_botGuardArmed = true;
    }
}
// При вылете (SIGSEGV и др.) дописываем в bots.guard адрес сбоя: файл+смещение, чтобы найти функцию в dump.cs по RVA.
static struct sigaction g_botOldSig[3];
static const int BOT_SIGS[3] = {SIGSEGV, SIGBUS, SIGABRT};
static void bots_sig_handler(int sig, siginfo_t* si, void* ctx) {
    if (g_botGuardArmed && !g_botGuard.empty()) {
        char buf[300]; const char* stage = g_botStageLast ? g_botStageLast : "?";
        uintptr_t pc = 0;
#if defined(__arm__)
        if (ctx) pc = ((ucontext_t*)ctx)->uc_mcontext.arm_pc;
#elif defined(__aarch64__)
        if (ctx) pc = ((ucontext_t*)ctx)->uc_mcontext.pc;
#endif
        Dl_info di; memset(&di, 0, sizeof di);
        const char* lib = "?"; uintptr_t off = 0;
        if (pc && dladdr((void*)pc, &di) && di.dli_fname) { const char* sl = strrchr(di.dli_fname, '/'); lib = sl ? sl + 1 : di.dli_fname; off = pc - (uintptr_t)di.dli_fbase; }
        int n = snprintf(buf, sizeof buf, "%s | SIG%d %s+0x%lX adr=%p", stage, sig, lib, (unsigned long)off, si ? si->si_addr : nullptr);
        int fd = open(g_botGuard.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { if (n > 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; } close(fd); }
    }
    for (int i = 0; i < 3; i++) if (BOT_SIGS[i] == sig) {                    // передаём дальше прежнему обработчику (Unity / система)
        struct sigaction* o = &g_botOldSig[i];
        if ((o->sa_flags & SA_SIGINFO) && o->sa_sigaction) { o->sa_sigaction(sig, si, ctx); return; }
        if (o->sa_handler && o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) { o->sa_handler(sig); return; }
    }
    signal(sig, SIG_DFL); raise(sig);
}
static void bots_install_sig() {
    for (int i = 0; i < 3; i++) {
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = bots_sig_handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
        sigaction(BOT_SIGS[i], &sa, &g_botOldSig[i]);
    }
}

// Вызов метода игры с перехватом исключения. ok=false и причина в g_botWhy, если игра бросила исключение.
static void* bots_call(void* mi, void* obj, void** args, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!bots_rtinvoke || !mi) { g_botWhy = "нет il2cpp_runtime_invoke"; return nullptr; }
    void* exc = nullptr;
    void* r = bots_rtinvoke(mi, obj, args, &exc);
    if (exc) {
        char buf[700] = {0};
        if (bots_fmtexc) bots_fmtexc(exc, buf, sizeof buf - 1); else strcpy(buf, "(текст недоступен)");
        std::string m(buf); for (auto& ch : m) if (ch == '\n' || ch == '\r') ch = ' ';
        g_botWhy = "исключение игры: " + m.substr(0, 140);
        blog("бот: %s", m.c_str());
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

static std::string bstr(void* s) { return s ? str_ascii(s) : std::string(); }

// ---------------------------------------------------------------- вспомогательное
static bool bots_local_eye(V3* out) {
    void* cam = mi_CamMain ? ((fn_stat0)fptr(mi_CamMain))(mi_CamMain) : nullptr;
    if (!cam || !mi_CompTransform || !mi_TrPos) return false;
    void* tr = ((fn_getPhoton)fptr(mi_CompTransform))(cam, mi_CompTransform);
    if (!tr) return false;
    *out = ((fn_getV3)fptr(mi_TrPos))(tr, mi_TrPos);
    return true;
}
// true, если между точками нет препятствий (концы чуть укорочены, чтобы не цеплять коллайдеры самих игроков)
static bool bots_clear(V3 a, V3 b) {
    if (!mi_Linecast) return true;
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d < 2.f) return true;
    float k0 = 0.8f / d, k1 = 1.f - 0.8f / d;
    V3 p0{a.x + dx * k0, a.y + dy * k0, a.z + dz * k0}, p1{a.x + dx * k1, a.y + dy * k1, a.z + dz * k1};
    return !((fn_linecast)fptr(mi_Linecast))(p0, p1, mi_Linecast);
}
static void* bots_room() { return mi_PNRoom ? ((fn_stat0)fptr(mi_PNRoom))(mi_PNRoom) : nullptr; }
static int bots_local_actor() {
    void* lp = mi_PNLocal ? ((fn_stat0)fptr(mi_PNLocal))(mi_PNLocal) : nullptr;
    return (lp && mi_PlActor) ? ((fn_geti)fptr(mi_PlActor))(lp, mi_PlActor) : -1;
}
// Чтение Photon Hashtable (это Dictionary<object,object>) прямо из памяти: без вызовов методов игры.
// Раскладка: поле entries - массив записей {int hashCode; int next; object key; object value}.
static void* g_botStrCls = nullptr;
static std::string bots_objstr(void* o) {
    if (!o) return "";
    if (*(void**)o == g_botStrCls) return str_ascii(o);
    return "?";
}
static std::vector<std::pair<std::string, std::string>> bots_ht_pairs(void* ht) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!ht || !kHashtable || !g_botStrCls) return out;
    int offE = field_off(kHashtable, "entries"), offC = field_off(kHashtable, "count");
    if (offE < 0 || offC < 0) return out;
    char* arr = *(char**)((char*)ht + offE);
    int count = *(int*)((char*)ht + offC);
    if (!arr || count <= 0 || count > 4096) return out;
    size_t len = *(size_t*)(arr + 3 * sizeof(void*));
    const size_t dataOff = 4 * sizeof(void*), esz = 8 + 2 * sizeof(void*);
    for (int i = 0; i < count && (size_t)i < len; i++) {
        char* e = arr + dataOff + (size_t)i * esz;
        int next = *(int*)(e + 4);
        void* key = *(void**)(e + 8);
        void* val = *(void**)(e + 8 + sizeof(void*));
        if (!key || next < -1) continue;                    // пустая/удалённая запись
        out.push_back({bots_objstr(key), bots_objstr(val)});
    }
    return out;
}
static std::string bots_clip(const std::string& s, size_t from, size_t n) {
    std::string o;
    for (size_t i = from; i < s.size() && o.size() < n; i++) o += (s[i] >= 32 && s[i] < 127) ? s[i] : '?';
    return o;
}
static std::vector<std::string> bots_room_teams() {
    std::vector<std::string> out;
    void* room = bots_room();
    if (!room || !mi_BRoomProps) return out;
    auto pr = bots_ht_pairs(((fn_getPhoton)fptr(mi_BRoomProps))(room, mi_BRoomProps));
    std::string keys;
    for (auto& kv : pr) {
        keys += kv.first + " ";
        if (kv.first.compare(0, 3, "td.") != 0) continue;
        size_t e = kv.first.find('.', 3);
        std::string id = kv.first.substr(3, e == std::string::npos ? std::string::npos : e - 3);
        if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    g_botDbg2 = "room(" + std::to_string(pr.size()) + "): " + bots_clip(keys, 0, 100);
    g_botDbg3 = bots_clip(keys, 100, 110);
    return out;
}
static std::string bots_local_team() {
    void* lp = mi_PNLocal ? ((fn_stat0)fptr(mi_PNLocal))(mi_PNLocal) : nullptr;
    if (!lp || !mi_BPlProps) return "";
    for (auto& kv : bots_ht_pairs(((fn_getPhoton)fptr(mi_BPlProps))(lp, mi_BPlProps))) if (kv.first == "team") return kv.second;
    return "";
}
static bool bots_local_alive() {
    int a = bots_local_actor();
    if (a < 0 || !mi_BSOHGet) return false;
    void* soh = ((fn_bget)fptr(mi_BSOHGet))(a, mi_BSOHGet);
    if (!soh) return false;
    return !(mi_BSOHDead && ((fn_bb0)fptr(mi_BSOHDead))(soh, mi_BSOHDead));
}

// ---------------------------------------------------------------- создание / удаление
static void bots_guard_disarm() {                    // бот убран штатно: игра не упала, защиту снимаем
    if (g_botGuardArmed && !g_botGuard.empty()) unlink(g_botGuard.c_str());
    g_botGuardArmed = false; g_botStageLast = nullptr;
}
static void bot_fail(double now, const char* why);
// Запасной поиск: все SpawnObjectHandler в сцене (Object.FindObjectsOfType); совпадение по владельцу или по номеру игрока.
static void* bots_find_soh_unity(void* papi, int actor, int* total) {
    static void* kUO = find_class("UnityEngine.CoreModule", "UnityEngine", "Object");
    static void* mFind = kUO ? find_method(kUO, "FindObjectsOfType", 1, 0, "System.Type") : nullptr;
    static void* kPH = find_class("Assembly-CSharp", "", "PlayerHandler");
    static int offHandler = field_off(kBSOH, "m_PlayerHandler"), offPid = field_off(kPH, "<PlayerId>k__BackingField");
    if (total) *total = -1;
    if (!mFind || !kBSOH) return nullptr;
    void* a[1] = {type_obj(kBSOH)};
    void* arr = bots_call(mFind, nullptr, a);
    if (!arr) return nullptr;
    size_t n = *(size_t*)((char*)arr + 3 * sizeof(void*));
    void** data = (void**)((char*)arr + 4 * sizeof(void*));
    if (total) *total = (int)n;
    for (size_t i = 0; i < n && i < 512; i++) {
        char* s = (char*)data[i];
        if (!s) continue;
        if (off_BSOHPlayer >= 0 && papi && *(void**)(s + off_BSOHPlayer) == papi) return s;
        void* ph = offHandler >= 0 ? *(void**)(s + offHandler) : nullptr;
        if (ph && offPid >= 0 && *(int*)((char*)ph + offPid) == actor) return s;
    }
    return nullptr;
}
// Аватар бота ищем в SpawnObjectHandler.Cache по владельцу (поле Player == PlayerApi бота): ключ в кэше может не совпадать с номером Photon.
static void* bots_find_soh_by_player(void* papi, std::string* info) {
    static void* fsg = dlsym(dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD), "il2cpp_field_static_get_value");
    void* fld = kBSOH ? il2cpp_class_get_field_from_name(kBSOH, "Cache") : nullptr;
    if (!fld || !fsg || !papi || off_BSOHPlayer < 0) return nullptr;
    void* dict = nullptr; ((void (*)(void*, void*))fsg)(fld, &dict);
    if (!dict) { if (info) *info = "Cache=null"; return nullptr; }
    void* dk = *(void**)dict;
    int offE = field_off(dk, "entries"), offC = field_off(dk, "count");
    if (offE < 0 || offC < 0) { if (info) *info = "Cache: нет полей"; return nullptr; }
    char* arr = *(char**)((char*)dict + offE); int count = *(int*)((char*)dict + offC);
    if (!arr || count <= 0 || count > 4096) { if (info) *info = "Cache пуст"; return nullptr; }
    size_t len = *(size_t*)(arr + 3 * sizeof(void*));
    const size_t dataOff = 4 * sizeof(void*), esz = 12 + sizeof(void*);        // {hashCode,next,key(int), value(ptr)}
    std::string keys; void* found = nullptr;
    for (int i = 0; i < count && (size_t)i < len; i++) {
        char* e = arr + dataOff + (size_t)i * esz;
        int next = *(int*)(e + 4), key = *(int*)(e + 8); void* val = *(void**)(e + 12);
        if (!val || next < -1) continue;
        keys += std::to_string(key) + " ";
        if (*(void**)((char*)val + off_BSOHPlayer) == papi) found = val;
    }
    if (info) *info = "Cache: " + keys;
    return found;
}
static void bot_forget() {                       // состояние сбрасываем, игре ничего не сообщаем (она сама чистит игроков при выходе из комнаты)
    if (g_bot.gc && il2cpp_gchandle_free) il2cpp_gchandle_free(g_bot.gc);
    g_bot = BotState();
    bots_guard_disarm();
}
static void bot_remove() {
    if (g_bot.ctx && g_bot.spawned) {                                   // сначала убираем аватар бота из мира
        void* md = find_method(*(void**)g_bot.ctx, "Despawn", 0);
        if (md) { bots_stage("remove: PlayerSpawnContext.Despawn"); bots_call(md, g_bot.ctx, nullptr); }
    }
    if (g_bot.player && g_punCB && mi_BPunLeft) { bots_stage("remove: OnPlayerLeftRoom"); void* a[1] = {g_bot.player}; bots_call(mi_BPunLeft, g_punCB, a); }
    blog("бот %d убран", g_bot.actor);
    bot_forget();
}
static bool bot_create(double now) {
    std::string mine = bots_local_team();
    std::vector<std::string> teams = bots_room_teams();
    std::string team;
    for (auto& t : teams) if (t != mine) { team = t; break; }
    if (team.empty()) { g_botDbg = "боты: нет вражеской команды"; return false; }
    int actor = BOT_ACTOR_BASE + (++g_botSeq);
    void* ht = il2cpp_object_new(kHashtable);
    ((fn_ctor)fptr(mi_HtCtor))(ht, mi_HtCtor);
    ((fn_htset)fptr(mi_HtSet))(ht, S("team"), S(team.c_str()), mi_HtSet);
    bots_stage("create: Player.ctor");
    void* pl = il2cpp_object_new(kBPlayer);
    char nick[32]; snprintf(nick, sizeof nick, "Bot_%d", g_botSeq);
    { int act = actor; bool loc = false; void* a[4] = {S(nick), &act, &loc, ht}; bool ok; bots_call(mi_BPlCtor4, pl, a, &ok); if (!ok) { g_botFails++; g_botNextSpawnT = now + 15.0; return false; } }
    if (mi_BPlSetUid) { char uid[40]; snprintf(uid, sizeof uid, "bot-%d", actor); void* a[1] = {S(uid)}; bots_call(mi_BPlSetUid, pl, a); }
    g_bot = BotState();
    g_bot.active = true; g_bot.actor = actor; g_bot.player = pl;
    g_bot.gc = il2cpp_gchandle_new(pl, 0);
    g_bot.bornT = now; g_bot.phase = (float)(g_botSeq % 7);
    blog("бот %d: вход в комнату (команда %s, моя %s)", actor, team.c_str(), mine.c_str());
    bots_stage("create: OnPlayerEnteredRoom");
    { void* a[1] = {pl}; bool ok; bots_call(mi_BPunEnter, g_punCB, a, &ok); if (!ok) { bot_fail(now, g_botWhy.c_str()); return false; } }
    return true;
}

// ---------------------------------------------------------------- движение и стрельба
static void bot_write(V3 p, float yawDeg, V3 vel) {
    if (!g_bot.sync) return;
    char* b = (char*)g_bot.sync;
    if (off_BSyncPos >= 0) memcpy(b + off_BSyncPos, &p, 12);
    if (off_BSyncRot >= 0) { float r[2] = {0.f, yawDeg}; memcpy(b + off_BSyncRot, r, 8); }
    if (off_BSyncVel >= 0) memcpy(b + off_BSyncVel, &vel, 12);
}
static float bots_rnd() { static unsigned s = 987654321u; s = s * 1664525u + 1013904223u; return (float)(s >> 8) / 16777216.f; }

static void bot_shoot_player(V3 botEye, V3 myEye) {
    if (!kBHP || off_BHPDmg < 0 || !mi_BSOHGet || !mi_BSOHMakeHit) return;
    int la = bots_local_actor();
    void* lsoh = la >= 0 ? ((fn_bget)fptr(mi_BSOHGet))(la, mi_BSOHGet) : nullptr;
    if (!lsoh) return;
    void* pk = il2cpp_object_new(kBHP);
    if (!pk) return;
    char* b = (char*)pk;
    float dmg = BOT_DMG_MIN + (BOT_DMG_MAX - BOT_DMG_MIN) * bots_rnd();
    int wpn = off_BSyncWpn >= 0 && g_bot.sync ? *(int*)((char*)g_bot.sync + off_BSyncWpn) : 1;
    if (wpn <= 0 || wpn > 65000) wpn = 1;
    if (off_BHPPid >= 0) *(int*)(b + off_BHPPid) = g_bot.actor;
    *(float*)(b + off_BHPDmg) = dmg;
    if (off_BHPWpn >= 0) *(uint16_t*)(b + off_BHPWpn) = (uint16_t)wpn;
    if (off_BHPShoot >= 0) memcpy(b + off_BHPShoot, &botEye, 12);
    if (off_BHPHit >= 0) memcpy(b + off_BHPHit, &myEye, 12);
    if (off_BHPCol >= 0) b[off_BHPCol] = 0;
    bots_stage("ai: MakeHit по игроку");
    { void* a[1] = {pk}; bool ok; bots_call(mi_BSOHMakeHit, lsoh, a, &ok); if (!ok) g_botDbg += " | урон по игроку: исключение"; }
    bots_stage("ai: бой");
}
static bool g_botFxBroken = false;
static void bot_fire_fx(bool on) {
    if (!BOT_FIRE_FX || g_botFxBroken || !mi_BSyncFire || !g_bot.sync || !g_bot.soh) return;
    void* ph = nullptr;                                    // PlayerHandler бота (sender и player одинаковы)
    void* getPH = find_method(kBSOH, "get_PlayerHandler", 0);
    if (getPH) ph = ((fn_getPhoton)fptr(getPH))(g_bot.soh, getPH);
    if (!ph) return;
    bots_stage("ai: OnFire");
    uint8_t flag = on ? 1 : 0; void* a[3] = {ph, ph, &flag}; bool ok;
    bots_call(mi_BSyncFire, g_bot.sync, a, &ok);
    if (!ok) { g_botFxBroken = true; }                  // вспышку отключаем, остальное работает
    bots_stage("ai: бой");
}
static void* bots_resolve_svc(void* klass) {            // сервис из Zenject-контейнера (TryResolve не бросает исключение, если не найдено)
    void* pc = mi_BPCInst ? ((fn_stat0)fptr(mi_BPCInst))(mi_BPCInst) : nullptr;
    void* di = (pc && off_BPCDi >= 0) ? *(void**)((char*)pc + off_BPCDi) : nullptr;
    if (!di || !klass || !mi_BDITry) return nullptr;
    void* a[1] = {type_obj(klass)};
    return bots_call(mi_BDITry, di, a);
}
static void bot_fail(double now, const char* why) {
    g_botWhy = why; blog("бот %d: %s", g_bot.actor, why);
    bot_remove();
    g_botFails++;
    if (g_botFails >= 3) { g_botsDisabled = true; g_botWhy += " (3 неудачи, боты выключены)"; }
    g_botNextSpawnT = now + 15.0;
}
struct BotSpawnPoint { int x, y, z; float angle; };     // SpawnPoint: Index(3 int) + Angle
static void* bots_overload(void* klass, const char* name, int argc, const char* p0sub) {
    void* it = nullptr; void* m;
    while (klass && (m = il2cpp_class_get_methods(klass, &it))) {
        if (strcmp(il2cpp_method_get_name(m), name) || (int)il2cpp_method_get_param_count(m) != argc) continue;
        const char* tn = il2cpp_type_get_name(il2cpp_method_get_param(m, 0));
        if (tn && strstr(tn, p0sub)) return m;
    }
    return nullptr;
}
static bool bots_spawn_args_default(uint8_t* out8) {     // SpawnArgs.Default (статическое поле структуры), 7 байт bool
    memset(out8, 0, 8);
    static void* fsg = dlsym(dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD), "il2cpp_field_static_get_value");
    void* k = find_class("PixelCombats.Api", "PixelCombats.Api.RoomServer.Services.Spawn", "SpawnArgs");
    void* f = k ? il2cpp_class_get_field_from_name(k, "Default") : nullptr;
    if (!f || !fsg) return false;
    ((void (*)(void*, void*))fsg)(f, out8);
    return true;
}
static void bot_ai(double now) {
    if (now - g_bot.lastAiT < 0.05) return;
    float dt = (float)(now - g_bot.lastAiT); if (dt > 0.2f || g_bot.lastAiT == 0) dt = 0.05f;
    g_bot.lastAiT = now;
    bots_stage("ai: камера");
    V3 eye; if (!bots_local_eye(&eye)) return;
    V3 feet{eye.x, eye.y - BOT_EYE, eye.z};

    if (!g_bot.soh) {
        if (!g_bot.spawned) {                              // шаг 1: берём сервис игроков из цепочки полей, находим PlayerApi бота, просим игру его заспавнить
            if (!g_bot.ips && now - g_bot.resolveT > 0.5) {
                g_bot.resolveT = now;
                bots_stage("ai: цепочка к IPlayersService");
                int la = bots_local_actor();
                void* lsoh = (la >= 0 && mi_BSOHGet) ? ((fn_bget)fptr(mi_BSOHGet))(la, mi_BSOHGet) : nullptr;
                void* lp = lsoh ? *(void**)((char*)lsoh + off_BSOHPlayer) : nullptr;
                void* sps = lp ? *(void**)((char*)lp + off_BPApiSpawnsSvc) : nullptr;
                g_bot.sps = sps;
                g_bot.ips = sps ? *(void**)((char*)sps + off_BSPSPlayers) : nullptr;
                char m[160]; snprintf(m, sizeof m, "нет IPlayersService (мой аватар=%d игрок=%d SpawnsService=%d)", lsoh ? 1 : 0, lp ? 1 : 0, sps ? 1 : 0);
                g_botDiag = m;
                blog("бот: сервисы: %s IPS=%p", m, g_bot.ips);
            }
            if (!g_bot.ips) { if (now - g_bot.bornT > 4.0) bot_fail(now, g_botDiag.c_str()); return; }
            bots_stage("ai: PlayersService.TryGetPlayer");
            int actArg = g_bot.actor; void* papi = nullptr;
            { void* ga[2] = {&actArg, &papi}; bots_call(mi_BPSTryGet, g_bot.ips, ga); }
            bots_stage("ai: после запроса PlayerApi");
            if (!papi) { if (now - g_bot.bornT > 6.0) bot_fail(now, "игра не создала PlayerApi бота"); return; }
            void* ctx = *(void**)((char*)papi + off_BPApiSpawnsCtx);
            if (!ctx && mi_BPApiGetSpawns) { bots_stage("ai: PlayerApi.get_Spawns"); ctx = bots_call(mi_BPApiGetSpawns, papi, nullptr); }
            if (!ctx && g_bot.sps && mi_BSPSGetCtx) { bots_stage("ai: SpawnsService.GetContext"); void* ca[1] = {papi}; ctx = bots_call(mi_BSPSGetCtx, g_bot.sps, ca); }
            bots_stage("ai: контекст спавна получен");
            if (!ctx) { if (now - g_bot.bornT > 6.0) bot_fail(now, "у PlayerApi бота нет контекста спавна"); return; }
            g_bot.ctx = ctx; g_bot.papi = papi;
            void* kctx = *(void**)ctx;
            bool ok = false;
            if (g_bot.attempt == 0) {                          // попытка 1: Spawn() - игра сама выбирает точку
                void* mS = find_method(kctx, "Spawn", 0);
                if (!mS) { bot_fail(now, "у контекста спавна нет метода Spawn()"); return; }
                bots_stage("ai: Spawn()");
                bots_call(mS, ctx, nullptr, &ok);
            } else {                                           // попытки 2-3: Spawn(SpawnPoint[, SpawnArgs]) - своя точка рядом с вами
                V3 c{feet.x + 6.f, feet.y, feet.z}; bool fnd = false;
                for (int k = 0; k < 14 && !fnd; k++) {
                    float ang = (float)k * 0.62f + (float)g_botSeq * 1.7f, d = 10.f + (float)(k % 4) * 4.f;
                    c = V3{feet.x + cosf(ang) * d, feet.y, feet.z + sinf(ang) * d};
                    if (bots_clear(V3{c.x, c.y + BOT_EYE, c.z}, eye)) fnd = true;
                }
                BotSpawnPoint sp{(int)floorf(c.x), (int)floorf(c.y), (int)floorf(c.z), atan2f(feet.x - c.x, feet.z - c.z) * 57.29578f};
                if (g_bot.attempt == 1) {
                    void* mS = bots_overload(kctx, "Spawn", 1, "SpawnPoint");
                    if (!mS) { bot_fail(now, "нет перегрузки Spawn(SpawnPoint)"); return; }
                    bots_stage("ai: Spawn(SpawnPoint)");
                    void* sa[1] = {&sp}; bots_call(mS, ctx, sa, &ok);
                } else {
                    void* mS = bots_overload(kctx, "Spawn", 2, "SpawnPoint");
                    if (!mS) { bot_fail(now, "нет перегрузки Spawn(SpawnPoint, SpawnArgs)"); return; }
                    uint8_t ar[8]; bots_spawn_args_default(ar);
                    bots_stage("ai: Spawn(SpawnPoint, SpawnArgs)");
                    void* sa[2] = {&sp, ar}; bots_call(mS, ctx, sa, &ok);
                }
                blog("бот %d: точка спавна (%d %d %d) попытка %d", g_bot.actor, sp.x, sp.y, sp.z, g_bot.attempt + 1);
            }
            if (!ok) { bot_fail(now, g_botWhy.c_str()); return; }
            g_bot.spawned = true; g_bot.spawnT = now;
            blog("бот %d: спавн вызван (попытка %d)", g_bot.actor, g_bot.attempt + 1);
            return;
        }
        bots_stage("ai: ждём аватар");
        std::string cinfo;
        g_bot.soh = mi_BSOHGet ? ((fn_bget)fptr(mi_BSOHGet))(g_bot.actor, mi_BSOHGet) : nullptr;   // шаг 2: ждём аватар
        if (!g_bot.soh) g_bot.soh = bots_find_soh_by_player(g_bot.papi, &cinfo);
        if (!g_bot.soh) { int tot = 0; g_bot.soh = bots_find_soh_unity(g_bot.papi, g_bot.actor, &tot); cinfo += " | SOH в сцене: " + std::to_string(tot); }
        if (!g_bot.soh) {
            if (now - g_bot.spawnT > 4.0) {
                if (g_bot.attempt < 2) { g_bot.attempt++; g_bot.spawned = false; blog("бот %d: аватара нет (%s), следующая попытка спавна (%d)", g_bot.actor, cinfo.c_str(), g_bot.attempt + 1); }
                else { std::string m = "аватар не найден (3 способа спавна) " + cinfo; bot_fail(now, m.substr(0, 150).c_str()); }
            }
            return;
        }
        if (off_BSync >= 0) g_bot.sync = *(void**)((char*)g_bot.soh + off_BSync);
        if (!g_bot.sync) { bot_fail(now, "у аватара нет PlayerSynchronizer"); return; }
        bots_stage("ai: ставим бота рядом");
        V3 c{0, 0, 0}; bool found = false;                 // переносим бота поближе: свободное место с открытой линией до вас
        for (int k = 0; k < 14 && !found; k++) {
            float ang = (float)k * 0.62f + (float)g_botSeq * 1.7f, d = 14.f + (float)(k % 4) * 4.f;
            c = V3{feet.x + cosf(ang) * d, feet.y, feet.z + sinf(ang) * d};
            if (bots_clear(V3{c.x, c.y + BOT_EYE, c.z}, eye)) found = true;
        }
        if (found) g_bot.pos = c;
        else if (off_BSyncPos >= 0) memcpy(&g_bot.pos, (char*)g_bot.sync + off_BSyncPos, 12);
        bot_write(g_bot.pos, 0.f, V3{0, 0, 0});
        g_bot.placed = true; g_bot.nextFireT = now + 1.5;
        g_botFails = 0; g_botWhy = "";
        g_botDbg = "боты: бот на карте";
        blog("бот %d: аватар готов (%.1f %.1f %.1f) рядом=%d", g_bot.actor, g_bot.pos.x, g_bot.pos.y, g_bot.pos.z, found ? 1 : 0);
        return;
    }

    bots_stage("ai: бой");
    float dx = feet.x - g_bot.pos.x, dz = feet.z - g_bot.pos.z;
    float dist = sqrtf(dx * dx + dz * dz); if (dist < 0.01f) dist = 0.01f;
    float ux = dx / dist, uz = dz / dist;                  // к игроку
    float want = dist > 16.f ? 1.f : (dist < 8.f ? -1.f : 0.f);
    float strafe = sinf((float)now * 0.9f + g_bot.phase);
    float mx = (ux * want + (-uz) * strafe * 0.8f) * BOT_SPEED * dt;
    float mz = (uz * want + ( ux) * strafe * 0.8f) * BOT_SPEED * dt;
    V3 np{g_bot.pos.x + mx, g_bot.pos.y, g_bot.pos.z + mz};
    float dy = feet.y - g_bot.pos.y; if (dy > 2.f) dy = 2.f; if (dy < -2.f) dy = -2.f;
    np.y += dy * 1.2f * dt;                                // высоту плавно подтягиваем к вашей (рельеф не знаем)
    if (bots_clear(V3{g_bot.pos.x, g_bot.pos.y + 0.6f, g_bot.pos.z}, V3{np.x + mx * 3.f, np.y + 0.6f, np.z + mz * 3.f})) g_bot.pos = np;
    else g_bot.phase += 2.1f;                              // упёрлись - меняем сторону
    float yaw = atan2f(dx, dz) * 57.29578f;
    bot_write(g_bot.pos, yaw, V3{mx / dt, 0.f, mz / dt});

    V3 botEye{g_bot.pos.x, g_bot.pos.y + BOT_EYE, g_bot.pos.z};
    if (g_bot.fxOffT > 0 && now >= g_bot.fxOffT) { bot_fire_fx(false); g_bot.fxOffT = 0; }
    if (now >= g_bot.nextFireT && dist <= BOT_RANGE && bots_local_alive() && bots_clear(botEye, eye)) {
        g_bot.nextFireT = now + BOT_FIRE_EVERY * (0.8 + 0.6 * bots_rnd());
        bot_fire_fx(true); g_bot.fxOffT = now + 0.12;
        float chance = BOT_HIT_CHANCE * (1.f - 0.6f * (dist / BOT_RANGE));
        if (bots_rnd() < chance) bot_shoot_player(botEye, eye);
    }
}

// ---------------------------------------------------------------- управление
static void bots_manage(double now) {
    void* room = bots_room();
    if (!room) { if (g_bot.active) { blog("боты: вышли из комнаты"); bot_forget(); } return; }
    int cnt = mi_BRoomCount ? (int)((fn_bu80)fptr(mi_BRoomCount))(room, mi_BRoomCount) : 99;
    bool inMatch = g_lastHudT > 0 && now - g_lastHudT < 3.0;
    bool alone = cnt <= 1;
    {
        std::vector<std::string> tm = bots_room_teams();
        char d[300];
        snprintf(d, sizeof d, "боты: игроков=%d команд=%d моя=%s pun=%d жив=%d | бот=%d аватар=%d стоит=%d hp=%d", cnt, (int)tm.size(),
                 bots_local_team().c_str(), g_punCB ? 1 : 0, bots_local_alive() ? 1 : 0, g_bot.active ? 1 : 0, g_bot.soh ? 1 : 0, g_bot.placed ? 1 : 0, (int)g_bot.hp);
        g_botDbg = d;
        if (!g_botWhy.empty()) g_botDbg += " | " + g_botWhy;
    }
    if (g_bot.active && (!alone || !inMatch || g_botKick)) {
        blog("боты: игроков=%d матч=%d -> бот уходит", cnt, inMatch ? 1 : 0);
        bot_remove(); g_botKick = false; g_botNextSpawnT = now + 2.0; return;
    }
    g_botKick = false;
    if (g_bot.active || !alone || !inMatch || !g_punCB || now < g_botNextSpawnT) return;
    if (bots_room_teams().size() < 2) { g_botDbg += " | НЕТ 2 команд"; return; }
    if (!bots_local_alive()) return;
    bot_create(now);
}
static void bots_tick() {
    if (!BOTS_ENABLED || !g_botsOk || g_botsDisabled) return;
    double now = now_s();
    if (g_botDead && g_bot.active) { g_botDead = false; blog("бот %d убит", g_bot.actor); bot_remove(); g_botNextSpawnT = now + BOT_RESPAWN_WAIT; }
    if (now - g_botMgrT > 1.0) { g_botMgrT = now; if (g_bot.active) bots_stage("tick: manage"); bots_manage(now); }
    if (g_bot.active) bot_ai(now);
    if (g_botGuardArmed && !g_botGuardCleared && g_bot.active && now - g_bot.bornT > 25.0) { unlink(g_botGuard.c_str()); g_botGuardCleared = true; }
}
// вызывается из хуков попаданий: hc - HitsController, packet - HitPacket
static void bots_on_hit(void* hc, void* packet) {
    if (!g_bot.active || !g_bot.placed || !hc || !packet || off_BHCAvatar < 0 || off_BHPDmg < 0 || !mi_BSOHPid) return;
    if (packet == g_botLastPacket) return;                 // этот же удар уже учтён (хуков на него три)
    void* av = *(void**)((char*)hc + off_BHCAvatar);
    if (!av || (av != g_bot.soh && ((fn_bi0)fptr(mi_BSOHPid))(av, mi_BSOHPid) != g_bot.actor)) return;   // аватар бота: тот же объект либо тот же номер
    const char* b = (const char*)packet;
    if (off_BHPLocal >= 0 && !b[off_BHPLocal]) return;     // только мои выстрелы
    float dmg = *(const float*)(b + off_BHPDmg);
    if (!(dmg >= 0.5f && dmg < 100000.f)) return;
    g_botLastPacket = packet;
    g_bot.hp -= dmg;
    if (g_bot.hp <= 0.f) g_botDead = true;
}

// ---------------------------------------------------------------- хуки Pun (нужны, чтобы получить экземпляр PunCallbacksService)
static void (*orig_BEnter)(void*, void*, void*);
static void hook_BEnter(void* self, void* pl, void* mi) {
    g_punCB = self;
    if (pl && g_bot.active && mi_PlActor && ((fn_geti)fptr(mi_PlActor))(pl, mi_PlActor) != g_bot.actor) g_botKick = true;   // зашёл настоящий игрок
    orig_BEnter(self, pl, mi);
}
static void (*orig_BLeft)(void*, void*, void*);
static void hook_BLeft(void* self, void* pl, void* mi) { g_punCB = self; orig_BLeft(self, pl, mi); }
static void (*orig_BJoined)(void*, void*);
static void hook_BJoined(void* self, void* mi) { g_punCB = self; if (g_bot.active) bot_forget(); orig_BJoined(self, mi); }

// ---------------------------------------------------------------- поиск методов (вызывается из resolve_all)
static void bots_resolve() {
    if (!BOTS_ENABLED) return;
    g_botGuard = g_guard; size_t q = g_botGuard.rfind("skinshop.guard");
    if (q != std::string::npos) g_botGuard.replace(q, std::string::npos, "bots.guard");
    {   void* hh = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (hh) { *(void**)&bots_rtinvoke = dlsym(hh, "il2cpp_runtime_invoke"); *(void**)&bots_fmtexc = dlsym(hh, "il2cpp_format_exception"); }
        blog("боты: runtime_invoke=%d format_exception=%d", bots_rtinvoke != nullptr, bots_fmtexc != nullptr); }
    if (access(g_botGuard.c_str(), F_OK) == 0) {
        char st[260] = {0}; FILE* gf = fopen(g_botGuard.c_str(), "r"); if (gf) { size_t n = fread(st, 1, sizeof st - 1, gf); st[n] = 0; fclose(gf); }
        unlink(g_botGuard.c_str()); g_botsDisabled = true;
        g_botDbg = std::string("боты: выключены (прошлый запуск упал на шаге: ") + st + ")"; blog("%s", g_botDbg.c_str()); return;
    }

    kBPlayer   = find_class("PhotonRealtime", "Photon.Realtime", "Player");
    kBRoomInfo = find_class("PhotonRealtime", "Photon.Realtime", "RoomInfo");
    kBRoom     = find_class("PhotonRealtime", "Photon.Realtime", "Room");
    kBPunCB    = find_class("UnityUtils", "Scripts.Pun", "PunCallbacksService");
    if (!kBPunCB) kBPunCB = find_class("Assembly-CSharp", "Scripts.Pun", "PunCallbacksService");
    kBSOH      = find_class("Assembly-CSharp", "", "SpawnObjectHandler");
    kBSync     = find_class("Assembly-CSharp", "", "PlayerSynchronizer");
    kBHC       = find_class("Assembly-CSharp", "", "HitsController");
    kBHP       = find_class("Assembly-CSharp", "", "HitPacket");

    mi_BPlCtor4   = find_method(kBPlayer, ".ctor", 4);
    mi_BPlSetUid  = find_method(kBPlayer, "set_UserId", 1);
    mi_BPlProps   = find_method(kBPlayer, "get_CustomProperties", 0);
    g_botStrCls   = find_class("mscorlib", "System", "String");
    mi_BRoomCount = find_method(kBRoom, "get_PlayerCount", 0);   // у Room свой PlayerCount (byte); у RoomInfo всегда 0
    mi_BRoomProps = find_method(kBRoomInfo, "get_CustomProperties", 0);
    mi_BSOHGet    = find_method(kBSOH, "Get", 1, 0, "System.Int32");
    mi_BSOHMakeHit= find_method(kBSOH, "MakeHit", 1);
    mi_BSOHPid    = find_method(kBSOH, "get_PlayerId", 0);
    mi_BSOHDead   = find_method(kBSOH, "get_IsDead", 0);
    mi_BSyncFire  = find_method(kBSync, "PlayerSynchroEvents_OnFire", 3);
    kBPC  = find_class("Assembly-CSharp", "", "PlayersController");
    kBDI  = find_class("Zenject", "Zenject", "DiContainer");
    kBSIS = find_class("Assembly-CSharp", "ApiImpl.RoomServer.Services.Spawn.Adapters", "SpawnInstancesService");
    kBIPS = find_class("PixelCombats.Api", "PixelCombats.Api.RoomServer.Services.Players", "IPlayersService");
    kBPS  = find_class("Assembly-CSharp", "ApiImpl.RoomServer.Services.Players", "PlayersService");
    mi_BPCInst   = find_method(kBPC, "get_Instance", 0);
    mi_BDITry    = find_method(kBDI, "TryResolve", 1, 0, "System.Type");
    mi_BSISSpawn = find_method(kBSIS, "Spawn", 3);
    mi_BPSGet    = find_method(kBPS, "Get", 1, 0, "System.Int32");
    mi_BPSTryGet = find_method(kBPS, "TryGetPlayer", 2);
    off_BPCDi    = field_off(kBPC, "DiContainer");
    off_BSISPlayers = field_off(kBSIS, "PlayersService");
    kBPApi = find_class("Assembly-CSharp", "ApiImpl.RoomServer.Services.Players", "PlayerApi");
    kBSPS  = find_class("Assembly-CSharp", "ApiImpl.RoomServer.Services.Spawn", "SpawnsService");
    kBPSC  = find_class("Assembly-CSharp", "ApiImpl.RoomServer.Services.Spawn", "PlayerSpawnContext");
    mi_BPApiGetSpawns = find_method(kBPApi, "get_Spawns", 0);
    if (kBSPS) {                                           // GetContext(IPlayerApi): среди перегрузок берём ту, где параметр - IPlayerApi
        void* it = nullptr; void* m;
        while ((m = il2cpp_class_get_methods(kBSPS, &it))) {
            if (strcmp(il2cpp_method_get_name(m), "GetContext") || il2cpp_method_get_param_count(m) != 1) continue;
            const char* tn = il2cpp_type_get_name(il2cpp_method_get_param(m, 0));
            if (tn && strstr(tn, "IPlayerApi")) { mi_BSPSGetCtx = m; break; }
        }
    }
    off_BSOHPlayer     = field_off(kBSOH, "<Player>k__BackingField");
    off_BPApiSpawnsSvc = field_off(kBPApi, "SpawnsService");
    off_BPApiSpawnsCtx = field_off(kBPApi, "m_Spawns");
    off_BSPSPlayers    = field_off(kBSPS, "PlayersService");
    mi_BPunEnter  = find_method(kBPunCB, "OnPlayerEnteredRoom", 1);
    mi_BPunLeft   = find_method(kBPunCB, "OnPlayerLeftRoom", 1);
    mi_BPunJoined = find_method(kBPunCB, "OnJoinedRoom", 0);

    off_BSync = field_off(kBSOH, "Synchronizer");
    off_BSyncPos = field_off(kBSync, "CurrentplayerPosition");
    off_BSyncRot = field_off(kBSync, "CurrentplayerRotation");
    off_BSyncVel = field_off(kBSync, "Velocity");
    off_BSyncWpn = field_off(kBSync, "m_DefaultWeaponId");
    off_BHCAvatar = field_off(kBHC, "Avatar");
    off_BHPPid = field_off(kBHP, "PlayerID");   off_BHPDmg = field_off(kBHP, "Damage");
    off_BHPWpn = field_off(kBHP, "WeaponID");   off_BHPShoot = field_off(kBHP, "ShootPoint");
    off_BHPHit = field_off(kBHP, "HitPoint");   off_BHPCol = field_off(kBHP, "ColliderID");
    off_BHPLocal = field_off(kBHP, "IsLocal");

    bool core = kBPlayer && mi_BPlCtor4 && g_botStrCls && mi_BRoomCount && mi_BRoomProps && mi_BSOHGet && mi_BSOHPid &&
                mi_BPunEnter && mi_BPunLeft && mi_BPSTryGet && off_BSOHPlayer >= 0 && off_BPApiSpawnsSvc >= 0 && off_BPApiSpawnsCtx >= 0 && off_BSPSPlayers >= 0 && off_BSync >= 0 && off_BSyncPos >= 0 && off_BSyncRot >= 0 && off_BHCAvatar >= 0 && off_BHPDmg >= 0 &&
                kHashtable && mi_HtCtor && mi_HtSet && mi_PNRoom && mi_PNLocal && mi_PlActor && mi_CamMain && mi_CompTransform && mi_TrPos;
    blog("боты: ядро=%d ctor=%d ht=%d cnt=%d props=%d soh=%d pid=%d enter=%d left=%d sync=%d/%d/%d hc=%d dmg=%d makehit=%d fire=%d linecast=%d",
         core, !!mi_BPlCtor4, !!g_botStrCls, !!mi_BRoomCount, !!mi_BRoomProps, !!mi_BSOHGet, !!mi_BSOHPid, !!mi_BPunEnter, !!mi_BPunLeft,
         off_BSync, off_BSyncPos, off_BSyncRot, off_BHCAvatar, off_BHPDmg, !!mi_BSOHMakeHit, !!mi_BSyncFire, !!mi_Linecast);
    if (!core) {
        std::string m = "боты: НЕ НАЙДЕНО:";
        auto chk = [&](bool ok, const char* n) { if (!ok) { m += " "; m += n; } };
        chk(kBPlayer, "Player"); chk(mi_BPlCtor4, "Player.ctor4"); chk(g_botStrCls, "String"); chk(mi_BRoomCount, "Room.PlayerCount");
        chk(mi_BRoomProps, "Room.Props"); chk(mi_BSOHGet, "SOH.Get"); chk(mi_BSOHPid, "SOH.PlayerId"); chk(mi_BPunEnter, "Pun.Enter"); chk(mi_BPunLeft, "Pun.Left");
        chk(mi_BPSTryGet, "PS.TryGetPlayer"); chk(kBPS, "PlayersService"); chk(off_BSOHPlayer >= 0, "off.SOH.Player"); chk(off_BPApiSpawnsSvc >= 0, "off.PlayerApi.SpawnsService"); chk(off_BPApiSpawnsCtx >= 0, "off.PlayerApi.m_Spawns"); chk(off_BSPSPlayers >= 0, "off.SpawnsService.PlayersService"); chk(off_BSync >= 0, "off.Synchronizer"); chk(off_BSyncPos >= 0, "off.Pos"); chk(off_BSyncRot >= 0, "off.Rot"); chk(off_BHCAvatar >= 0, "off.HCAvatar"); chk(off_BHPDmg >= 0, "off.Dmg");
        chk(kHashtable, "kHashtable"); chk(mi_HtCtor, "HtCtor"); chk(mi_HtSet, "HtSet"); chk(mi_PNRoom, "PN.Room"); chk(mi_PNLocal, "PN.Local"); chk(mi_PlActor, "Pl.Actor");
        chk(mi_CamMain, "Cam.main"); chk(mi_CompTransform, "Comp.transform"); chk(mi_TrPos, "Tr.pos");
        g_botDbg = m; blog("%s", m.c_str()); return;
    }

    if (mi_BPunEnter) install_hook(fptr(mi_BPunEnter), (void*)hook_BEnter, (void**)&orig_BEnter);
    if (mi_BPunLeft)  install_hook(fptr(mi_BPunLeft), (void*)hook_BLeft, (void**)&orig_BLeft);
    if (mi_BPunJoined) install_hook(fptr(mi_BPunJoined), (void*)hook_BJoined, (void**)&orig_BJoined);
    bots_install_sig();
    g_botsOk = true;
    g_botDbg = "боты: готовы";
}
