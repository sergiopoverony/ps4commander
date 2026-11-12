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


html = open(os.path.join(root, 'web', 'index.html'), encoding='utf-8').read().replace('__VERSION__', ver).encode('utf-8')
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'w') as f:
    f.write('// Сгенерировано tools/embed.py. Не редактировать.\n#pragma once\n')
    f.write(arr('ASSET_INDEX_HTML', html))
