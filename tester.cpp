// ============================================================================
//  libtester.so - отдельная библиотека со списком тестеров.
//  Мод (libmod.so) при запуске подгружает её и спрашивает уровень тестера по ID игрока.
//  Если libtester.so нет рядом с libmod.so, список просто не используется.
//
//  Как менять список:
//    1) впишите строки в таблицу LIST ниже:  {"ID игрока PlayFab", уровень},
//    2) пересоберите (в GitHub Actions собирается libtester.so вместе с libmod.so) и положите libtester.so
//       в APK рядом с libmod.so (папка lib/armeabi-v7a/).
//  Без пересборки список можно дополнять текстовым файлом testers.txt в папке
//  Android/data/com.retro.combat.one/files/ - по одной строке: "ID уровень".
//
//  ID игрока - это PlayFab ID: 16 символов из цифр и букв A-F, например 12E8519AB02583C2.
//  Кого нет в списке - у того уровня тестера не будет.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <map>

struct TesterEntry { const char* id; int lvl; };
static const TesterEntry LIST[] = {
    {"DA0C582CF090BD6D", 3},
    // {"ЕЩЁ_ОДИН_ID", 3},
};

static std::map<std::string, int> g_map;
static bool g_builtin = false;

static std::string norm(const std::string& in) {                                  // без пробелов по краям, заглавными буквами
    size_t a = 0, b = in.size();
    while (a < b && (unsigned char)in[a] <= ' ') a++;
    while (b > a && (unsigned char)in[b - 1] <= ' ') b--;
    std::string o = in.substr(a, b - a);
    for (auto& ch : o) if (ch >= 'a' && ch <= 'z') ch -= 32;
    return o;
}
static void load_builtin() {
    if (g_builtin) return;
    g_builtin = true;
    for (const auto& e : LIST) g_map[norm(e.id)] = e.lvl;
}

extern "C" {

// dir - папка файлов игры (с завершающим /); из неё читается необязательный testers.txt
__attribute__((visibility("default"))) void rco_tester_init(const char* dir) {
    load_builtin();
    if (!dir) return;
    FILE* f = fopen((std::string(dir) + "testers.txt").c_str(), "r");
    if (!f) return;
    char line[200];
    while (fgets(line, sizeof line, f)) {
        std::string l(line);
        size_t st = l.find_first_not_of(" \t\r\n");                               // пробелы в начале строки не мешают
        if (st == std::string::npos) continue;
        l = l.substr(st);
        if (l[0] == '#') continue;
        size_t sp = l.find_first_of(" \t,;=");
        if (sp == std::string::npos) continue;
        std::string id = norm(l.substr(0, sp));
        if (!id.empty()) g_map[id] = atoi(l.c_str() + sp + 1);
    }
    fclose(f);
}

// сколько игроков в списке
__attribute__((visibility("default"))) int rco_tester_count() {
    load_builtin();
    return (int)g_map.size();
}

// уровень тестера по ID; 0, если игрока нет в списке
__attribute__((visibility("default"))) int rco_tester_level(const char* id) {
    load_builtin();
    if (!id) return 0;
    auto it = g_map.find(norm(id));
    return it == g_map.end() ? 0 : it->second;
}

}
