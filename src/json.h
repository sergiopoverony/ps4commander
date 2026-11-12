#pragma once
// Минимальный разбор тел запросов: плоский объект со строками, числами, булевыми значениями
// и массивами строк. Этого хватает для API; вложенные объекты не поддерживаются.
#include <map>
#include <string>
#include <vector>

struct JsonObj {
    bool ok = false;
    std::map<std::string, std::string> str;
    std::map<std::string, double> num;
    std::map<std::string, bool> flag;
    std::map<std::string, std::vector<std::string>> arr;

    std::string get_str(const char* k, const std::string& d = "") const {
        auto it = str.find(k);
        return it == str.end() ? d : it->second;
    }
    double get_num(const char* k, double d = 0) const {
        auto it = num.find(k);
        return it == num.end() ? d : it->second;
    }
    bool get_flag(const char* k, bool d = false) const {
        auto it = flag.find(k);
        return it == flag.end() ? d : it->second;
    }
    const std::vector<std::string>* get_arr(const char* k) const {
        auto it = arr.find(k);
        return it == arr.end() ? nullptr : &it->second;
    }
};

JsonObj json_parse(const std::string& body);
// Экранирование строки для JSON (без внешних кавычек). Невалидный UTF-8 заменяется на U+FFFD.
std::string json_esc(const std::string& s);
