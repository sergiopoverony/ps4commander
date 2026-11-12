#!/usr/bin/env python3
"""Проверка веб-страницы в headless Chromium (Playwright) против службы на ПК.
Запуск: python3 tests/test_ui.py build/host_test"""
import os, shutil, subprocess, sys, time, urllib.request
from playwright.sync_api import sync_playwright

BIN = sys.argv[1]
BASE = '/tmp/fmui'
ROOT = BASE + '/root'
PORT = 18585
shots = os.environ.get('SHOTS', '')
fails = []


def check(name, cond, extra=''):
    print(('PASS ' if cond else 'FAIL ') + name + (' ' + str(extra) if not cond else ''))
    if not cond:
        fails.append(name)


shutil.rmtree(BASE, ignore_errors=True)
shutil.rmtree('/tmp/ps4commander_test_data', ignore_errors=True)
os.makedirs(ROOT + '/docs/inner')
os.makedirs(ROOT + '/other')
for i in range(300):
    open(ROOT + '/docs/file%03d.txt' % i, 'w').write('line %d\n' % i)
open(ROOT + '/docs/readme.md', 'w').write('# Привет\nтекст\n')
open(ROOT + '/docs/data.bin', 'wb').write(bytes(range(256)) * 4)
open(ROOT + '/docs/pic.png', 'wb').write(bytes.fromhex(
    '89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6360000002000001e221bc330000000049454e44ae426082'))
env = dict(os.environ, FM_PORT=str(PORT), FM_ROOT=ROOT, ASAN_OPTIONS='detect_leaks=0')
srv = subprocess.Popen([BIN], env=env, stdout=open(BASE + '/out.txt', 'w'), stderr=subprocess.STDOUT)
for _ in range(50):
    try:
        urllib.request.urlopen('http://127.0.0.1:%d/api/roots' % PORT); break
    except Exception:
        time.sleep(0.1)

try:
    with sync_playwright() as pw:
        br = pw.chromium.launch()
        ctx = br.new_context(viewport={'width': 1280, 'height': 760}, accept_downloads=True)
        pg = ctx.new_page()
        errors = []
        pg.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)
        pg.on('pageerror', lambda e: errors.append(str(e)))
        pg.goto('http://127.0.0.1:%d/' % PORT)
        pg.wait_for_selector('.row')
        check('две панели, строки отрисованы', pg.locator('.panel').count() == 2 and pg.locator('.row').count() > 0)
        check('заголовок версии', pg.locator('#ver').inner_text().strip() != '__VERSION__')

        def names(i):
            return pg.evaluate('(i)=>panels[i].view.map(e=>e.n)', i)

        def cur(i=None):
            return pg.evaluate('()=>panels[active].view[panels[active].cur].n')

        check('левая панель = корень теста', pg.evaluate('()=>panels[0].path') == ROOT)
        # навигация с клавиатуры
        pg.keyboard.press('ArrowDown')
        check('курсор: вниз', cur() == 'docs', cur())
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[0].path.endsWith("/docs")')
        check('Enter входит в папку', pg.evaluate('()=>panels[0].path') == ROOT + '/docs')
        check('виртуализация: в DOM мало строк при 300+ файлах', pg.locator('.panel').first.locator('.row').count() < 60, pg.locator('.row').count())
        pg.keyboard.press('End')
        check('End: последний элемент', cur() == 'readme.md', cur())
        pg.keyboard.press('Home')
        check('Home: «..»', cur() == '..')
        pg.keyboard.press('Backspace')
        pg.wait_for_function('()=>panels[0].path.endsWith("/root")')
        check('Backspace: наверх, курсор на папке', cur() == 'docs', cur())
        pg.keyboard.press('Tab')
        check('Tab переключает панель', pg.evaluate('()=>active') == 1)
        pg.keyboard.press('Tab')

        # создание папки F7
        pg.keyboard.press('F7')
        pg.fill('.dlg input[type=text]', 'created')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="created")')
        check('F7: папка создана', os.path.isdir(ROOT + '/created'))

        # выбор Insert и копирование F5 в другую панель
        pg.evaluate('()=>load(panels[1], "%s/other", null)' % ROOT)
        pg.wait_for_function('()=>panels[1].path.endsWith("/other")')
        pg.evaluate('()=>load(panels[0], "%s/docs", null)' % ROOT)
        pg.wait_for_function('()=>panels[0].path.endsWith("/docs")')
        pg.keyboard.press('ArrowDown')          # file000.txt
        pg.keyboard.press('Insert')             # выбрать и сдвинуться
        pg.keyboard.press('Insert')
        check('Insert: отмечено 2 файла', pg.evaluate('()=>panels[0].sel.size') == 2)
        if shots: pg.screenshot(path=shots + '/panels.png')
        selnames = sorted(pg.evaluate('()=>Array.from(panels[0].sel)'))
        pg.keyboard.press('F5')
        inp = pg.locator('.dlg input[type=text]')
        check('F5: путь назначения = другая панель', inp.input_value() == ROOT + '/other', inp.input_value())
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[1].entries.length===2', timeout=10000)
        pg.wait_for_function('()=>!overlay.classList.contains("on")', timeout=10000)
        check('F5: файлы скопированы', sorted(os.listdir(ROOT + '/other')) == selnames, str(selnames) + str(os.listdir(ROOT + '/other')))
        check('F5: правая панель обновилась', pg.evaluate('()=>panels[1].entries.length') == 2)

        # маска
        pg.keyboard.press('Escape') if False else None
        pg.keyboard.press('+')
        pg.fill('.dlg input[type=text]', 'file00*.txt')
        pg.keyboard.press('Enter')
        check('+ маска: отмечено file00*.txt (10)', pg.evaluate('()=>panels[0].sel.size') == 10, pg.evaluate('()=>panels[0].sel.size'))
        pg.keyboard.press('-')
        pg.fill('.dlg input[type=text]', '*')
        pg.keyboard.press('Enter')
        check('- маска: снята', pg.evaluate('()=>panels[0].sel.size') == 0)

        # просмотр текста F3
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="readme.md");render(p)}')
        pg.keyboard.press('F3')
        pg.wait_for_selector('.viewer')
        pg.wait_for_function('()=>document.querySelector(".viewer").textContent.includes("Привет")')
        check('F3: текст с кириллицей', 'Привет' in pg.locator('.viewer').inner_text())
        pg.keyboard.press('Escape')
        # hex для двоичного файла
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="data.bin");render(p)}')
        pg.keyboard.press('F3')
        pg.wait_for_function('()=>document.querySelector(".viewer").textContent.includes("00000000")')
        check('F3: двоичный файл показан как hex', '00 01 02 03' in pg.locator('.viewer').inner_text())
        pg.keyboard.press('Escape')
        # картинка
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="pic.png");render(p)}')
        pg.keyboard.press('F3')
        pg.wait_for_selector('.viewer img')
        pg.wait_for_function('()=>{const i=document.querySelector(".viewer img");return i&&i.complete&&i.naturalWidth===1}')
        check('F3: картинка отображается', True)
        pg.keyboard.press('Escape')

        # редактор F4
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="readme.md");render(p)}')
        pg.keyboard.press('F4')
        pg.wait_for_selector('textarea')
        pg.fill('textarea', 'новый текст\nвторая строка\n')
        pg.keyboard.press('Control+s')
        pg.wait_for_function('()=>document.querySelector(".dlg .kv").textContent.length>0 && !document.querySelector(".dlg .kv").textContent.startsWith("*")')
        check('F4: сохранение в файл', open(ROOT + '/docs/readme.md', encoding='utf-8').read() == 'новый текст\nвторая строка\n')
        pg.keyboard.press('Escape')
        pg.wait_for_function('()=>!overlay.classList.contains("on")')

        # переименование Shift+F6
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="readme.md");render(p)}')
        pg.keyboard.press('Shift+F6')
        pg.fill('.dlg input[type=text]', 'README.txt')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="README.txt")')
        check('Shift+F6: переименовано', os.path.exists(ROOT + '/docs/README.txt') and not os.path.exists(ROOT + '/docs/readme.md'))

        # права F9
        pg.keyboard.press('F9')
        pg.fill('.dlg input[type=text]', '600')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="README.txt"&&e.p==="600")', timeout=10000)
        pg.wait_for_function('()=>!overlay.classList.contains("on")', timeout=10000)
        check('F9: права изменены', (os.stat(ROOT + '/docs/README.txt').st_mode & 0o777) == 0o600)

        # поиск
        pg.keyboard.press('Control+f')
        pg.fill('.dlg input[type=text]', 'file29*')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>document.querySelectorAll(".dlg button[title]").length>=10')
        check('Ctrl+F: найдено 10 файлов', pg.locator('.dlg button[title]').count() == 10, pg.locator('.dlg button[title]').count())
        pg.locator('.dlg button[title]').first.click()
        pg.wait_for_function('()=>!overlay.classList.contains("on")')
        check('результат поиска: переход к файлу', pg.evaluate('()=>panels[0].view[panels[0].cur].n').startswith('file29'))

        # свойства + размер папки
        pg.evaluate('()=>load(panels[0], "%s", null)' % ROOT)
        pg.wait_for_function('()=>panels[0].path.endsWith("/root")')
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="docs");render(p)}')
        pg.keyboard.press('Alt+Enter')
        pg.get_by_text('Calculate size').click()
        pg.wait_for_function('()=>document.querySelector(".dlg pre")&&/objects/.test(document.querySelector(".dlg").textContent)', timeout=10000)
        check('Alt+Enter: размер папки посчитан', 'objects' in pg.locator('.dlg').inner_text())
        pg.keyboard.press('Escape')

        # скачивание
        pg.evaluate('()=>load(panels[0], "%s/docs", null)' % ROOT)
        pg.wait_for_function('()=>panels[0].path.endsWith("/docs")')
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="data.bin");render(p)}')
        with pg.expect_download() as dl:
            pg.evaluate('()=>doDownload()')
        check('скачивание файла', dl.value.suggested_filename == 'data.bin')

        # загрузка через input
        open(BASE + '/up.txt', 'w').write('uploaded\n')
        pg.evaluate('()=>{$("#file").value=""}')
        pg.set_input_files('#file', BASE + '/up.txt')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="up.txt")', timeout=10000)
        check('загрузка файла', open(ROOT + '/docs/up.txt').read() == 'uploaded\n')

        # удаление F8 нескольких
        pg.evaluate('()=>{const p=panels[0];p.sel=new Set(["up.txt","data.bin"]);render(p)}')
        pg.keyboard.press('F8')
        pg.wait_for_selector('.dlg.err')
        if shots: pg.screenshot(path=shots + '/delete.png')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>!panels[0].entries.some(e=>e.n==="up.txt")', timeout=10000)
        check('F8: удалено', not os.path.exists(ROOT + '/docs/up.txt') and not os.path.exists(ROOT + '/docs/data.bin'))

        # ошибка: запрещённый путь показывается пользователю
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="inner");render(p)}')
        pg.keyboard.press('F5')
        pg.fill('.dlg input[type=text]', '/tmp')
        pg.keyboard.press('Enter')
        pg.wait_for_selector('.dlg.err')
        check('копирование вне корня: сообщение об ошибке', 'allowed folder' in pg.locator('.dlg.err').inner_text() or 'Outside' in pg.locator('.dlg.err').inner_text(), pg.locator('.dlg.err').inner_text())
        pg.keyboard.press('Escape')

        # XSS: имя файла с HTML не исполняется
        open(ROOT + '/docs/<img src=x onerror=window.__xss=1>.txt', 'w').write('x')
        pg.evaluate('()=>refreshAll()')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n.startsWith("<img"))')
        check('имя с HTML не исполняется (XSS)', pg.evaluate('()=>window.__xss') is None and pg.locator('.row img').count() == 0)

        # Enter на .pkg предлагает установку (а не просмотр)
        h = bytearray(0x200); h[0:4] = b'\x7fCNT'; h[0x40:0x40 + 36] = b'UP0000-CUSA00000_00-TESTTESTTESTTEST'
        open(ROOT + '/docs/game.pkg', 'wb').write(bytes(h))
        pg.evaluate('()=>refreshAll()')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="game.pkg")')
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="game.pkg");render(p)}')
        pg.keyboard.press('Enter')
        pg.wait_for_selector('.dlg')
        check('Enter на .pkg: сначала предупреждение о поиске связанных пакетов', ('DLC' in pg.locator('.dlg').inner_text()) and pg.locator('.viewer').count() == 0 and pg.locator('.dlg button').count() == 2, pg.locator('.dlg').inner_text())
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>/install|Установить/i.test((document.querySelector(".dlg button.primary")||{}).textContent||"")&&!/OK/.test(document.querySelector(".dlg button.primary").textContent)', timeout=10000)
        check('нечитаемый pkg: затем обычное подтверждение установки', 'game.pkg' in pg.locator('.dlg').inner_text(), pg.locator('.dlg').inner_text())
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>/game.pkg/.test(document.querySelector(".dlg pre")&&document.querySelector(".dlg pre").textContent||"")', timeout=10000)
        check('очередь установки показана', True)
        check('в очереди есть полоса прогресса на всю ширину', pg.evaluate('()=>{const b=document.querySelector(".dlg .pbar");const w=document.querySelector(".dlg .pq");return !!b && b.getBoundingClientRect().width > 0.8 * w.getBoundingClientRect().width}'))
        pg.wait_for_function('()=>/installed|установлено/.test(document.querySelector(".dlg pre").textContent)', timeout=15000)
        pg.keyboard.press('Escape')
        pg.wait_for_function('()=>!overlay.classList.contains("on")')

        # окно выбора: рядом лежат патч и DLC той же игры
        import struct
        def mkpkg(rel, cid, ctype, flags, title, ver):
            ents = [(b'APP_VER', ver.encode() + b'\0'), (b'TITLE', title.encode() + b'\0')]
            keys = b''.join(k + b'\0' for k, _ in ents)
            kofs = 20 + 16 * len(ents)
            tab, data, ko, do = b'', b'', 0, 0
            for k, v in ents:
                tab += struct.pack('<HHIII', ko, 0x204, len(v), len(v), do); ko += len(k) + 1; data += v; do += len(v)
            sfo = b'\0PSF' + struct.pack('<IIII', 0x101, kofs, kofs + len(keys), len(ents)) + tab + keys + data
            b = bytearray(0x1000 + 100)
            b[0:4] = b'\x7fCNT'; b[0x10:0x14] = struct.pack('>I', 1); b[0x18:0x1c] = struct.pack('>I', 0x800)
            b[0x40:0x40 + 36] = cid.encode(); b[0x74:0x78] = struct.pack('>I', ctype); b[0x78:0x7c] = struct.pack('>I', flags)
            b[0x430:0x438] = struct.pack('>Q', len(b)); b[0x800:0x820] = struct.pack('>IIIIIIII', 0x1000, 0, 0, 0, 0xA00, len(sfo), 0, 0)
            b[0xA00:0xA00 + len(sfo)] = sfo
            os.makedirs(os.path.dirname(ROOT + rel), exist_ok=True)
            open(ROOT + rel, 'wb').write(bytes(b))
        mkpkg('/lib/main/Game.pkg', 'UP0000-CUSA55555_00-UITESTUITESTUITE', 0x1A, 0, 'UI Game', '01.00')
        mkpkg('/lib/upd/Patch.pkg', 'UP0000-CUSA55555_00-UITESTUITESTUITE', 0x1A, 0x40000000, 'UI Game', '01.05')
        mkpkg('/lib/dlc/Extra.pkg', 'UP0000-CUSA55555_00-UIDLCUIDLCUIDLC0', 0x1B, 0, 'UI DLC', '01.00')
        saved_path = pg.evaluate('()=>panels[0].path')
        pg.evaluate('(p)=>{panels[0].path=p;return refreshAll()}', ROOT + '/lib/main')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="Game.pkg")')
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="Game.pkg");render(p)}')
        pg.keyboard.press('Enter')
        pg.wait_for_selector('.dlg')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>document.querySelectorAll(".dlg input[type=checkbox]").length===3', timeout=10000)
        txt = pg.locator('.dlg').inner_text()
        check('окно выбора: игра, патч и DLC с галочками', 'Game.pkg' in txt and 'Patch.pkg' in txt and 'Extra.pkg' in txt and pg.locator('.dlg input:checked').count() == 3, txt)
        check('высота окна выбора по содержимому, а не во весь экран', pg.evaluate('()=>document.querySelector(".dlg").getBoundingClientRect().height') < 0.6 * 760, pg.evaluate('()=>document.querySelector(".dlg").getBoundingClientRect().height'))
        pg.locator('.dlg input[type=checkbox]').nth(2).uncheck()
        check('счётчик выбранных обновился', '(2)' in pg.locator('.dlg button.primary').inner_text(), pg.locator('.dlg button.primary').inner_text())
        pg.locator('.dlg button.primary').click()
        pg.wait_for_function('()=>/Game.pkg/.test(document.querySelector(".dlg pre")&&document.querySelector(".dlg pre").textContent||"")', timeout=10000)
        pg.wait_for_function('()=>{const t=document.querySelector(".dlg pre").textContent;return /Patch.pkg/.test(t)&&!/Extra.pkg/.test(t)}', timeout=10000)
        check('в очередь ушли только отмеченные, по порядку', pg.evaluate('()=>document.querySelector(".dlg pre").textContent.indexOf("Game.pkg")<document.querySelector(".dlg pre").textContent.indexOf("Patch.pkg")'))
        pg.wait_for_function('()=>(document.querySelector(".dlg pre").textContent.match(/installed|установлено/g)||[]).length>=2', timeout=20000)
        pg.keyboard.press('Escape')
        pg.wait_for_function('()=>!overlay.classList.contains("on")')
        pg.evaluate('(p)=>{panels[0].path=p;return refreshAll()}', saved_path)
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="inner")')

        # строка под панелями показывает полное имя выбранного файла
        longn = 'very_long_file_name_' + 'x' * 120 + '.txt'
        open(ROOT + '/docs/' + longn, 'w').write('1')
        pg.evaluate('()=>refreshAll()')
        pg.wait_for_function('(n)=>panels[0].entries.some(e=>e.n===n)', arg=longn)
        pg.evaluate('(n)=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n===n);render(p);updateFoot(p)}', longn)
        pg.evaluate('()=>{flashing=false;showName()}')
        check('под панелями полное имя выбранного файла', pg.locator('#msg').inner_text().strip() == longn, pg.locator('#msg').inner_text())
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="inner");render(p);updateFoot(p)}')
        check('у папки имя со слешем', pg.locator('#msg').inner_text().strip() == '/inner', pg.locator('#msg').inner_text())
        os.remove(ROOT + '/docs/' + longn)
        pg.evaluate('()=>refreshAll()')
        pg.wait_for_function('()=>panels[0].entries.some(e=>e.n==="inner")')

        # скачивание папки ZIP-архивом
        pg.evaluate('()=>{const p=panels[0];p.sel.clear();p.cur=p.view.findIndex(e=>e.n==="inner");render(p)}')
        with pg.expect_download(timeout=15000) as dl:
            pg.keyboard.press('Control+d')
        d = dl.value
        check('Ctrl+D на папке: скачан ZIP', d.suggested_filename == 'inner.zip', d.suggested_filename)
        import zipfile
        check('ZIP читается', zipfile.ZipFile(d.path()).testzip() is None)

        # запоминание левой панели между запусками
        pg.evaluate('()=>load(panels[0], "%s/docs", null)' % ROOT)
        pg.wait_for_function('()=>panels[0].path.endsWith("/docs")')
        pg.wait_for_timeout(900)
        pg.reload()
        pg.wait_for_selector('.row')
        pg.wait_for_function('()=>stateReady')
        check('левая панель восстановлена после перезапуска', pg.evaluate('()=>panels[0].path') == ROOT + '/docs', pg.evaluate('()=>panels[0].path'))

        check('isDiskPath: USB/EXT распознаются, остальное нет', pg.evaluate("()=>['/mnt/usb0','/mnt/usb1/games','/mnt/ext0/x'].every(isDiskPath)&&['/data','/mnt/usbx','/mnt/usb0evil','/mnt'].every(p=>!isDiskPath(p))"))

        # стартовые панели на «настоящей» консоли: слева /data, справа подключённый USB (ответы сервера подменены)
        import json as _json
        def fake_roots(route):
            route.fulfill(content_type='application/json', body=_json.dumps({'version': 'x', 'roots': [{'p': '/', 'mnt': False}, {'p': '/data', 'mnt': False}, {'p': '/mnt/usb0', 'mnt': False}, {'p': '/mnt/usb1', 'mnt': True}]}))
        def fake_ls(route):
            from urllib.parse import urlparse, parse_qs
            pth = parse_qs(urlparse(route.request.url).query)['path'][0]
            route.fulfill(content_type='application/json', body=_json.dumps({'path': pth, 'entries': [], 'free': 0, 'total': 0, 'danger': 0}))
        def fake_state(route):
            route.fulfill(content_type='application/json', body='{}')
        pg.route('**/api/roots', fake_roots); pg.route('**/api/ls*', fake_ls); pg.route('**/api/state', fake_state)
        pg.reload()
        pg.wait_for_function('()=>stateReady')
        check('старт: слева /data, справа подключённый USB', pg.evaluate('()=>[panels[0].path,panels[1].path]') == ['/data', '/mnt/usb1'], pg.evaluate('()=>[panels[0].path,panels[1].path]'))
        pg.unroute('**/api/roots'); pg.unroute('**/api/ls*'); pg.unroute('**/api/state')
        pg.reload()
        pg.wait_for_function('()=>stateReady')

        # язык и тема
        pg.keyboard.press('F10')
        pg.select_option('.dlg select >> nth=1', 'ru')
        pg.wait_for_function('()=>document.querySelector("#keys").textContent.includes("Копия")')
        check('язык переключается на русский', 'Удалить' in pg.locator('#keys').inner_text())
        pg.keyboard.press('F10')
        pg.select_option('.dlg select >> nth=0', 'dark')
        check('тёмная тема применяется', pg.evaluate('()=>document.documentElement.getAttribute("data-theme")') == 'dark')
        if shots: pg.screenshot(path=shots + '/dark.png')
        pg.keyboard.press('Escape')

        # мобильный размер
        pg.set_viewport_size({'width': 390, 'height': 800})
        time.sleep(0.4)
        pg.reload()
        pg.wait_for_selector('.row')
        check('мобильная раскладка: панели друг под другом', pg.evaluate('()=>{const a=panels[0].el.getBoundingClientRect(),b=panels[1].el.getBoundingClientRect();return b.top>=a.bottom-2}'))
        check('мобильная раскладка: нет горизонтальной прокрутки', pg.evaluate('()=>document.documentElement.scrollWidth<=window.innerWidth+1'))
        if shots: pg.screenshot(path=shots + '/mobile.png')

        errors = [e for e in errors if '403' not in e]  # ожидаемый отказ для запрещённого пути
        check('нет ошибок в консоли браузера', not errors, errors)
        br.close()
finally:
    srv.kill()

print('\nИТОГО: провалено %d' % len(fails))
sys.exit(1 if fails else 0)
