#!/usr/bin/env python3
"""Тест веб-API на ПК (HOST_TEST + ASAN). Запуск из корня проекта: python3 tests/test_api.py build/host_test
Все операции идут только внутри FM_ROOT (/tmp/fmtest/root): за его пределы сервер не выпускает."""
import json, os, re, shutil, socket, subprocess, sys, time, urllib.request, urllib.error, urllib.parse

BIN = sys.argv[1]
BASE = '/tmp/fmtest'
ROOT = BASE + '/root'
PORT = 18484
H = {'X-Requested-With': 'ps4cmd', 'Content-Type': 'application/json'}
fails = []
VER = re.search(r'APP_VERSION\s+"([^"]+)"', open('src/version.h').read()).group(1)


def check(name, cond, extra=''):
    print(('PASS ' if cond else 'FAIL ') + name + (' ' + str(extra) if not cond else ''))
    if not cond:
        fails.append(name)


def raw(path, data=None, headers=None, method=None):
    req = urllib.request.Request('http://127.0.0.1:%d%s' % (PORT, path), data=data, headers=headers or {}, method=method)
    try:
        r = urllib.request.urlopen(req, timeout=30)
        return r.status, r.read(), r.headers
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers


def get(path):
    st, body, hd = raw(path)
    try:
        return st, json.loads(body)
    except Exception:
        return st, body


def post(path, body=None, headers=H):
    st, b, _ = raw(path, json.dumps(body or {}).encode(), headers, 'POST')
    return st, json.loads(b or b'{}')


def q(p, **kw):
    return p + '?' + urllib.parse.urlencode(kw)


def wait_job(limit=60):
    t0 = time.time()
    while time.time() - t0 < limit:
        st, j = get('/api/job')
        if j['state'] not in ('scan', 'running'):
            return j
        time.sleep(0.1)
    raise SystemExit('задание не завершилось')


def start():
    shutil.rmtree(BASE, ignore_errors=True)
    shutil.rmtree('/tmp/ps4commander_test_data', ignore_errors=True)
    os.makedirs(ROOT + '/src/sub')
    os.makedirs(ROOT + '/dst')
    open(ROOT + '/src/a.txt', 'w').write('hello\n')
    open(ROOT + '/src/sub/b.bin', 'wb').write(os.urandom(300000))
    open(ROOT + '/src/Привет файл.txt', 'w').write('юникод\n')
    os.symlink('a.txt', ROOT + '/src/link')
    os.chmod(ROOT + '/src/a.txt', 0o640)
    env = dict(os.environ, FM_PORT=str(PORT), FM_ROOT=ROOT, FM_SLOW_MS='5', ASAN_OPTIONS='detect_leaks=0')
    p = subprocess.Popen([BIN], env=env, stdout=open(BASE + '/out.txt', 'w'), stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            get('/api/roots')
            return p
        except Exception:
            time.sleep(0.1)
    raise SystemExit('сервер не стартовал')


p = start()
try:
    st, j = get('/api/roots')
    check('roots: версия и корень', st == 200 and j['version'] == VER and j['roots'][0]['p'] == ROOT, j)
    st, body, hd = raw('/')
    check('страница отдаётся', st == 200 and b'PS4 Commander' in body and VER.encode() in body)

    # ----- список
    st, j = get(q('/api/ls', path=ROOT + '/src'))
    names = {e['n']: e for e in j['entries']}
    check('ls: файлы и папки', st == 200 and set(names) == {'a.txt', 'sub', 'link', 'Привет файл.txt'}, set(names))
    check('ls: типы', names['sub']['t'] == 'd' and names['a.txt']['t'] == 'f' and names['link']['t'] == 'l' and names['link']['l'] == 'a.txt')
    check('ls: права/размер', names['a.txt']['p'] == '640' and names['a.txt']['s'] == 6)
    check('ls: свободное место', j['free'] > 0 and j['total'] > 0 and j['parent'] == ROOT)
    st, j = get(q('/api/ls', path='/tmp'))
    check('ls: вне корня запрещён', st == 403 and j['error'] == 'outside_root', j)
    st, j = get(q('/api/ls', path=ROOT + '/../..'))
    check('ls: ../ не выпускает', st in (400, 403), (st, j))
    st, j = get(q('/api/ls', path=ROOT + '/nope'))
    check('ls: несуществующий -> ENOENT', st == 404 and j['error'] == 'ENOENT', j)
    st, j = get(q('/api/ls', path='relative'))
    check('ls: относительный путь', st == 400)

    # ----- защита запросов
    st, j = post('/api/mkdir', {'path': ROOT + '/x'}, headers={'Content-Type': 'application/json'})
    check('POST без заголовка -> 403', st == 403)
    st, b, _ = raw('/api/roots', headers={'Host': 'evil.example.com'})
    check('DNS rebinding: чужое имя в Host -> 403', st == 403)
    st, b, _ = raw('/api/roots', headers={'Host': '192.168.0.24:8484'})
    check('Host с IP разрешён', st == 200)

    # ----- mkdir / rename
    st, j = post('/api/mkdir', {'path': ROOT + '/newdir'})
    check('mkdir', st == 200 and os.path.isdir(ROOT + '/newdir'))
    st, j = post('/api/mkdir', {'path': ROOT + '/newdir'})
    check('mkdir существующий -> EEXIST', st == 400 and j['error'] == 'EEXIST', j)
    st, j = post('/api/mkdir', {'path': ROOT + '/a/b'})
    check('mkdir без родителя -> ENOENT', st == 404, j)
    st, j = post('/api/rename', {'path': ROOT + '/newdir', 'to': ROOT + '/renamed'})
    check('rename', st == 200 and os.path.isdir(ROOT + '/renamed') and not os.path.exists(ROOT + '/newdir'))
    st, j = post('/api/rename', {'path': ROOT + '/renamed', 'to': ROOT + '/src'})
    check('rename на существующее -> EEXIST', st == 400 and j['error'] == 'EEXIST', j)
    st, j = post('/api/rename', {'path': ROOT + '/renamed', 'to': ROOT + '/renamed/inner'})
    check('rename внутрь себя запрещён', st == 400 and j['error'] == 'into_self', j)
    st, j = post('/api/rename', {'path': ROOT + '/renamed', 'to': '/tmp/escape'})
    check('rename за пределы корня запрещён', st == 403, j)
    os.rmdir(ROOT + '/renamed')

    # ----- копирование (папка с вложенностью, ссылка, юникод)
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [ROOT + '/src'], 'dest': ROOT + '/dst'})
    check('copy: старт', st == 200, j)
    j = wait_job()
    check('copy: завершено без ошибок', j['state'] == 'done' and j['err_total'] == 0, j)
    check('copy: содержимое совпало', open(ROOT + '/dst/src/sub/b.bin', 'rb').read() == open(ROOT + '/src/sub/b.bin', 'rb').read())
    check('copy: юникод-имя', open(ROOT + '/dst/src/Привет файл.txt').read() == 'юникод\n')
    check('copy: ссылка осталась ссылкой', os.path.islink(ROOT + '/dst/src/link') and os.readlink(ROOT + '/dst/src/link') == 'a.txt')
    check('copy: права сохранены', (os.stat(ROOT + '/dst/src/a.txt').st_mode & 0o777) == 0o640)
    check('copy: счётчики', j['files_total'] >= 6 and j['done'] >= 300000, j)
    # повтор без overwrite: ошибки EEXIST, файлы целы
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [ROOT + '/src'], 'dest': ROOT + '/dst'})
    j = wait_job()
    check('copy повторно без overwrite: EEXIST', j['state'] == 'done_errors' and any(e['e'] == 'EEXIST' for e in j['errors']), j)
    open(ROOT + '/src/a.txt', 'w').write('changed\n')
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [ROOT + '/src'], 'dest': ROOT + '/dst', 'overwrite': True})
    j = wait_job()
    check('copy с overwrite перезаписывает', j['state'] == 'done' and open(ROOT + '/dst/src/a.txt').read() == 'changed\n', j)
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [ROOT + '/src'], 'dest': ROOT + '/src/sub'})
    check('copy папки в себя запрещён', st == 400 and j['error'] == 'into_self', j)
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [ROOT + '/src/a.txt'], 'dest': ROOT + '/src'})
    check('copy на то же место запрещён', st == 400 and j['error'] == 'same_path', j)
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [ROOT + '/src/a.txt'], 'dest': ROOT + '/src/a.txt'})
    check('dest не папка', st == 400 and j['error'] == 'bad_dest', j)
    st, j = post('/api/job/start', {'op': 'copy', 'paths': ['/etc/passwd'], 'dest': ROOT + '/dst'})
    check('copy источника вне корня запрещён', st == 403, j)
    st, j = post('/api/job/start', {'op': 'bogus', 'paths': [ROOT + '/src'], 'dest': ROOT})
    check('неизвестная операция', st == 400 and j['error'] == 'bad_op', j)

    # ----- перенос
    os.makedirs(ROOT + '/mv')
    st, j = post('/api/job/start', {'op': 'move', 'paths': [ROOT + '/dst/src'], 'dest': ROOT + '/mv'})
    j = wait_job()
    check('move', j['state'] == 'done' and os.path.isdir(ROOT + '/mv/src') and not os.path.exists(ROOT + '/dst/src'), j)

    # ----- размер
    st, j = post('/api/job/start', {'op': 'size', 'paths': [ROOT + '/mv/src']})
    j = wait_job()
    check('size: объекты и байты', j['state'] == 'done' and j['total'] >= 300000 and j['files_total'] >= 6, j)

    # ----- chmod
    st, j = post('/api/job/start', {'op': 'chmod', 'paths': [ROOT + '/mv/src'], 'mode': '755', 'recursive': True})
    j = wait_job()
    check('chmod рекурсивно', j['state'] == 'done' and (os.stat(ROOT + '/mv/src/sub/b.bin').st_mode & 0o777) == 0o755, j)
    st, j = post('/api/job/start', {'op': 'chmod', 'paths': [ROOT + '/mv/src'], 'mode': '9zz'})
    check('chmod: неверные права', st == 400 and j['error'] == 'bad_mode', j)

    # ----- просмотр и скачивание
    st, body, hd = raw(q('/api/view', path=ROOT + '/src/sub/b.bin', off=100, len=50))
    check('view: диапазон', st == 200 and body == open(ROOT + '/src/sub/b.bin', 'rb').read()[100:150] and hd['X-File-Size'] == '300000')
    st, body, hd = raw(q('/api/download', path=ROOT + '/src/sub/b.bin'))
    check('download: целиком', st == 200 and body == open(ROOT + '/src/sub/b.bin', 'rb').read() and 'attachment' in hd['Content-Disposition'])
    st, body, hd = raw(q('/api/download', path=ROOT + '/src/Привет файл.txt'))
    check('download: юникод в имени (RFC 5987)', 'filename*=UTF-8' in hd['Content-Disposition'] and '%D0%9F' in hd['Content-Disposition'])
    open(ROOT + '/pic.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + b'0' * 20)
    st, body, hd = raw(q('/api/download', path=ROOT + '/pic.png', inline='1'))
    check('download inline: картинка без attachment', hd['Content-Type'] == 'image/png' and 'Content-Disposition' not in hd)
    open(ROOT + '/page.html', 'w').write('<script>alert(1)</script>')
    st, body, hd = raw(q('/api/download', path=ROOT + '/page.html', inline='1'))
    check('download inline: HTML всё равно как вложение', 'attachment' in hd.get('Content-Disposition', '') and hd['Content-Type'] == 'application/octet-stream')
    st, body, hd = raw(q('/api/download', path='/etc/passwd'))
    check('download вне корня запрещён', st == 403)

    # ----- загрузка
    data = os.urandom(3 * 1024 * 1024 + 17)
    hdr = {'X-Requested-With': 'ps4cmd', 'Content-Type': 'application/octet-stream'}
    st, b, _ = raw(q('/api/upload', path=ROOT + '/up.bin'), data, hdr, 'POST')
    check('upload: 3 МБ', st == 200 and open(ROOT + '/up.bin', 'rb').read() == data and not os.path.exists(ROOT + '/up.bin.part'), (st, b))
    st, b, _ = raw(q('/api/upload', path=ROOT + '/up.bin'), b'x', hdr, 'POST')
    check('upload: существующий -> 409', st == 409)
    os.chmod(ROOT + '/up.bin', 0o600)
    st, b, _ = raw(q('/api/upload', path=ROOT + '/up.bin', overwrite='1'), b'new', hdr, 'POST')
    check('upload overwrite: содержимое и права', st == 200 and open(ROOT + '/up.bin', 'rb').read() == b'new' and (os.stat(ROOT + '/up.bin').st_mode & 0o777) == 0o600)
    st, b, _ = raw(q('/api/upload', path=ROOT + '/empty.txt'), b'', hdr, 'POST')
    check('upload: пустой файл', st == 200 and os.path.getsize(ROOT + '/empty.txt') == 0)
    st, b, _ = raw(q('/api/upload', path='/tmp/evil.bin'), b'x', hdr, 'POST')
    check('upload вне корня запрещён', st == 403 and not os.path.exists('/tmp/evil.bin'))
    st, b, _ = raw(q('/api/upload', path=ROOT + '/nocsrf.bin'), b'x', {'Content-Type': 'application/octet-stream'}, 'POST')
    check('upload без заголовка -> 403', st == 403 and not os.path.exists(ROOT + '/nocsrf.bin'))

    # ----- поиск
    st, j = get(q('/api/find', path=ROOT, q='B.BIN'))
    check('find: подстрока без регистра', st == 200 and any(r['p'].endswith('/b.bin') for r in j['results']), j)
    st, j = get(q('/api/find', path=ROOT, q='*.txt'))
    check('find: маска', st == 200 and len(j['results']) >= 3 and all(r['p'].endswith('.txt') for r in j['results']), j)
    st, j = get(q('/api/find', path=ROOT, q='привет'))
    check('find: кириллица (регистр только ASCII)', st == 200 and isinstance(j['results'], list))
    st, j = get(q('/api/find', path=ROOT, q=''))
    check('find: пустой запрос', st == 400)
    st, j = get(q('/api/find', path='/tmp', q='x'))
    check('find вне корня запрещён', st == 403)

    # ----- удаление
    st, j = post('/api/job/start', {'op': 'delete', 'paths': [ROOT + '/mv', ROOT + '/dst', ROOT + '/up.bin']})
    j = wait_job()
    check('delete: деревья и файл', j['state'] == 'done' and not os.path.exists(ROOT + '/mv') and not os.path.exists(ROOT + '/up.bin'), j)
    st, j = post('/api/job/start', {'op': 'delete', 'paths': [ROOT + '/nonexistent']})
    j = wait_job()
    check('delete несуществующего: ошибка ENOENT', j['state'] == 'done_errors' and j['errors'][0]['e'] == 'ENOENT', j)
    st, j = post('/api/job/start', {'op': 'delete', 'paths': ['/']})
    check('delete корня запрещён', st == 403, j)
    st, j = post('/api/job/start', {'op': 'delete', 'paths': ['/tmp']})
    check('delete вне корня запрещён', st == 403, j)

    # ----- отмена и занятость
    big = ROOT + '/big.bin'
    with open(big, 'wb') as f:
        f.write(os.urandom(1 << 20) * 400)   # 400 МБ
    os.makedirs(ROOT + '/cp')
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [big], 'dest': ROOT + '/cp'})
    check('big copy: старт', st == 200)
    st2, j2 = post('/api/job/start', {'op': 'delete', 'paths': [ROOT + '/cp']})
    check('второе задание -> busy (409)', st2 == 409 and j2['error'] == 'busy', (st2, j2))
    time.sleep(0.3)
    post('/api/job/cancel')
    j = wait_job()
    check('отмена: состояние cancelled', j['state'] == 'cancelled', j)
    check('отмена: недокопированный файл удалён', not os.path.exists(ROOT + '/cp/big.bin'))
    shutil.rmtree(ROOT + '/cp'); os.unlink(big)

    # ----- завершение
    # запоминание панелей
    st, j = get('/api/state')
    check('state: изначально пусто', st == 200 and j.get('l', '') == '' and j.get('r', '') == '', j)
    st, j = post('/api/state', {'l': ROOT + '/src', 'r': ROOT + '/dst'})
    check('state: сохранено', st == 200)
    st, j = post('/api/state', {'l': ROOT + '/src/sub'})
    st, j = get('/api/state')
    check('state: поле r сохраняется при записи только l', j == {'l': ROOT + '/src/sub', 'r': ROOT + '/dst'}, j)
    st, j = post('/api/state', {'l': ROOT + '/a/../src'})
    st, j = get('/api/state')
    check('state: путь нормализуется', j['l'] == ROOT + '/src', j)
    st, _, _ = raw('/api/state', b'{}', {}, 'POST')
    check('state: POST без заголовка запрещён', st in (400, 403), st)
    st, j = get('/api/roots')
    check('roots: есть флаг mnt', all('mnt' in r for r in j['roots']), j)

    # скачивание папки одним ZIP
    import zipfile, io
    Z = ROOT + '/zsrc'
    os.makedirs(Z + '/top/empty'); os.makedirs(Z + '/top/вложенная')
    open(Z + '/top/a.txt', 'w').write('alpha\n')
    open(Z + '/top/вложенная/файл ё.bin', 'wb').write(os.urandom(700000))
    open(Z + '/top/zero.dat', 'wb').close()
    os.symlink('a.txt', Z + '/top/lnk')
    open(Z + '/single.txt', 'w').write('one')
    st, body, hd = raw('/api/zip?dir=%s&n=top' % urllib.parse.quote(Z))
    check('zip: Content-Length совпадает с телом', st == 200 and int(hd['Content-Length']) == len(body), (st, hd.get('Content-Length'), len(body)))
    check('zip: имя файла в заголовке', "top.zip" in hd['Content-Disposition'])
    zf = zipfile.ZipFile(io.BytesIO(body))
    check('zip: контрольные суммы верны', zf.testzip() is None)
    names = sorted(zf.namelist())
    check('zip: состав (папки, юникод, без ссылки)', names == sorted(['top/', 'top/a.txt', 'top/empty/', 'top/zero.dat', 'top/вложенная/', 'top/вложенная/файл ё.bin']), names)
    check('zip: содержимое совпадает', zf.read('top/вложенная/файл ё.bin') == open(Z + '/top/вложенная/файл ё.bin', 'rb').read() and zf.read('top/a.txt') == b'alpha\n')
    check('zip: права сохранены', (zf.getinfo('top/a.txt').external_attr >> 16) & 0o777 == os.stat(Z + '/top/a.txt').st_mode & 0o777)
    st, body, hd = raw('/api/zip?dir=%s&n=top&n=single.txt' % urllib.parse.quote(Z))
    zf = zipfile.ZipFile(io.BytesIO(body))
    check('zip: несколько имён', zf.testzip() is None and 'single.txt' in zf.namelist() and 'top/a.txt' in zf.namelist() and 'zsrc.zip' in hd['Content-Disposition'])
    st, body, _ = raw('/api/zip?dir=%s&n=..' % urllib.parse.quote(Z))
    check('zip: имя с .. отклонено', st == 400, st)
    st, body, _ = raw('/api/zip?dir=%s&n=nope' % urllib.parse.quote(Z))
    check('zip: нет такого файла', st in (403, 404), st)
    st, body, _ = raw('/api/zip?dir=%s&n=x' % urllib.parse.quote('/etc'))
    check('zip: вне корня отклонено', st in (403, 404), st)
    if os.environ.get('ZIP64'):
        with open(Z + '/huge.bin', 'wb') as hf: hf.truncate((4 << 30) + 12345)
        st, body, hd = raw('/api/zip?dir=%s&n=huge.bin' % urllib.parse.quote(Z))
        zf = zipfile.ZipFile(io.BytesIO(body))
        check('zip64: файл > 4 ГБ', int(hd['Content-Length']) == len(body) and zf.testzip() is None and zf.getinfo('huge.bin').file_size == (4 << 30) + 12345)
        os.remove(Z + '/huge.bin')

    # обрыв соединения клиентом посреди скачивания/загрузки не должен ронять службу (SIGPIPE)
    import socket, struct
    open(ROOT + '/big.bin', 'wb').write(os.urandom(8 << 20))
    def abort_download():
        c = socket.create_connection(('127.0.0.1', PORT))
        c.sendall(('GET /api/download?path=%s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' % urllib.parse.quote(ROOT + '/big.bin')).encode())
        c.recv(65536)
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        c.close()
    def abort_upload():
        c = socket.create_connection(('127.0.0.1', PORT))
        c.sendall(('POST /api/upload?path=%s HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Requested-With: ps4cmd\r\nContent-Length: 5000000\r\n\r\n' % urllib.parse.quote(ROOT + '/aborted.bin')).encode() + b'x' * 100000)
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        c.close()
    for _ in range(6):
        abort_download(); abort_upload()
    time.sleep(1)
    check('обрыв скачивания/загрузки: служба жива', p.poll() is None and get('/api/roots')[0] == 200)
    check('обрыв загрузки: неполный файл не остался', not os.path.exists(ROOT + '/aborted.bin'), os.listdir(ROOT))

    # установка pkg (заглушка BGFT)
    def mkpkg(name, cid='UP0000-CUSA00000_00-TESTTESTTESTTEST', magic=b'\x7fCNT'):
        h = bytearray(0x200)
        h[0:4] = magic
        h[0x40:0x40 + 36] = cid.encode()
        open(ROOT + '/' + name, 'wb').write(bytes(h))
    mkpkg('good.pkg'); mkpkg('bad.pkg', magic=b'XXXX'); mkpkg('dup.pkg'); mkpkg('fail.pkg')
    open(ROOT + '/note.txt', 'w').write('x')
    st, j = post('/api/pkg/install', {'paths': [ROOT + '/note.txt']})
    check('pkg: не .pkg отклонён', st == 400 and j.get('error') == 'not_pkg', (st, j))
    st, j = post('/api/pkg/install', {'paths': [ROOT + '/bad.pkg']})
    check('pkg: неверный заголовок отклонён', st == 400 and j.get('error') == 'not_pkg', (st, j))
    st, j = post('/api/pkg/install', {'paths': ['/etc/x.pkg']})
    check('pkg: путь вне корня отклонён', st in (400, 403, 404), (st, j))
    st, j = raw('/api/pkg/install', b'{}', {}, 'POST')[0], None
    check('pkg: POST без заголовка запрещён', st in (400, 403), st)
    st, j = post('/api/pkg/install', {'paths': [ROOT + '/good.pkg', ROOT + '/dup.pkg', ROOT + '/fail.pkg']})
    check('pkg: поставлено в очередь', st == 200, (st, j))
    t0 = time.time()
    while time.time() - t0 < 30:
        st, j = get('/api/pkg')
        if not j['active']: break
        time.sleep(0.2)
    sts = {i['n']: i['st'] for i in j['items']}
    check('pkg: статусы done/already/failed', sts == {'good.pkg': 'done', 'dup.pkg': 'already_installed', 'fail.pkg': 'failed'}, sts)
    st, j = post('/quit')
    check('quit', st == 200)
    try:
        p.wait(timeout=10)
        check('процесс завершился', p.returncode == 0, p.returncode)
    except subprocess.TimeoutExpired:
        check('процесс завершился', False)
finally:
    if p.poll() is None:
        p.kill()

out = open(BASE + '/out.txt').read()
check('ASAN/UBSAN: нет ошибок', 'ERROR: AddressSanitizer' not in out and 'runtime error' not in out, out[-500:])
print('\nИТОГО: провалено %d' % len(fails))
sys.exit(1 if fails else 0)
