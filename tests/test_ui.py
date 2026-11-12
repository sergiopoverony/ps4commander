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
        check('Enter на .pkg: предложена установка', 'game.pkg' in pg.locator('.dlg').inner_text() and pg.locator('.viewer').count() == 0, pg.locator('.dlg').inner_text())
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>/game.pkg/.test(document.querySelector(".dlg pre")&&document.querySelector(".dlg pre").textContent||"")', timeout=10000)
        check('очередь установки показана', True)
        pg.wait_for_function('()=>/installed|установлено/.test(document.querySelector(".dlg pre").textContent)', timeout=15000)
        pg.keyboard.press('Escape')
        pg.wait_for_function('()=>!overlay.classList.contains("on")')

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
