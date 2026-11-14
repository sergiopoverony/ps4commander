#pragma once
#include <string>

// Тексты интерфейса приложения. Используются те же файлы, что и у веб-страницы
// (lang/en.json, lang/ru.json из корня проекта) - один набор переводов на оба интерфейса.
// Встроенные копии лежат в PKG как assets/misc/lang_<код>.json.
// Английский загружается всегда первым как запасной, поверх него - системный/выбранный язык.

void i18n_init(const char* assetDir = "/app0/assets/misc");
std::string tr(const char* key, const char* def);

// Переключение языка из меню приложения (помимо автоопределения по консоли при старте).
// code - из i18n_code(i). Если встроенного файла для этого кода нет, язык не меняется.
void i18n_set(const char* code);
std::string i18n_current();
std::string i18n_name();                 // название текущего языка на нём самом (поле _name)
int i18n_count();                        // сколько языков встроено
const char* i18n_code(int i);            // код i-го языка (0 - en)
void i18n_cycle(int dir);                // следующий/предыдущий язык
