#!/usr/bin/env python3
"""Окно сетевых папок в headless Chromium против тестового SMB-сервера (нужен root и impacket).
Запуск: python3 tests/test_ui_smb.py build/host_test"""
import os, shutil, socket, subprocess, sys, time
from playwright.sync_api import sync_playwright

BIN = sys.argv[1]
BASE = '/tmp/fmuismb'
ROOT = BASE + '/root'
SHARE = BASE + '/share'
PORT = 18586
fails = []


def check(name, cond, extra=''):
    print(('PASS ' if cond else 'FAIL ') + name + (' ' + str(extra) if not cond else ''))
    if not cond:
        fails.append(name)


s = socket.socket()
try:
    s.bind(('127.0.0.1', 445))
except OSError:
    print('SKIP: порт 445 занят или нет прав'); sys.exit(0)
finally:
    s.close()
try:
    import impacket  # noqa
except ImportError:
    print('SKIP: нет impacket'); sys.exit(0)

shutil.rmtree(BASE, ignore_errors=True)
shutil.rmtree('/tmp/ps4commander_test_data', ignore_errors=True)
os.makedirs(ROOT); os.makedirs(SHARE + '/folder')
open(SHARE + '/file.txt', 'w').write('x')
srv = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'smb_server.py'), SHARE, '445'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    try:
        socket.create_connection(('127.0.0.1', 445), timeout=0.2).close(); break
    except OSError:
        time.sleep(0.1)
env = dict(os.environ, FM_PORT=str(PORT), FM_ROOT=ROOT, ASAN_OPTIONS='detect_leaks=0')
p = subprocess.Popen([BIN], env=env, stdout=open(BASE + '/out.txt', 'w'), stderr=subprocess.STDOUT)
import urllib.request
for _ in range(50):
    try:
        urllib.request.urlopen('http://127.0.0.1:%d/api/roots' % PORT, timeout=1).read(); break
    except Exception:
        time.sleep(0.1)
try:
    with sync_playwright() as pw:
        br = pw.chromium.launch()
        pg = br.new_context(viewport={'width': 1280, 'height': 760}).new_page()
        errors = []
        pg.on('pageerror', lambda e: errors.append(str(e)))
        pg.goto('http://127.0.0.1:%d/' % PORT)
        pg.wait_for_selector('.row')
        pg.keyboard.press('n')
        pg.wait_for_selector('.dlg')
        check('N открывает окно сетевых папок', 'SMB' in pg.locator('.dlg').inner_text())
        pg.get_by_role('button', name='Add').click()
        inputs = pg.locator('.dlg input')
        inputs.nth(0).fill('nas'); inputs.nth(1).fill('127.0.0.1'); inputs.nth(2).fill('DATA')
        inputs.nth(3).fill('test'); inputs.nth(4).fill('wrongpass')
        pg.get_by_role('button', name='Connect and save').click()
        pg.wait_for_function('()=>/Wrong user/.test(document.querySelector(".dlg").textContent)', timeout=15000)
        check('неверный пароль: понятное сообщение в окне', True)
        inputs.nth(4).fill('secret')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[0].path==="/smb/nas"', timeout=15000)
        check('после сохранения шара открывается в панели', pg.evaluate('()=>panels[0].view.map(e=>e.n).filter(n=>n!="..")') == ['folder', 'file.txt'] or
              sorted(pg.evaluate('()=>panels[0].view.map(e=>e.n).filter(n=>n!="..")')) == ['file.txt', 'folder'])
        pg.keyboard.press('n')
        pg.wait_for_selector('.dlg')
        check('подключение видно в списке без пароля', 'nas' in pg.locator('.dlg').inner_text() and 'secret' not in pg.locator('.dlg').inner_text())
        pg.keyboard.press('Escape')
        # копирование из шары в правую панель (локальную)
        pg.evaluate('()=>load(panels[1], "%s", null)' % ROOT)
        pg.wait_for_function('()=>panels[1].path.endsWith("/root")')
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="file.txt");render(p)}')
        pg.keyboard.press('F5')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[1].entries.some(e=>e.n==="file.txt")', timeout=15000)
        check('F5 с шары в локальную панель', open(ROOT + '/file.txt').read() == 'x')
        # Enter по папке в шаре, Backspace наверх
        pg.wait_for_function('()=>!overlay.classList.contains("on")', timeout=15000)
        pg.wait_for_timeout(800)  # дать завершиться обновлению панелей после задания
        pg.evaluate('()=>{const p=panels[0];p.cur=p.view.findIndex(e=>e.n==="folder");render(p)}')
        pg.keyboard.press('Enter')
        pg.wait_for_function('()=>panels[0].path==="/smb/nas/folder"')
        pg.keyboard.press('Backspace')
        pg.wait_for_function('()=>panels[0].path==="/smb/nas"')
        check('навигация внутри шары', True)
        # удаление подключения
        pg.keyboard.press('n')
        pg.wait_for_selector('.dlg')
        pg.get_by_role('button', name='Remove').first.click()
        pg.get_by_role('button', name='Remove').last.click()
        pg.wait_for_function('()=>/No network folders/.test((document.querySelector(".dlg")||{textContent:""}).textContent)', timeout=10000)
        check('удаление подключения', True)
        check('нет JS-ошибок', not errors, errors)
        br.close()
    post = urllib.request.Request('http://127.0.0.1:%d/quit' % PORT, b'{}', {'X-Requested-With': 'ps4cmd'}, method='POST')
    urllib.request.urlopen(post).read(); p.wait(timeout=10)
finally:
    for pr in (p, srv):
        if pr.poll() is None:
            pr.kill()
print('\nИТОГО: провалено %d' % len(fails))
sys.exit(1 if fails else 0)
