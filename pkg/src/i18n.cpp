#include "i18n.h"
#include "json.h"
#include "log.h"

#include <map>
#include <string>
#include <stdio.h>

// Язык системы PS4: sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &id).
extern "C" int sceSystemServiceParamGetInt(int paramId, int* value);
static const int PARAM_ID_LANG = 1;

static std::string g_want = "en";
static std::map<std::string, std::string> g_strings;

static void merge_json(const JsonValue& j)
{
    for (size_t i = 0; i < j.obj.size(); i++)
        if (j.obj[i].second.type == JsonValue::STR) g_strings[j.obj[i].first] = j.obj[i].second.str;
}

static bool load_bundled(const std::string& path, JsonValue& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string body;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);
    return json_parse(body, out) && out.type == JsonValue::OBJ;
}

// ID языка PS4 -> код файла lang_<код>.json. Встроены только en и ru (см. pkg/Makefile) -
// любой другой системный язык даёт английский по умолчанию, пока не добавят файл.
static std::string lang_for_system(int id)
{
    switch (id) {
    case 8: return "ru";    // русский
    default: return "en";
    }
}

static std::string asset_dir_g = "/app0/assets/misc";

void i18n_init(const char* assetDir)
{
    asset_dir_g = assetDir;
    g_strings.clear();

    JsonValue en;
    bool enOk = load_bundled(asset_dir_g + "/lang_en.json", en);
    if (enOk) merge_json(en);

    int id = -1;
    int r = sceSystemServiceParamGetInt(PARAM_ID_LANG, &id);
    if (r != 0) id = -1;
    std::string want = lang_for_system(id);
    logf_("i18n: системный язык id %d (rc 0x%08X) -> '%s'", id, (unsigned)r, want.c_str());

    g_want = "en";
    if (want != "en") {
        JsonValue j;
        if (load_bundled(asset_dir_g + "/lang_" + want + ".json", j)) { merge_json(j); g_want = want; }
    }
    logf_("i18n: используется '%s', загружено %d строк (en %s)", g_want.c_str(), (int)g_strings.size(), enOk ? "ok" : "не найден");
}

std::string tr(const char* key, const char* def)
{
    std::map<std::string, std::string>::const_iterator it = g_strings.find(key);
    if (it != g_strings.end() && !it->second.empty()) return it->second;
    return def;
}

void i18n_set(const char* code)
{
    if (std::string(code) == g_want) return;
    JsonValue en, j;
    bool enOk = load_bundled(asset_dir_g + "/lang_en.json", en);
    if (!load_bundled(asset_dir_g + "/lang_" + code + ".json", j)) {
        logf_("i18n: нет встроенного файла для '%s', язык не изменён", code);
        return;
    }
    g_strings.clear();
    if (enOk) merge_json(en);
    merge_json(j);
    g_want = code;
    logf_("i18n: переключено на '%s' из меню", code);
}

std::string i18n_current() { return g_want; }
