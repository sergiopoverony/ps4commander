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
    check('smb: установка с шары завершилась', [i['st'] for i in j['items']] == ['done'], j)
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
