#!/usr/bin/env python3
"""Генерирует build/assets_gen.h: веб-страница с подставленной версией из version.h."""
import re, sys, os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'build', 'assets_gen.h')
ver = re.search(r'APP_VERSION\s+"([^"]+)"', open(os.path.join(root, 'src', 'version.h')).read()).group(1)


def arr(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append(','.join(str(b) for b in data[i:i + 24]))
    return 'static const unsigned char %s[] = {\n%s\n};\nstatic const unsigned long %s_LEN = %d;\n' % (
        name, ',\n'.join(lines), name, len(data))


html = open(os.path.join(root, 'web', 'index.html'), encoding='utf-8').read().replace('__VERSION__', ver)
import base64
for ph, fn in (('__QR_RU__', 'qr_sbp.png'), ('__QR_EN__', 'qr_paypal.png')):
    html = html.replace(ph, 'data:image/png;base64,' + base64.b64encode(open(os.path.join(root, 'web', fn), 'rb').read()).decode())
html = html.encode('utf-8')
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'w') as f:
    f.write('// Сгенерировано tools/embed.py. Не редактировать.\n#pragma once\n')
    f.write(arr('ASSET_INDEX_HTML', html))

# языковые файлы lang/<код>.json; en первым, остальные по алфавиту
import glob, json
langs = sorted(glob.glob(os.path.join(root, 'lang', '*.json')), key=lambda x: (os.path.basename(x) != 'en.json', x))
index = []
with open(out, 'a') as f:
    for fn in langs:
        code = os.path.basename(fn)[:-5]
        assert re.fullmatch(r'[a-z]{2,8}(-[a-z0-9]{2,8})?', code), code
        raw = open(fn, 'rb').read()
        d = json.loads(raw.decode('utf-8'))
        index.append({'code': code, 'name': d.get('_name', code)})
        f.write(arr('ASSET_LANG_' + re.sub('[^A-Za-z0-9]', '_', code).upper(), raw))
    f.write(arr('ASSET_LANG_INDEX', json.dumps(index, ensure_ascii=False).encode('utf-8')))
    f.write('struct LangAsset { const char* code; const unsigned char* data; unsigned long len; };\n')
    f.write('static const LangAsset kLangAssets[] = {\n')
    for fn in langs:
        code = os.path.basename(fn)[:-5]
        n = 'ASSET_LANG_' + re.sub('[^A-Za-z0-9]', '_', code).upper()
        f.write('  {"%s", %s, %s_LEN},\n' % (code, n, n))
    f.write('  {nullptr, nullptr, 0}\n};\n')
