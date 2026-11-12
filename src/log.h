#pragma once
#include <string>
// Лог: кольцевой буфер в памяти (до 64 КБ) + файл log.txt. Потокобезопасен.
void log_open(const char* dir);
void app_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string log_tail(size_t max_bytes);
