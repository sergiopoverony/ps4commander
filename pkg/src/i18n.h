#pragma once
#include <string>

// Тексты интерфейса приложения. Используются те же файлы, что и у веб-страницы
// (lang/en.json, lang/ru.json из корня проекта) - один набор переводов на оба интерфейса.
// Встроенные копии лежат в PKG как assets/misc/lang_<код>.json.
// Английский загружается всегда первым как запасной, поверх него - системный/выбранный язык.

void i18n_init(const char* assetDir = "/app0/assets/misc");
std::string tr(const char* key, const char* def);

// Переключение языка из меню приложения (помимо автоопределения по консоли при старте).
// code - "ru" или "en". Если встроенного файла для этого кода нет, язык не меняется.
void i18n_set(const char* code);
std::string i18n_current();
