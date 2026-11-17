// ============================================================================
//  dumper.h - выгрузка структуры классов игры (поля, методы, типы) в текстовый файл.
//  Заменяет Il2CppDumper: работает прямо внутри игры через il2cpp API.
//  Включается константой DUMP_ON_START в main.cpp. Дамп пишется один раз
//  (если файл уже есть - пропускается). Файл: Android/data/<пакет>/files/rco_dump.txt
// ============================================================================
#pragma once
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace rcodump {

typedef void* (*f_p_p)(void*);
typedef void* (*f_p_pp)(void*, void*);
typedef const char* (*f_s_p)(void*);
typedef const char* (*f_s_pu)(const void*);
typedef size_t (*f_z_p)(void*);
typedef void* (*f_p_pz)(void*, size_t);
typedef void* (*f_p_pu)(void*, uint32_t);
typedef int (*f_i_p)(void*);
typedef int (*f_i_pp)(void*, uint32_t*);
typedef uint32_t (*f_u_p)(void*);
typedef int (*f_i_pu)(const void*);

static f_p_pp  api_domain_get_assemblies;     // (domain, size_t*) -> Assembly**
static f_p_p   api_assembly_get_image;
static f_s_p   api_image_get_name;
static f_z_p   api_image_get_class_count;
static f_p_pz  api_image_get_class;
static f_s_p   api_class_get_name, api_class_get_namespace;
static f_p_p   api_class_get_parent;
static f_p_pp  api_class_get_fields, api_class_get_methods;
static f_s_p   api_field_get_name;
static f_p_p   api_field_get_type, api_method_get_return_type;
static int   (*api_field_get_offset)(void*);
static int   (*api_field_get_flags)(void*);
static const char* (*api_type_get_name)(const void*);
static f_s_p   api_method_get_name;
static uint32_t (*api_method_get_param_count)(void*);
static const void* (*api_method_get_param)(void*, uint32_t);
static const char* (*api_method_get_param_name)(void*, uint32_t);
static uint32_t (*api_method_get_flags)(void*, uint32_t*);

static bool load(void* h) {
#define L(n) *(void**)&api_##n = dlsym(h, "il2cpp_" #n); if (!api_##n) return false;
    L(domain_get_assemblies) L(assembly_get_image) L(image_get_name) L(image_get_class_count) L(image_get_class)
    L(class_get_name) L(class_get_namespace) L(class_get_parent) L(class_get_fields) L(class_get_methods)
    L(field_get_name) L(field_get_type) L(method_get_return_type) L(field_get_offset) L(field_get_flags)
    L(type_get_name) L(method_get_name) L(method_get_param_count) L(method_get_param) L(method_get_param_name) L(method_get_flags)
#undef L
    return true;
}

static bool ci_has(const char* s, const char* sub) {
    size_t n = strlen(sub);
    for (; *s; s++) { size_t i = 0; while (i < n && s[i] && (s[i] | 32) == (sub[i] | 32)) i++; if (i == n) return true; }
    return false;
}

// классы, структура которых нужна для ботов (поиск по части имени, без учёта регистра)
static const char* const KEYS[] = {
    "avatar", "health", "spawn", "respawn", "hit", "damage", "team", "gamemode", "player", "weapon", "shoot", "fire",
    "death", "dead", "kill", "score", "room", "enemy", "bot", "ragdoll", "skin", "photon", "preasset", "projectile",
    "bullet", "mapservice", "match", "round", "controller", "fpweapon", "vp_player", "vp_fp", "inventory", "nickname"
};

static bool wanted(const char* cls) {
    for (const char* k : KEYS) if (ci_has(cls, k)) return true;
    return false;
}
static bool wanted_asm(const char* a) {
    return !strcmp(a, "Assembly-CSharp") || !strncmp(a, "PixelCombats", 12) || !strcmp(a, "UnityUtils") ||
           !strcmp(a, "BackEnd") || !strncmp(a, "Scripts", 7);
}

static void dump_class(FILE* f, void* k, const char* asmName) {
    const char* ns = api_class_get_namespace(k); const char* nm = api_class_get_name(k);
    void* par = api_class_get_parent(k);
    fprintf(f, "\n// %s\nclass %s%s%s", asmName, ns && *ns ? ns : "", ns && *ns ? "." : "", nm);
    if (par) { const char* pn = api_class_get_name(par); const char* pns = api_class_get_namespace(par); fprintf(f, " : %s%s%s", pns && *pns ? pns : "", pns && *pns ? "." : "", pn); }
    fprintf(f, " {\n");
    void* it = nullptr; void* fld;
    while ((fld = api_class_get_fields(k, &it))) {
        const void* t = api_field_get_type(fld);
        fprintf(f, "  %s%s %s; // off=0x%X\n", (api_field_get_flags(fld) & 0x10) ? "static " : "", t ? api_type_get_name(t) : "?", api_field_get_name(fld), api_field_get_offset(fld));
    }
    it = nullptr; void* m;
    while ((m = api_class_get_methods(k, &it))) {
        uint32_t ifl = 0; uint32_t fl = api_method_get_flags(m, &ifl);
        const void* rt = api_method_get_return_type(m);
        fprintf(f, "  %s%s %s(", (fl & 0x10) ? "static " : "", rt ? api_type_get_name(rt) : "?", api_method_get_name(m));
        uint32_t pc = api_method_get_param_count(m);
        for (uint32_t i = 0; i < pc; i++) {
            const void* pt = api_method_get_param(m, i); const char* pn = api_method_get_param_name(m, i);
            fprintf(f, "%s%s %s", i ? ", " : "", pt ? api_type_get_name(pt) : "?", pn ? pn : "");
        }
        fprintf(f, ");\n");
    }
    fprintf(f, "}\n");
}

// h - handle libil2cpp.so, domain - il2cpp_domain_get(), outPath - куда писать
static bool run(void* h, void* domain, const std::string& outPath) {
    if (!load(h)) return false;
    FILE* chk = fopen(outPath.c_str(), "r");
    if (chk) { fclose(chk); return true; }                               // уже выгружено раньше
    FILE* f = fopen(outPath.c_str(), "w");
    if (!f) return false;
    size_t n = 0;
    void** asms = (void**)api_domain_get_assemblies(domain, &n);
    fprintf(f, "// RCO dump. сборок: %zu\n", n);
    long budget = 14L * 1024 * 1024;                                     // потолок размера файла
    for (size_t a = 0; a < n; a++) {
        void* img = api_assembly_get_image(asms[a]);
        const char* an = api_image_get_name(img);
        if (!wanted_asm(an)) continue;
        size_t cc = api_image_get_class_count(img);
        fprintf(f, "\n// ===== %s (классов: %zu) =====\n", an, cc);
        for (size_t i = 0; i < cc; i++) {                                // сначала список всех имён - чтобы найти нужное
            void* k = api_image_get_class(img, i);
            const char* ns = api_class_get_namespace(k);
            fprintf(f, "//  %s%s%s\n", ns && *ns ? ns : "", ns && *ns ? "." : "", api_class_get_name(k));
        }
    }
    for (size_t a = 0; a < n; a++) {
        void* img = api_assembly_get_image(asms[a]);
        const char* an = api_image_get_name(img);
        if (!wanted_asm(an)) continue;
        size_t cc = api_image_get_class_count(img);
        for (size_t i = 0; i < cc; i++) {
            void* k = api_image_get_class(img, i);
            if (!wanted(api_class_get_name(k))) continue;
            dump_class(f, k, an);
            if (ftell(f) > budget) { fprintf(f, "\n// файл обрезан по размеру\n"); fclose(f); return true; }
        }
    }
    fclose(f);
    return true;
}

}  // namespace rcodump
