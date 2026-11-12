#pragma once
// Всё, что отличается между консолью и тестом на ПК (HOST_TEST).
#ifdef HOST_TEST
#ifndef DATA_DIR
#define DATA_DIR "/tmp/ps4commander_test_data"
#endif
#else
#define DATA_DIR "/data/ps4commander"
#endif

#include <string>
// IPv4-адрес консоли в локальной сети (не 127.0.0.1). Пустая строка, если определить не удалось.
std::string local_ip();
// Уведомление на экране консоли.
void notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Сбрасывает таймер сна консоли. Вызывать не реже раза в ~10 с во время долгой операции.
void keep_awake_tick();
// Своя подсеть: адрес и маска хоста (в порядке байт хоста). false, если определить не удалось.
bool local_subnet(unsigned& ip, unsigned& mask);
