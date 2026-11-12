#!/usr/bin/env python3
"""Проверки SMB-клиента против тестового сервера impacket (порт 445, нужен root). Запуск: python3 tests/test_smb.py build/host_test"""
import json, os, shutil, socket, subprocess, sys, time, urllib.error, urllib.parse, urllib.request

BIN = sys.argv[1] if len(sys.argv) > 1 else 'build/host_test'
BASE = '/tmp/fmsmb'
ROOT = BASE + '/root'
SHARE = BASE + '/share'
PORT = 18997
fails = []
H = {'X-Requested-With': 'ps4cmd', 'Content-Type': 'application/json'}


def check(name, cond, extra=''):
    print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else ' ' + str(extra)))
    if not cond:
        fails.append(name)


def raw(path, data=None, headers=None, method=None):
    req = urllib.request.Request('http://127.0.0.1:%d%s' % (PORT, path), data=data, headers=headers or {}, method=method)
    try:
        r = urllib.request.urlopen(req, timeout=60)
        return r.status, r.read(), r.headers
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers


def get(path):
    st, b, _ = raw(path)
    try:
        return st, json.loads(b)
    except Exception:
        return st, b


def post(path, body=None):
    st, b, _ = raw(path, json.dumps(body or {}).encode(), H, 'POST')
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


def port_free(p):
    s = socket.socket()
    try:
        s.bind(('127.0.0.1', p)); return True
    except OSError:
        return False
    finally:
        s.close()


if not port_free(445):
    print('SKIP: порт 445 занят или нет прав (нужен root)'); sys.exit(0)
try:
    import impacket  # noqa
except ImportError:
    print('SKIP: нет impacket'); sys.exit(0)

shutil.rmtree(BASE, ignore_errors=True)
shutil.rmtree('/tmp/ps4commander_test_data', ignore_errors=True)
os.makedirs(ROOT); os.makedirs(SHARE + '/sub/deep'); os.makedirs(SHARE + '/empty')
open(SHARE + '/a.txt', 'w').write('hello smb\n')
open(SHARE + '/Привет.txt', 'w').write('юникод\n')
BIG = os.urandom(3 * 1024 * 1024 + 123)
open(SHARE + '/sub/big.bin', 'wb').write(BIG)
open(SHARE + '/sub/deep/x.pkg', 'wb').write(b'\x7fCNT' + b'\0' * 60 + b'UP0000-CUSA00000_00-SMBSMBSMBSMBSMBS' + b'\0' * 100)

srv = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'smb_server.py'), SHARE, '445'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    try:
        socket.create_connection(('127.0.0.1', 445), timeout=0.2).close(); break
    except OSError:
        time.sleep(0.1)
env = dict(os.environ, FM_PORT=str(PORT), FM_ROOT=ROOT, ASAN_OPTIONS='detect_leaks=0')
p = subprocess.Popen([BIN], env=env, stdout=open(BASE + '/out.txt', 'w'), stderr=subprocess.STDOUT)
for _ in range(50):
    try:
        get('/api/roots'); break
    except Exception:
        time.sleep(0.1)
try:
    conn = {'alias': 'nas', 'host': '127.0.0.1', 'share': 'DATA', 'user': 'test', 'pass': 'secret'}
    st, j = post('/api/smb/save', dict(conn, pass_='x', **{'pass': 'wrong'}))
    check('smb: неверный пароль отклонён', st == 400 and j.get('error') == 'smb_auth', (st, j))
    st, j = post('/api/smb/save', dict(conn, alias='bad name!'))
    check('smb: плохое имя отклонено', st == 400 and j.get('error') == 'bad_alias', (st, j))
    st, j = post('/api/smb/save', dict(conn, host='127.0.0.1:1'))
    check('smb: недоступный сервер -> ошибка', st == 400 and j.get('error') in ('ECONNREFUSED', 'ETIMEDOUT', 'EIO'), (st, j))
    st, j = post('/api/smb/scan')
    check('поиск: запущен', st == 200, (st, j))
    for _ in range(50):
        st, j = get('/api/smb/scan')
        if not j['running']: break
        time.sleep(0.1)
    check('поиск: найден тестовый сервер', st == 200 and not j['running'] and j['found'] == ['127.0.0.1'] and j['done'] == j['total'] == 1, j)
    st, j = post('/api/smb/shares', dict(conn, **{'pass': 'wrong'}))
    check('папки: неверный пароль', st == 400 and j.get('error') == 'smb_auth', (st, j))
    st, j = post('/api/smb/shares', conn)
    check('папки: список общих папок сервера', st == 200 and j.get('shares') == ['DATA'], (st, j))
    st, j = post('/api/smb/shares', {'host': 'bad host!'})
    check('папки: плохой адрес отклонён', st == 400 and j.get('error') == 'bad_host', (st, j))
    st, j = post('/api/smb/save', conn)
    check('smb: подключение сохранено', st == 200, (st, j))
    st, j = get('/api/smb')
    check('smb: список без пароля', st == 200 and j['conns'][0]['alias'] == 'nas' and '"pass"' not in json.dumps(j) and j['conns'][0]['has_pass'] is True, j)
    check('smb: файл настроек не читается всеми', (os.stat('/tmp/ps4commander_test_data/smb.json').st_mode & 0o077) == 0)
    st, j = get('/api/roots')
    check('smb: шара в списке корней', any(r['p'] == '/smb/nas' for r in j['roots']), j)
    st, j = get('/api/ls?path=/smb')
    check('smb: /smb показывает подключения', st == 200 and [e['n'] for e in j['entries']] == ['nas'], j)
    st, j = get(q('/api/ls', path='/smb/nas'))
    names = {e['n']: e for e in j['entries']}
    check('smb: список шары', st == 200 and set(names) == {'a.txt', 'Привет.txt', 'sub', 'empty'} and names['sub']['t'] == 'd' and names['a.txt']['s'] == 10, j)
    st, body, _ = raw(q('/api/view', path='/smb/nas/a.txt', off=0, len=100))
    check('smb: просмотр файла', st == 200 and body == b'hello smb\n', body)
    st, body, hd = raw(q('/api/download', path='/smb/nas/sub/big.bin'))
    check('smb: скачивание большого файла', st == 200 and body == BIG)
    st, body, hd = raw(q('/api/download', path='/smb/nas/Привет.txt'))
    check('smb: юникод в имени', body == 'юникод\n'.encode())
    st, j = post('/api/mkdir', {'path': '/smb/nas/newdir'})
    check('smb: создание папки', st == 200 and os.path.isdir(SHARE + '/newdir'), (st, j))
    st, j = post('/api/rename', {'path': '/smb/nas/newdir', 'to': '/smb/nas/renamed'})
    check('smb: переименование', st == 200 and os.path.isdir(SHARE + '/renamed') and not os.path.exists(SHARE + '/newdir'), (st, j))
    # копирование шара -> локально
    os.makedirs(ROOT + '/dst')
    st, j = post('/api/job/start', {'op': 'copy', 'paths': ['/smb/nas/sub', '/smb/nas/a.txt'], 'dest': ROOT + '/dst'})
    j = wait_job()
    check('smb->локально: копирование', j['state'] == 'done' and open(ROOT + '/dst/sub/big.bin', 'rb').read() == BIG and open(ROOT + '/dst/a.txt').read() == 'hello smb\n', j)
    # локально -> шара
    open(ROOT + '/up.bin', 'wb').write(BIG[:1500000])
    os.makedirs(ROOT + '/tree/in'); open(ROOT + '/tree/in/f.txt', 'w').write('f')
    st, j = post('/api/job/start', {'op': 'copy', 'paths': [ROOT + '/up.bin', ROOT + '/tree'], 'dest': '/smb/nas/empty'})
    j = wait_job()
    check('локально->smb: копирование', j['state'] == 'done' and open(SHARE + '/empty/up.bin', 'rb').read() == BIG[:1500000] and os.path.exists(SHARE + '/empty/tree/in/f.txt'), j)
    # перенос между шарой и локальной (с удалением источника)
    st, j = post('/api/job/start', {'op': 'move', 'paths': [ROOT + '/up.bin'], 'dest': '/smb/nas/renamed'})
    j = wait_job()
    check('локально->smb: перенос', j['state'] == 'done' and os.path.exists(SHARE + '/renamed/up.bin') and not os.path.exists(ROOT + '/up.bin'), j)
    # перенос внутри шары (rename на сервере)
    st, j = post('/api/job/start', {'op': 'move', 'paths': ['/smb/nas/renamed/up.bin'], 'dest': '/smb/nas/sub/deep'})
    j = wait_job()
    check('smb: перенос внутри шары', j['state'] == 'done' and os.path.exists(SHARE + '/sub/deep/up.bin') and not os.path.exists(SHARE + '/renamed/up.bin'), j)
    # размер
    st, j = post('/api/job/start', {'op': 'size', 'paths': ['/smb/nas/sub']})
    j = wait_job()
    check('smb: размер папки', j['total'] >= len(BIG), j)
    # загрузка через /api/upload
    st, b, _ = raw(q('/api/upload', path='/smb/nas/empty/up2.txt'), b'uploaded', {'X-Requested-With': 'ps4cmd'}, 'POST')
    check('smb: загрузка файла', st == 200 and open(SHARE + '/empty/up2.txt', 'rb').read() == b'uploaded', (st, b))
    st, b, _ = raw(q('/api/upload', path='/smb/nas/empty/up2.txt'), b'x', {'X-Requested-With': 'ps4cmd'}, 'POST')
    check('smb: загрузка поверх существующего без overwrite = 409', st == 409, st)
    st, b, _ = raw(q('/api/upload', path='/smb/nas/empty/up2.txt', overwrite='1'), b'second', {'X-Requested-With': 'ps4cmd'}, 'POST')
    check('smb: перезапись файла', st == 200 and open(SHARE + '/empty/up2.txt', 'rb').read() == b'second', (st, b))
    check('smb: временных .part нет', not [f for f in os.listdir(SHARE + '/empty') if f.endswith('.part')], os.listdir(SHARE + '/empty'))
    # поиск
    st, j = get(q('/api/find', path='/smb/nas', q='*.pkg'))
    check('smb: поиск', st == 200 and [r['p'] for r in j['results']] == ['/smb/nas/sub/deep/x.pkg'], j)
    # zip
    import zipfile, io
    st, body, hd = raw(q('/api/zip', dir='/smb/nas', n='sub'))
    zf = zipfile.ZipFile(io.BytesIO(body))
    check('smb: ZIP папки', int(hd['Content-Length']) == len(body) and zf.testzip() is None and zf.read('sub/big.bin') == BIG, (hd['Content-Length'], len(body)))
    # удаление
    st, j = post('/api/job/start', {'op': 'delete', 'paths': ['/smb/nas/empty', '/smb/nas/renamed']})
    j = wait_job()
    check('smb: удаление папок', j['state'] == 'done' and not os.path.exists(SHARE + '/empty') and not os.path.exists(SHARE + '/renamed'), j)
    # защита
    st, j = post('/api/job/start', {'op': 'delete', 'paths': ['/smb/nas']})
    check('smb: корень шары нельзя удалить', st == 403 and j.get('error') == 'protected', (st, j))
    st, j = post('/api/rename', {'path': '/smb/nas', 'to': '/smb/zzz'})
    check('smb: корень шары нельзя переименовать', st == 403, (st, j))
    st, j = post('/api/pkg/install', {'paths': ['/smb/nas/sub/deep/x.pkg']})
    check('smb: .pkg с шары ставится в очередь', st == 200, (st, j))
    t0 = time.time()
    while time.time() - t0 < 20:
        st, j = get('/api/pkg')
        if not j['active']: break
        time.sleep(0.2)
    log = open(BASE + '/out.txt').read()
    log = open(BASE + '/out.txt').read()
    check('smb: без FUSE установка не копирует файл, а сообщает причину', [i['st'] for i in j['items']] == ['failed'] and j['items'][0]['msg'] == 'fuse: no_fw' and 'stage' not in log and 'копирование' not in log, (j, log[-300:]))
    check('smb: временных копий нет', not os.path.exists('/tmp/ps4commander_test_data/stage'))
    check('smb: /p/ нет', get('/p/UP0000-CUSA00000_00-SMBSMBSMBSMBSMBS')[0] == 404)
    # связанные пакеты: патчи, DLC, русификатор рядом и в соседних папках
    import struct
    def mkpkg(rel, cid, ctype, flags, title, ver, pkg_size=None, extra=b''):
        def psf():
            ents = [(b'APP_VER', ver.encode() + b'\0'), (b'TITLE', title.encode() + b'\0')]
            keys = b''.join(k + b'\0' for k, _ in ents)
            kofs, dofs, cnt = 20 + 16 * len(ents), 0, len(ents)
            tab, data, ko, do = b'', b'', 0, 0
            for k, v in ents:
                tab += struct.pack('<HHIII', ko, 0x204, len(v), len(v), do)
                ko += len(k) + 1; data += v; do += len(v)
            dofs = kofs + len(keys)
            return b'\0PSF' + struct.pack('<IIII', 0x101, kofs, dofs, cnt) + tab + keys + data
        sfo = psf()
        buf = bytearray(0x1000 + len(extra))
        buf[0:4] = b'\x7fCNT'
        buf[0x10:0x14] = struct.pack('>I', 1)
        buf[0x18:0x1c] = struct.pack('>I', 0x800)
        buf[0x40:0x40 + 36] = cid.encode()
        buf[0x74:0x78] = struct.pack('>I', ctype)
        buf[0x78:0x7c] = struct.pack('>I', flags)
        buf[0x430:0x438] = struct.pack('>Q', pkg_size if pkg_size else len(buf))
        buf[0x800:0x820] = struct.pack('>IIIIIIII', 0x1000, 0, 0, 0, 0xA00, len(sfo), 0, 0)
        buf[0xA00:0xA00 + len(sfo)] = sfo
        os.makedirs(os.path.dirname(SHARE + rel), exist_ok=True)
        open(SHARE + rel, 'wb').write(bytes(buf))
    C = 'UP0000-CUSA77777_00-TESTGAMETESTGAME'
    mkpkg('/lib/game/main/Game.pkg', C, 0x1A, 0, 'Test Game', '01.00', extra=b'\1' * 5000)
    mkpkg('/lib/game/patch/Patch_101.pkg', C, 0x1A, 0x40000000, 'Test Game', '01.01', extra=b'\2' * 3000)
    mkpkg('/lib/game/patch/Patch_102.pkg', C, 0x1A, 0x40000000, 'Test Game', '01.02', extra=b'\3' * 3100)
    mkpkg('/lib/game/patch/Patch_103_broken.pkg', C, 0x1A, 0x40000000, 'Test Game', '01.03', pkg_size=10 ** 6, extra=b'\4' * 100)
    mkpkg('/lib/game/dlc/Pack.pkg', 'UP0000-CUSA77777_00-TESTDLCTESTDLC00', 0x1B, 0, 'Test DLC', '01.00', extra=b'\5' * 2000)
    mkpkg('/lib/game/rus/Russian.pkg', 'UP0000-CUSA77777_00-TESTRUSTESTRUS00', 0x1A, 0, 'Test Game RUS', '01.00', extra=b'\6' * 1500)
    mkpkg('/lib/game/other/Other.pkg', 'UP0000-CUSA88888_00-OTHERGAMEOTHERG0', 0x1A, 0, 'Other Game', '01.00', extra=b'\7' * 800)
    open(SHARE + '/lib/game/other/readme.txt', 'w').write('x')
    st, j = get('/api/pkg/related?path=' + urllib.parse.quote('/smb/nas/lib/game/main/Game.pkg'))
    names = [i['n'] for i in j.get('items', [])] if st == 200 else []
    check('pkg: найдены связанные, порядок игра-патч-DLC-русификатор', names[:4] == ['Game.pkg', 'Patch_102.pkg', 'Pack.pkg', 'Russian.pkg'], (st, names))
    byname = {i['n']: i for i in j.get('items', [])}
    check('pkg: ставятся по умолчанию основа, новейший патч, DLC, русификатор', all(byname[n]['c'] for n in names[:4]), j)
    check('pkg: старый патч пропущен с причиной', byname['Patch_101.pkg']['c'] is False and byname['Patch_101.pkg']['why'] == 'newer_patch:1.02', byname.get('Patch_101.pkg'))
    check('pkg: недокачанный пропущен', byname['Patch_103_broken.pkg']['c'] is False and byname['Patch_103_broken.pkg']['why'] == 'incomplete', byname.get('Patch_103_broken.pkg'))
    check('pkg: чужая игра не попала в список', 'Other.pkg' not in byname, names)
    check('pkg: тип и версия из заголовка', byname['Game.pkg']['k'] == 'game' and byname['Patch_102.pkg']['k'] == 'patch' and byname['Patch_102.pkg']['v'] == '01.02' and byname['Pack.pkg']['k'] == 'dlc' and j['title'] == 'Test Game' and j['tid'] == 'CUSA77777', j)
    st, j2 = get('/api/pkg/related?path=' + urllib.parse.quote('/smb/nas/lib/game/patch/Patch_101.pkg'))
    b2 = {i['n']: i for i in j2.get('items', [])}
    check('pkg: выбранный пакет ставится всегда, даже если он старый патч', st == 200 and b2['Patch_101.pkg']['c'] and b2['Patch_101.pkg']['self'], j2)
    st, j3 = get('/api/pkg/related?path=' + urllib.parse.quote('/smb/nas/lib/game/other/Other.pkg'))
    check('pkg: у игры без связанных один пакет', st == 200 and [i['n'] for i in j3['items']] == ['Other.pkg'], j3)
    st, j4 = get('/api/pkg/related?path=' + urllib.parse.quote('/smb/nas/lib/game/other/readme.txt'))
    check('pkg: не pkg - пустой список, без ошибки', st == 200 and j4['items'] == [], (st, j4))
    # операции FUSE-папки (то, что видит системный установщик)
    st, j = get('/api/_nf?op=ls&path=/')
    check('fuse: корень - список шар', st == 200 and j['r'] == 0 and 'nas' in j['n'], j)
    st, j = get('/api/_nf?op=stat&path=/nas')
    check('fuse: шара - каталог', j['r'] == 0 and j['dir'], j)
    st, j = get('/api/_nf?op=stat&path=/nope')
    check('fuse: нет такой шары - ENOENT', j['r'] == -2, j)
    st, j = get('/api/_nf?op=ls&path=/nas/sub')
    check('fuse: список папки', j['r'] == 0 and 'big.bin' in j['n'], j)
    st, j = get('/api/_nf?op=stat&path=/nas/sub/big.bin')
    check('fuse: размер файла', j['r'] == 0 and not j['dir'] and j['size'] == len(BIG), j)
    st, j = get('/api/_nf?op=read&path=/nas/sub/big.bin&off=1000&n=5000')
    check('fuse: чтение из середины', j['r'] == 0 and bytes.fromhex(j['hex']) == BIG[1000:6000], j.get('r'))
    st, j = get('/api/_nf?op=read&path=/nas/sub/big.bin&off=%d&n=100000' % (len(BIG) - 50))
    check('fuse: чтение у конца файла не выходит за размер', j['r'] == 0 and bytes.fromhex(j['hex']) == BIG[-50:], j.get('r'))
    st, j = get('/api/_nf?op=read&path=/nas/sub/big.bin&off=1048000&n=2000')
    check('fuse: чтение через границу окна', j['r'] == 0 and bytes.fromhex(j['hex']) == BIG[1048000:1050000], j.get('r'))
    st, j = get('/api/_nf?op=stat&path=/nas/../etc/passwd')
    check('fuse: .. запрещено', j['r'] == -13, j)
    # Range: нужен для BGFT и перемотки видео
    st, body, hd = raw(q('/api/download', path='/smb/nas/sub/big.bin'), headers={'Range': 'bytes=1000-1999'})
    check('Range: 206 и нужные байты', st == 206 and body == BIG[1000:2000] and hd['Content-Range'] == 'bytes 1000-1999/%d' % len(BIG), (st, hd.get('Content-Range')))
    st, body, hd = raw(q('/api/download', path='/smb/nas/sub/big.bin'), headers={'Range': 'bytes=-100'})
    check('Range: последние байты', st == 206 and body == BIG[-100:])
    st, body, hd = raw(q('/api/download', path='/smb/nas/sub/big.bin'), headers={'Range': 'bytes=%d-' % (len(BIG) + 5)})
    check('Range: за пределами файла = 416', st == 416, st)
    st, body, hd = raw(q('/api/download', path='/smb/nas/sub/big.bin'), method='HEAD')
    check('HEAD: размер без тела', st == 200 and body == b'' and int(hd['Content-Length']) == len(BIG) and hd['Accept-Ranges'] == 'bytes', (st, dict(hd)))
    # несуществующее
    st, j = get(q('/api/ls', path='/smb/nas/nope'))
    check('smb: нет такой папки -> ENOENT', st == 404 and j.get('error') == 'ENOENT', (st, j))
    st, j = get(q('/api/ls', path='/smb/nope'))
    check('smb: нет такого подключения -> ENOENT', st == 404, (st, j))
    # обрыв клиентом посреди скачивания и дальше всё работает (пул подключений)
    for _ in range(4):
        c = socket.create_connection(('127.0.0.1', PORT))
        c.sendall(('GET /api/download?path=%s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' % urllib.parse.quote('/smb/nas/sub/big.bin')).encode())
        c.recv(65536); c.close()
    time.sleep(1)
    st, j = get(q('/api/ls', path='/smb/nas'))
    check('smb: после обрыва скачивания всё работает', st == 200, (st, j))
    # перезапуск службы: подключение восстанавливается из файла
    post('/quit'); p.wait(timeout=10)
    p = subprocess.Popen([BIN], env=env, stdout=open(BASE + '/out2.txt', 'w'), stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            get('/api/roots'); break
        except Exception:
            time.sleep(0.1)
    st, j = get(q('/api/ls', path='/smb/nas'))
    check('smb: подключение сохранилось после перезапуска', st == 200 and any(e['n'] == 'a.txt' for e in j['entries']), (st, j))
    st, j = post('/api/smb/del', {'alias': 'nas'})
    st2, j2 = get('/api/smb')
    check('smb: удаление подключения', st == 200 and j2['conns'] == [], (st, j2))
    post('/quit'); p.wait(timeout=10)
finally:
    for pr in (p, srv):
        if pr.poll() is None:
            pr.kill()
out = open(BASE + '/out.txt').read() + open(BASE + '/out2.txt').read() if os.path.exists(BASE + '/out2.txt') else open(BASE + '/out.txt').read()
check('ASAN/UBSAN: нет ошибок', 'ERROR: AddressSanitizer' not in out and 'runtime error' not in out, out[-800:])
print('\nИТОГО: провалено %d' % len(fails))
sys.exit(1 if fails else 0)
