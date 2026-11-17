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
    bool active = false, placed = false;
    int actor = 0; uint32_t gc = 0;
    void *player = nullptr, *soh = nullptr, *sync = nullptr;
    float hp = BOT_HP, phase = 0.f;
    V3 pos{0, 0, 0};
    double bornT = 0, lastAiT = 0, nextFireT = 0, fxOffT = 0;
};
static BotState g_bot;
static int g_botSeq = 0;
static double g_botMgrT = 0, g_botNextSpawnT = 0;
static bool g_botsOk = false, g_botsDisabled = false, g_botKick = false, g_botDead = false;
static bool g_botGuardArmed = false, g_botGuardCleared = false;
static std::string g_botGuard, g_botDbg = "боты: ожидание";
static void* g_botLastPacket = nullptr;

static void *kBPlayer, *kBHP, *kBSOH, *kBSync, *kBHC, *kBRoomInfo, *kBPunCB;
static void *mi_BPlCtor4, *mi_BPlSetUid, *mi_BPlProps, *mi_BHtStr, *mi_BRoomCount, *mi_BRoomProps;
static void *mi_BSOHGet, *mi_BSOHMakeHit, *mi_BSOHPid, *mi_BSOHDead, *mi_BSyncFire;
static void *mi_BPunEnter, *mi_BPunLeft, *mi_BPunJoined;
static int off_BSync = -1, off_BSyncPos = -1, off_BSyncRot = -1, off_BSyncVel = -1, off_BSyncWpn = -1, off_BHCAvatar = -1;
static int off_BHPPid = -1, off_BHPDmg = -1, off_BHPWpn = -1, off_BHPShoot = -1, off_BHPHit = -1, off_BHPCol = -1, off_BHPLocal = -1;

typedef void (*fn_bplctor)(void*, void*, int, bool, void*, void*);
typedef void (*fn_bvp1)(void*, void*, void*);          // (self, arg, mi)
typedef void (*fn_bvp0)(void*, void*);                 // (self, mi)
typedef void* (*fn_bget)(int, void*);                  // static Get(int, mi)
typedef int (*fn_bi0)(void*, void*);                   // (self, mi) -> int
typedef bool (*fn_bb0)(void*, void*);                  // (self, mi) -> bool
typedef void (*fn_bfire)(void*, void*, void*, uint8_t, void*);

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
// Photon Hashtable.ToString() даёт строки вида "(System.String)ключ=значение(System.String), ..."
static std::string bots_props_str(void* ht) {
    return (ht && mi_BHtStr) ? bstr(((fn_getPhoton)fptr(mi_BHtStr))(ht, mi_BHtStr)) : std::string();
}
static std::vector<std::string> bots_room_teams() {
    std::vector<std::string> out;
    void* room = bots_room();
    if (!room || !mi_BRoomProps) return out;
    std::string s = bots_props_str(((fn_getPhoton)fptr(mi_BRoomProps))(room, mi_BRoomProps));
    size_t p = 0;
    while ((p = s.find("td.", p)) != std::string::npos) {
        bool okStart = p == 0 || s[p - 1] == ')' || s[p - 1] == ' ';
        p += 3;
        if (!okStart) continue;
        size_t e = p;
        while (e < s.size() && s[e] != '.' && s[e] != '=' && s[e] != ',' && s[e] != '(' && s[e] != ' ') e++;
        std::string id = s.substr(p, e - p);
        if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}
static std::string bots_local_team() {
    void* lp = mi_PNLocal ? ((fn_stat0)fptr(mi_PNLocal))(mi_PNLocal) : nullptr;
    if (!lp || !mi_BPlProps) return "";
    std::string s = bots_props_str(((fn_getPhoton)fptr(mi_BPlProps))(lp, mi_BPlProps));
    size_t p = s.find(")team=");
    if (p == std::string::npos) return "";
    p += 6;
    size_t e = s.find('(', p);
    return s.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
static bool bots_local_alive() {
    int a = bots_local_actor();
    if (a < 0 || !mi_BSOHGet) return false;
    void* soh = ((fn_bget)fptr(mi_BSOHGet))(a, mi_BSOHGet);
    if (!soh) return false;
    return !(mi_BSOHDead && ((fn_bb0)fptr(mi_BSOHDead))(soh, mi_BSOHDead));
}

// ---------------------------------------------------------------- создание / удаление
static void bots_guard_arm() {
    if (g_botGuardArmed || g_botGuard.empty()) return;
    FILE* f = fopen(g_botGuard.c_str(), "w"); if (f) { fputs("1", f); fclose(f); }
    g_botGuardArmed = true;
}
static void bot_forget() {                       // состояние сбрасываем, игре ничего не сообщаем (она сама чистит игроков при выходе из комнаты)
    if (g_bot.gc && il2cpp_gchandle_free) il2cpp_gchandle_free(g_bot.gc);
    g_bot = BotState();
}
static void bot_remove() {
    if (g_bot.player && g_punCB && mi_BPunLeft) ((fn_bvp1)fptr(mi_BPunLeft))(g_punCB, g_bot.player, mi_BPunLeft);
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
    void* pl = il2cpp_object_new(kBPlayer);
    char nick[32]; snprintf(nick, sizeof nick, "Bot_%d", g_botSeq);
    ((fn_bplctor)fptr(mi_BPlCtor4))(pl, S(nick), actor, false, ht, mi_BPlCtor4);
    if (mi_BPlSetUid) { char uid[40]; snprintf(uid, sizeof uid, "bot-%d", actor); ((fn_bvp1)fptr(mi_BPlSetUid))(pl, S(uid), mi_BPlSetUid); }
    bots_guard_arm();
    g_bot = BotState();
    g_bot.active = true; g_bot.actor = actor; g_bot.player = pl;
    g_bot.gc = il2cpp_gchandle_new(pl, 0);
    g_bot.bornT = now; g_bot.phase = (float)(g_botSeq % 7);
    blog("бот %d: вход в комнату (команда %s, моя %s)", actor, team.c_str(), mine.c_str());
    ((fn_bvp1)fptr(mi_BPunEnter))(g_punCB, pl, mi_BPunEnter);
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
    ((fn_bvp1)fptr(mi_BSOHMakeHit))(lsoh, pk, mi_BSOHMakeHit);
}
static void bot_fire_fx(bool on) {
    if (!BOT_FIRE_FX || !mi_BSyncFire || !g_bot.sync || !g_bot.soh) return;
    void* ph = nullptr;                                    // PlayerHandler бота (sender и player одинаковы)
    void* getPH = find_method(kBSOH, "get_PlayerHandler", 0);
    if (getPH) ph = ((fn_getPhoton)fptr(getPH))(g_bot.soh, getPH);
    if (!ph) return;
    ((fn_bfire)fptr(mi_BSyncFire))(g_bot.sync, ph, ph, on ? 1 : 0, mi_BSyncFire);
}
static void bot_ai(double now) {
    if (!g_bot.soh) {                                      // ждём, пока игра создаст аватар бота
        g_bot.soh = mi_BSOHGet ? ((fn_bget)fptr(mi_BSOHGet))(g_bot.actor, mi_BSOHGet) : nullptr;
        if (g_bot.soh && off_BSync >= 0) g_bot.sync = *(void**)((char*)g_bot.soh + off_BSync);
        if (!g_bot.soh) {
            if (now - g_bot.bornT > 6.0) { blog("бот %d: игра не создала аватар за 6 с, отказ", g_bot.actor); g_botDbg = "боты: аватар не создан"; bot_remove(); g_botNextSpawnT = now + 15.0; }
            return;
        }
        if (!g_bot.sync) { blog("бот %d: нет PlayerSynchronizer", g_bot.actor); bot_remove(); g_botNextSpawnT = now + 30.0; return; }
        blog("бот %d: аватар готов", g_bot.actor);
    }
    if (now - g_bot.lastAiT < 0.05) return;
    float dt = (float)(now - g_bot.lastAiT); if (dt > 0.2f || g_bot.lastAiT == 0) dt = 0.05f;
    g_bot.lastAiT = now;
    V3 eye; if (!bots_local_eye(&eye)) return;
    V3 feet{eye.x, eye.y - BOT_EYE, eye.z};

    if (!g_bot.placed) {                                   // ищем свободное место с открытой линией до игрока
        for (int k = 0; k < 14; k++) {
            float ang = (float)k * 0.62f + (float)g_botSeq * 1.7f, d = 14.f + (float)(k % 4) * 4.f;
            V3 c{feet.x + cosf(ang) * d, feet.y, feet.z + sinf(ang) * d};
            if (bots_clear(V3{c.x, c.y + BOT_EYE, c.z}, eye)) { g_bot.pos = c; g_bot.placed = true; break; }
        }
        if (!g_bot.placed) return;
        bot_write(g_bot.pos, 0.f, V3{0, 0, 0});
        g_bot.nextFireT = now + 1.5;
        g_botDbg = "боты: бот на карте";
        blog("бот %d: поставлен (%.1f %.1f %.1f)", g_bot.actor, g_bot.pos.x, g_bot.pos.y, g_bot.pos.z);
        return;
    }

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
    int cnt = mi_BRoomCount ? ((fn_bi0)fptr(mi_BRoomCount))(room, mi_BRoomCount) : 99;
    bool inMatch = g_lastHudT > 0 && now - g_lastHudT < 3.0;
    bool alone = cnt <= 1;
    if (g_bot.active && (!alone || !inMatch || g_botKick)) {
        blog("боты: игроков=%d матч=%d -> бот уходит", cnt, inMatch ? 1 : 0);
        bot_remove(); g_botKick = false; g_botNextSpawnT = now + 2.0; return;
    }
    g_botKick = false;
    if (g_bot.active || !alone || !inMatch || !g_punCB || now < g_botNextSpawnT) return;
    if (bots_room_teams().size() < 2) { g_botDbg = "боты: не командный режим"; return; }
    if (!bots_local_alive()) return;
    bot_create(now);
}
static void bots_tick() {
    if (!BOTS_ENABLED || !g_botsOk || g_botsDisabled) return;
    double now = now_s();
    if (g_botDead && g_bot.active) { g_botDead = false; blog("бот %d убит", g_bot.actor); bot_remove(); g_botNextSpawnT = now + BOT_RESPAWN_WAIT; }
    if (now - g_botMgrT > 1.0) { g_botMgrT = now; bots_manage(now); }
    if (g_bot.active) bot_ai(now);
    if (g_botGuardArmed && !g_botGuardCleared && g_bot.active && now - g_bot.bornT > 25.0) { unlink(g_botGuard.c_str()); g_botGuardCleared = true; }
}
// вызывается из хуков попаданий: hc - HitsController, packet - HitPacket
static void bots_on_hit(void* hc, void* packet) {
    if (!g_bot.active || !g_bot.placed || !hc || !packet || off_BHCAvatar < 0 || off_BHPDmg < 0 || !mi_BSOHPid) return;
    if (packet == g_botLastPacket) return;                 // этот же удар уже учтён (хуков на него три)
    void* av = *(void**)((char*)hc + off_BHCAvatar);
    if (!av || ((fn_bi0)fptr(mi_BSOHPid))(av, mi_BSOHPid) != g_bot.actor) return;
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
    if (access(g_botGuard.c_str(), F_OK) == 0) { unlink(g_botGuard.c_str()); g_botsDisabled = true; blog("боты: прошлый запуск с ботами упал, на этот раз боты выключены"); return; }

    kBPlayer   = find_class("PhotonRealtime", "Photon.Realtime", "Player");
    kBRoomInfo = find_class("PhotonRealtime", "Photon.Realtime", "RoomInfo");
    kBPunCB    = find_class("UnityUtils", "Scripts.Pun", "PunCallbacksService");
    if (!kBPunCB) kBPunCB = find_class("Assembly-CSharp", "Scripts.Pun", "PunCallbacksService");
    kBSOH      = find_class("Assembly-CSharp", "", "SpawnObjectHandler");
    kBSync     = find_class("Assembly-CSharp", "", "PlayerSynchronizer");
    kBHC       = find_class("Assembly-CSharp", "", "HitsController");
    kBHP       = find_class("Assembly-CSharp", "", "HitPacket");

    mi_BPlCtor4   = find_method(kBPlayer, ".ctor", 4);
    mi_BPlSetUid  = find_method(kBPlayer, "set_UserId", 1);
    mi_BPlProps   = find_method(kBPlayer, "get_CustomProperties", 0);
    mi_BHtStr     = find_method(kHashtable, "ToString", 0);
    mi_BRoomCount = find_method(kBRoomInfo, "get_PlayerCount", 0);
    mi_BRoomProps = find_method(kBRoomInfo, "get_CustomProperties", 0);
    mi_BSOHGet    = find_method(kBSOH, "Get", 1, 0, "System.Int32");
    mi_BSOHMakeHit= find_method(kBSOH, "MakeHit", 1);
    mi_BSOHPid    = find_method(kBSOH, "get_PlayerId", 0);
    mi_BSOHDead   = find_method(kBSOH, "get_IsDead", 0);
    mi_BSyncFire  = find_method(kBSync, "PlayerSynchroEvents_OnFire", 3);
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

    bool core = kBPlayer && mi_BPlCtor4 && mi_BHtStr && mi_BRoomCount && mi_BRoomProps && mi_BSOHGet && mi_BSOHPid &&
                mi_BPunEnter && mi_BPunLeft && off_BSync >= 0 && off_BSyncPos >= 0 && off_BSyncRot >= 0 && off_BHCAvatar >= 0 && off_BHPDmg >= 0 &&
                kHashtable && mi_HtCtor && mi_HtSet && mi_PNRoom && mi_PNLocal && mi_PlActor && mi_CamMain && mi_CompTransform && mi_TrPos;
    blog("боты: ядро=%d ctor=%d ht=%d cnt=%d props=%d soh=%d pid=%d enter=%d left=%d sync=%d/%d/%d hc=%d dmg=%d makehit=%d fire=%d linecast=%d",
         core, !!mi_BPlCtor4, !!mi_BHtStr, !!mi_BRoomCount, !!mi_BRoomProps, !!mi_BSOHGet, !!mi_BSOHPid, !!mi_BPunEnter, !!mi_BPunLeft,
         off_BSync, off_BSyncPos, off_BSyncRot, off_BHCAvatar, off_BHPDmg, !!mi_BSOHMakeHit, !!mi_BSyncFire, !!mi_Linecast);
    if (!core) { g_botDbg = "боты: не найдены нужные методы игры"; return; }

    if (mi_BPunEnter) install_hook(fptr(mi_BPunEnter), (void*)hook_BEnter, (void**)&orig_BEnter);
    if (mi_BPunLeft)  install_hook(fptr(mi_BPunLeft), (void*)hook_BLeft, (void**)&orig_BLeft);
    if (mi_BPunJoined) install_hook(fptr(mi_BPunJoined), (void*)hook_BJoined, (void**)&orig_BJoined);
    g_botsOk = true;
    g_botDbg = "боты: готовы";
}
