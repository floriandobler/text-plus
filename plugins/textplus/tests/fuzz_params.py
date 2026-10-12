#!/usr/bin/env python3
"""Robustness test: random parameter combinations through the headless host.

Every run must finish without crash, GL error or GL state leak.
Usage: tests/fuzz_params.py build/ffgl_test_host build/TextPlus.so [runs]
"""
import random, re, subprocess, sys, tempfile, os

host, plugin = sys.argv[1], sys.argv[2]
runs = int(sys.argv[3]) if len(sys.argv) > 3 else 40
random.seed(1234)

params = []
for line in subprocess.run([host, plugin, '--list'], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\d+\s+(.+?)\s{2,}(.+?)\s+type (\d+)\s+default (\S+)(?:\s+range \[(\S+), (\S+)\])?', line)
    if m:
        group, name, typ = m.group(1), m.group(2).strip(), int(m.group(3))
        lo = float(m.group(5)) if m.group(5) else 0.0
        hi = float(m.group(6)) if m.group(6) else 1.0
        params.append((name, typ, lo, hi))

texts = ['', ' ', 'A', 'Hello World', 'Ünïcödé Größe ß', 'emoji 😀 and 中文 missing', 'a|b|c|d|e|f',
         'x' * 600, 'Tab\there', '{#ff0000}red{/} {{literal', 'Line one||Block two||Three', 'W' * 40 + '|' + 'i' * 80]
skip = {'Text File', 'Font File', 'Text', 'Font', 'L2 Font', 'L3 Font', 'Target Time', 'Duration', 'Separator'}
failures = 0
out = tempfile.mkdtemp()
for r in range(runs):
    args = [host, plugin, os.path.join(out, 'f.png'), str(random.choice([320, 640, 1280])), str(random.choice([180, 360, 720])),
            'Text=' + random.choice(texts), '@frames=12', '@dt=0.016']
    if random.random() < 0.5:
        args.append('Font=' + random.choice(['Inter / Bold', 'DejaVu Sans / Book', 'Cantarell / Regular', 'nonexistent font', '']))
    for name, typ, lo, hi in random.sample(params, k=min(len(params), random.randint(5, 40))):
        if name in skip or typ in (100, 14):
            continue
        if typ == 1:  # event
            v = random.choice([0, 1])
        elif typ in (0,):
            v = random.choice([0, 1])
        elif typ == 11:  # option: value, may be out of range on purpose
            v = random.randint(0, 19)
        else:
            v = random.uniform(lo - 0.1 * (hi - lo), hi + 0.1 * (hi - lo))  # also slightly out of range
        args.append(('+' if random.random() < 0.3 else '') + f'{name}={v:.4g}')
    for k, choices in (('Target Time', ['20:00', '0:00', 'x', '', '23:59:59']), ('Duration', ['5:00', '90', '1h', '', 'abc']),
                       ('Separator', ['', ' • ', '|', '   +++   '])):
        if random.random() < 0.3:
            args.append(k + '=' + random.choice(choices))
    for k in ('L2 Font', 'L3 Font'):
        if random.random() < 0.3:
            args.append(k + '=' + random.choice(['Bold', 'Light Italic', 'DejaVu Sans / Bold', 'x', '']))
    p = subprocess.run(args, capture_output=True, text=True, timeout=300)
    bad = (p.returncode != 0 or 'GL error' in p.stderr or 'state-leaks 0' not in p.stdout
           or 'runtime error' in p.stderr or 'AddressSanitizer' in p.stderr)
    if bad:
        failures += 1
        print('FAIL run', r, 'code', p.returncode, p.stderr[-400:], '\n  ', ' '.join(repr(a) for a in args[5:]))
print(f'{runs - failures}/{runs} runs ok')
sys.exit(1 if failures else 0)
