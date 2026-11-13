#pragma once
#include <string>
#include <vector>
#include <stdint.h>

// Простой разборщик JSON. Не универсальная библиотека: умеет ровно то, что нужно
// для ответов службы ps4hddtools (объекты, массивы объектов, строки, числа, bool).
// Если сервер когда-нибудь добавит новое поле, разбор не сломается: лишние поля
// просто пропускаются. Если поле отсутствует, возвращается значение по умолчанию.

struct JsonValue {
    enum Type { NONE, OBJ, ARR, STR, NUM, BOOL } type = NONE;
    std::string str;
    double num = 0;
    bool boolean = false;
    std::vector<std::pair<std::string, JsonValue> > obj;   // для OBJ
    std::vector<JsonValue> arr;                             // для ARR

    // Доступ к полю объекта; если поля нет, возвращает "пустое" значение (не указатель,
    // чтобы вызывающий код мог сразу писать json["x"]["y"].asInt()).
    const JsonValue& operator[](const char* key) const;

    std::string asStr(const std::string& def = "") const { return type == STR ? str : def; }
    long long asInt(long long def = 0) const { return type == NUM ? (long long)num : def; }
    double asNum(double def = 0) const { return type == NUM ? num : def; }
    bool asBool(bool def = false) const { return type == BOOL ? boolean : def; }
    bool isNull() const { return type == NONE; }
};

// Возвращает false при ошибке разбора (обрезанный ответ, не JSON и т.п.).
bool json_parse(const std::string& text, JsonValue& out);
