// Юнит-тест чистых функций fs.cpp на ПК. Сборка:
//   g++ -std=c++17 -Wall -Wextra -DHOST_TEST -Isrc -o build/test_fs tests/test_fs.cpp src/fs.cpp src/json.cpp
#include "fs.h"
#include "json.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(name, cond) do { bool ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)

int main() {
    CHECK("normalize ..", fs_normalize("/a/b/../c") == "/a/c");
    CHECK("normalize //", fs_normalize("//a///b/") == "/a/b");
    CHECK("normalize . ", fs_normalize("/a/./b") == "/a/b");
    CHECK("normalize root", fs_normalize("/") == "/");
    CHECK("normalize выше корня", fs_normalize("/../x").empty());
    CHECK("normalize относительный", fs_normalize("a/b").empty());
    CHECK("normalize пустой", fs_normalize("").empty());
    CHECK("normalize NUL", fs_normalize(std::string("/a\0b", 4)).empty());
    CHECK("join", fs_join("/", "a") == "/a" && fs_join("/x", "a") == "/x/a");
    CHECK("basename/parent", fs_basename("/a/b") == "b" && fs_parent("/a/b") == "/a" && fs_parent("/a") == "/");
    CHECK("inside", fs_inside("/a", "/a/b") && fs_inside("/a", "/a") && !fs_inside("/a", "/ab") && fs_inside("/", "/x"));

    CHECK("danger /", fs_danger("/") == 2);
    CHECK("danger /data", fs_danger("/data") == 2);
    CHECK("danger /system", fs_danger("/system") == 2);
    CHECK("danger /mnt/usb0", fs_danger("/mnt/usb0") == 2);
    CHECK("danger /user/home", fs_danger("/user/home") == 2);
    CHECK("danger /data/x обычный", fs_danger("/data/x") == 0);
    CHECK("danger /mnt/usb0/x обычный", fs_danger("/mnt/usb0/x") == 0);
    CHECK("danger /system/x системный", fs_danger("/system/x") == 1);
    CHECK("danger /dev/x системный", fs_danger("/dev/x") == 1);
    CHECK("danger /preinst/x/y системный", fs_danger("/preinst/x/y") == 1);

    CHECK("glob *.txt", fs_glob("*.txt", "a.TXT") && !fs_glob("*.txt", "a.txt.bak"));
    CHECK("glob ?", fs_glob("a?c", "abc") && !fs_glob("a?c", "ac"));
    CHECK("glob середина", fs_glob("a*c*e", "abcde") && !fs_glob("a*c*e", "abd"));
    CHECK("glob пустое *", fs_glob("*", "") && fs_glob("*", "anything"));

    JsonObj j = json_parse("{\"op\":\"copy\",\"paths\":[\"/a\",\"/b c\"],\"overwrite\":true,\"mode\":\"755\",\"n\":12.5,\"u\":\"\\u0416\\n\"}");
    CHECK("json ok", j.ok);
    CHECK("json str", j.get_str("op") == "copy" && j.get_str("mode") == "755");
    CHECK("json arr", j.get_arr("paths") && j.get_arr("paths")->size() == 2 && (*j.get_arr("paths"))[1] == "/b c");
    CHECK("json flag/num", j.get_flag("overwrite") && j.get_num("n") == 12.5);
    CHECK("json unicode", j.get_str("u") == "\xD0\x96\n");
    CHECK("json битый", !json_parse("{\"a\":").ok && !json_parse("[]").ok && !json_parse("{\"a\":\"\\u0000\"}").ok);
    CHECK("json esc", json_esc("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
    CHECK("json esc битый utf8", json_esc("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK("json esc кириллица", json_esc("Привет") == "Привет");

    printf("\nИТОГО: провалено %d\n", fails);
    return fails ? 1 : 0;
}
