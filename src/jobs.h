#pragma once
// Фоновые задания: копирование, перенос, удаление, смена прав. Одновременно выполняется одно.
#include <string>
#include <vector>

struct JobSpec {
    std::string op;                 // copy | move | delete | chmod | size
    std::vector<std::string> srcs;  // нормализованные пути
    std::string dest;               // каталог назначения (copy, move)
    bool overwrite = false;         // перезаписывать существующие файлы
    bool unsafe = false;            // подтверждены действия в системных областях
    int mode = 0;                   // права для chmod (например 0755)
    bool recursive = false;         // chmod: рекурсивно
};

// Запускает задание. false и err (busy, bad_op, bad_path, protected, unsafe, ...) при отказе.
bool job_start(const JobSpec& spec, std::string& err);
std::string job_json();
void job_cancel();
// Для завершения службы и тестов: отменяет задание и ждёт, пока оно остановится.
void job_shutdown();
