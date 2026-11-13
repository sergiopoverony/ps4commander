#pragma once
void log_reset();
void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
const char* log_path();
