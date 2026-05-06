#!/usr/bin/env python3
# analyze-capture.py : analyze terminal capture logs for escape sequence overhead
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)

import re
import sys
from collections import Counter

def analyze(path):
    data = open(path, 'rb').read()

    sync_begin = b'\x1b[?2026h'
    sync_end = b'\x1b[?2026l'

    # --- global counts ---

    counts = {
        'sync_begin':  data.count(sync_begin),
        'sync_end':    data.count(sync_end),
        'sgr_reset':   data.count(b'\x1b[m'),
        'sgr0':        data.count(b'\x1b[0m'),
        'scs_b':       data.count(b'\x1b(B'),
        'el':          data.count(b'\x1b[K'),
        'ed':          data.count(b'\x1b[2J'),
        'civis':       data.count(b'\x1b[?25l'),
        'cnorm':       data.count(b'\x1b[?25h'),
        'sc':          data.count(b'\x1b7'),
        'rc':          data.count(b'\x1b8'),
    }
    counts['cup'] = len(re.findall(rb'\x1b\[\d+;\d*H', data))
    counts['sgr_all'] = len(re.findall(rb'\x1b\[\d*m', data))

    print(f'File: {path}')
    print(f'Total bytes: {len(data)}')
    print()
    print('Sequence counts:')
    for k, v in counts.items():
        if v:
            print(f'  {k:14s} {v:6d}')

    # --- sync block analysis ---

    blocks = []
    pos = 0
    while True:
        start = data.find(sync_begin, pos)
        if start < 0:
            break
        end = data.find(sync_end, start + len(sync_begin))
        if end < 0:
            break
        blocks.append(data[start + len(sync_begin):end])
        pos = end + len(sync_end)

    if not blocks:
        print('\nNo sync blocks found.')
        return

    sizes = [len(b) for b in blocks]
    print(f'\nSync blocks: {len(blocks)}')
    print(f'  min size: {min(sizes)},  max: {max(sizes)},  avg: {sum(sizes)/len(sizes):.1f}')

    # classify: char-bearing vs cursor-only
    char_blocks = []
    cursor_only = []
    for i, content in enumerate(blocks):
        pre_esc = content.split(b'\x1b')[0]
        has_char = pre_esc and any(32 <= b <= 126 for b in pre_esc)
        cup = re.search(rb'\x1b\[(\d+);(\d+)H', content)
        row = int(cup.group(1)) if cup else -1
        col = int(cup.group(2)) if cup else -1
        if has_char:
            char_blocks.append((i, row, col, len(content)))
        else:
            cursor_only.append((i, row, col, len(content)))

    print(f'  with cell content: {len(char_blocks)}')
    print(f'  cursor-only:       {len(cursor_only)}')

    # --- overhead estimate ---

    sync_bytes = counts['sync_begin'] * 10 + counts['sync_end'] * 10
    scs_sgr_bytes = counts['scs_b'] * 3 + counts['sgr_reset'] * 3
    cursor_only_bytes = sum(sz + 20 for _, _, _, sz in cursor_only)

    print(f'\nOverhead estimate:')
    print(f'  sync begin+end:    {sync_bytes:6d} bytes ({sync_bytes*100/len(data):.0f}%)')
    print(f'  SCS+SGR resets:    {scs_sgr_bytes:6d} bytes ({scs_sgr_bytes*100/len(data):.0f}%)')
    print(f'  cursor-only total: {cursor_only_bytes:6d} bytes ({cursor_only_bytes*100/len(data):.0f}%)')

    # --- cursor-only row distribution ---

    if cursor_only:
        row_counts = Counter(r for _, r, _, _ in cursor_only)
        print(f'\nCursor-only blocks per row (top 10):')
        for row, cnt in row_counts.most_common(10):
            print(f'  row {row:3d}: {cnt:4d}')

    # --- first N blocks detail ---

    n = min(25, len(blocks))
    print(f'\nFirst {n} sync blocks:')
    for i in range(n):
        tag = 'CELL' if any(idx == i for idx, _, _, _ in char_blocks) else 'CUR '
        print(f'  [{i:4d}] {tag} ({sizes[i]:4d}B): {blocks[i][:72]}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f'usage: {sys.argv[0]} <capture.log>', file=sys.stderr)
        sys.exit(1)
    analyze(sys.argv[1])
