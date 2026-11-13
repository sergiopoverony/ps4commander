#pragma once
#include <netinet/in.h>

bool net_init();
bool net_resolve(const char* host, struct in_addr* out);
int  net_connect(const char* host, int port, int timeout_ms);  // блокирующий, сокет или -1
int  net_connect_nb(const char* ip, int port);                 // неблокирующий, ip только цифрами