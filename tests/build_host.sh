#!/bin/sh
# Сборка тестового бинарника для ПК: build/host_test (с ASAN/UBSAN). libsmb2 собирается один раз (без санитайзеров).
set -e
cd "$(dirname "$0")/.."
SMB=third_party/libsmb2
mkdir -p build/smb2_host
for f in $(cat $SMB/SOURCES.txt); do
  o=build/smb2_host/${f%.c}.o
  if [ ! -f "$o" ] || [ "$SMB/lib/$f" -nt "$o" ]; then
    gcc -c -O2 -w -include stddef.h "-D_U_=__attribute__((unused))" -DHAVE_CONFIG_H -D_GNU_SOURCE \
      -I$SMB -I$SMB/include -I$SMB/include/smb2 -I$SMB/lib -o "$o" "$SMB/lib/$f"
  fi
done
python3 tools/embed.py build/assets_gen.h
g++ -std=c++17 -g -pthread -fsanitize=address,undefined -DHOST_TEST -Wall -Wextra -Isrc -Ibuild -I$SMB/include \
  -o build/host_test src/*.cpp build/smb2_host/*.o
