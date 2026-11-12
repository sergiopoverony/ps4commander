// ps4commander: файловый менеджер для PS4 в стиле Norton Commander (веб-интерфейс).
// Created by SergioPoverony and Mr.Claude. License: GPLv3.
//
// В покое ничего не делает: ждёт обращения к веб-интерфейсу. Все изменения файлов выполняются
// только по явной команде пользователя и только в фоновом задании с прогрессом и отменой.
#include "http.h"
#include "install.h"
#include "jobs.h"
#include "log.h"
#include "platform.h"
#include "version.h"
#include <cstdlib>

int main() {
    log_open(DATA_DIR);   // первым делом: по этому файлу видно, что программа дошла до main()
    app_log("version %s", APP_VERSION);
    int port = APP_PORT;
#ifdef HOST_TEST
    if (const char* e = getenv("FM_PORT")) port = atoi(e);
#endif
    int rc = http_run(port);
    job_shutdown();
    install_shutdown();
    app_log("завершение, код %d", rc);
    if (rc == 0) notify("ps4commander: выключен");
    return rc;
}
