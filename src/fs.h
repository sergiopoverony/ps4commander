#pragma once
// Операции с файловой системой консоли: список, чтение, создание/переименование, права,
// проверки безопасности. Все пути абсолютные и нормализованные (без «.», «..», «//»).
#include <string>
#include <vector>

// Нормализует путь. Пустая строка, если путь не абсолютный, содержит NUL или выходит выше корня.
std::string fs_normalize(const std::string& p);
// Склеивает каталог и имя (имя без «/»).
std::string fs_join(const std::string& dir, const std::string& name);
std::string fs_basename(const std::string& norm);
std::string fs_parent(const std::string& norm);
// true, если child лежит внутри dir (или равен ему).
bool fs_inside(const std::string& dir, const std::string& child);

// Уровень опасности пути:
//   0 - обычный;
//   1 - системная область (/system, /dev, ...): менять только с явным подтверждением (unsafe);
//   2 - защищённый: корень, каталоги верхнего уровня и точки монтирования - сами не изменяются никогда.
int fs_danger(const std::string& norm);
// Для тестов на ПК: чтение разрешено только внутри FM_ROOT. На консоли всегда пустая строка.
std::string fs_check_read(const std::string& norm);
// Пустая строка, если менять путь можно; иначе код ошибки: outside_root, protected, unsafe.
// as_container=true: путь только вмещает новые файлы (назначение копирования) - защита уровня 2 не применяется.
std::string fs_check_modify(const std::string& norm, bool unsafe, bool as_container);

std::string fs_roots_json();
// JSON со списком каталога. При ошибке возвращает "" и заполняет err (имя errno или код).
std::string fs_ls_json(const std::string& norm, std::string& err);
// Кусок файла [off, off+len). len ограничивается 1 МиБ.
bool fs_read_range(const std::string& norm, long long off, long long len, std::string& out, long long& size, std::string& err);

bool fs_mkdir(const std::string& norm, bool unsafe, std::string& err);
bool fs_rename(const std::string& from, const std::string& to, bool unsafe, std::string& err);
// Имя errno в виде короткого кода (EACCES -> "EACCES").
std::string fs_errname(int e);

// Поиск по имени (подстрока без учёта регистра или маска со * и ?) в каталоге и ниже. Символические ссылки
// не раскрываются. Ограничения: max_results найденных и max_secs секунд.
std::string fs_find_json(const std::string& root, const std::string& pattern, int max_results, int max_secs, std::string& err);
// Совпадение имени с маской (* и ?), без учёта регистра. Используется поиском и тестами.
bool fs_glob(const std::string& pattern, const std::string& name);
