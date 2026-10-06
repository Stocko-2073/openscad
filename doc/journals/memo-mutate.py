# Usage: python3 -I memo-mutate.py BASE.scad OUTDIR COUNT SEED
# Writes OUTDIR/mut_NN.scad, each BASE with one numeric literal changed by a
# visible amount (so a stale reuse would show in six-digit node dumps).
import random, re, sys, os
base_path, out_dir, count, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
src = open(base_path).read()
# Mark comment and string spans so their digits are never touched.
skip = [False] * len(src)
i = 0
while i < len(src):
    if src.startswith('//', i):
        j = src.find('\n', i); j = len(src) if j < 0 else j
        for k in range(i, j): skip[k] = True
        i = j
    elif src.startswith('/*', i):
        j = src.find('*/', i + 2); j = len(src) if j < 0 else j + 2
        for k in range(i, j): skip[k] = True
        i = j
    elif src[i] == '"':
        j = i + 1
        while j < len(src) and src[j] != '"':
            j += 2 if src[j] == '\\' else 1
        for k in range(i, min(j + 1, len(src))): skip[k] = True
        i = j + 1
    else:
        i += 1
lines_start = [0] + [m.end() for m in re.finditer('\n', src)]
def line_of(pos):
    lo, hi = 0, len(lines_start) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if lines_start[mid] <= pos: lo = mid
        else: hi = mid - 1
    return lo + 1
cands = []
for m in re.finditer(r'(?<![\w.$])(\d+\.\d*|\d*\.\d+|\d+)(?![\w.])', src):
    if skip[m.start()]: continue
    line = src[lines_start[line_of(m.start()) - 1]:].split('\n', 1)[0]
    if line.lstrip().startswith(('include', 'use')): continue
    cands.append(m)
rng = random.Random(seed)
os.makedirs(out_dir, exist_ok=True)
for n in range(1, count + 1):
    m = rng.choice(cands)
    text = m.group(1)
    value = float(text)
    if '.' in text:
        new = repr(round(value * 1.1 + 0.1, 4))
    else:
        new = str(int(value) + rng.choice([1, 2, -1]) if int(value) > 2 else int(value) + 1)
    out = src[:m.start()] + new + src[m.end():]
    path = os.path.join(out_dir, 'mut_%02d.scad' % n)
    open(path, 'w').write(out)
    print('%s line %d: %s -> %s' % (os.path.basename(path), line_of(m.start()), text, new))
