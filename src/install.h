#pragma once
// Установка .pkg через системный BGFT (libSceBgft). Очередь, один пакет за раз.
#include <string>
#include <vector>

// Ставит пути в очередь. false и err (bad_path, not_pkg, no_bgft, ...) при отказе; ничего не ставится, если хоть один путь плохой.
bool install_start(const std::vector<std::string>& paths, std::string& err);
std::string install_json();
void install_cancel();
void install_shutdown();
// Путь к pkg из очереди по его Content ID (для отдачи системному установщику файлов с сетевых папок).
bool install_path_for_token(const std::string& token, std::string& path);
