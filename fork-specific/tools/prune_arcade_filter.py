#!/usr/bin/env python3
"""Prune fork-specific/arcade.flt down to video games.

Run after regenerating arcade.flt with make_arcade_filter.py (see CLAUDE.md).
It removes fruit machines, casino/gambling games (slots, poker, bingo, medal
slots...), mahjong/hanafuda, quiz machines, pinball and mechanical games
(cranes, coin pushers, redemption) from the build:

  * a driver source file whose GAME()s are all excluded is dropped from the
    filter, unless a kept file includes its header;
  * an excluded game in a file that also has kept games gets a "-name" line,
    which drops it from the driver list (the file itself still compiles).

It also writes the list of remaining romsets (fork-specific/arcade-games.txt).

Usage: prune_arcade_filter.py [--report] [flt] [games.txt]
Safe to re-run on an already pruned filter.
"""
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'src', 'mame')

# Whole directories of fruit machine / gambling / pinball makers.
EXCLUDE_DIRS = {
    'bfm', 'barcrest', 'maygay', 'jpm', 'cirsa', 'recfranco',   # fruit machines
    'aristocrat', 'igt', 'amcoe',                                 # casino / video slots
    'pinball',                                                    # pinball machines
}

# Single files that are fruit machines, casino machines or pinball but whose
# games aren't all caught by the input/flag checks below.
EXCLUDE_FILES = set('''
adp/manohman adp/stella8085 adp/stellafr bally/eurotec bally/cashline capcom/miteshin
gaelco/sralslot misc/aces1 misc/acesp misc/acefruit misc/astrafr misc/castle misc/cleartone
misc/diamondking misc/ecoinf1 misc/ecoinf2 misc/ecoinf3 misc/ecoinfr misc/globalfr misc/hazelgr
misc/hobbyplay misc/interflip8035 misc/opercoin misc/proconn misc/sumt8035 sega/segafruit
sigma/sigmab31 skeleton/bolsaint skeleton/bullion2 skeleton/madmoney2 skeleton/shoken_md06
skeleton/sammy_pachislo_z80 skeleton/st25 universal/pachislo_z80 williams/pinball2k
sigma/sammymdl konami/konmedal konami/konmedal68k konami/konmedal020 konami/piratesh sega/anyworks
pc/przone pc/mdartstr misc/notechan sunwise/jankenmn ice/cutrope seibu/banprestoms namco/30test
'''.split())

# Video games caught by the checks below (shared input ports with a casino game,
# "slot" in the title, or a mechanical control on a real video game).
KEEP_GAMES = set('''
revenger revngr84 beastf snes4sl snes4sln
sbm sbmj realpunc realpuncj armchmp2 armchmp2o armchmp2o2
'''.split())

INPUT_CATEGORIES = [
    ('mahjong', re.compile(r'IPT_(MAHJONG|HANAFUDA)_')),
    ('casino', re.compile(r'IPT_(GAMBLE|POKER|SLOT)_')),
]

TITLE_CATEGORIES = [
    ('mahjong', re.compile(r'mah[- ]?j[oa]ng|majong|mahjang|hanafuda|\bjansou|\bjanshi', re.I)),
    ('quiz', re.compile(r'quiz|trivia', re.I)),
    ('mechanical', re.compile(r'\bmedal\b|prize|redemption', re.I)),
    ('casino', re.compile(r'poker|casino|roulette|black ?jack|\bkeno\b|\bbingo\b|baccarat|pachinko|'
                          r'pachi-?slo|\bslots?\b|\bcraps\b|\blott(o|ery)\b|fruit machine', re.I)),
]

GAME_START = re.compile(r'(?<![\w#])(GAME|GAMEL|CONS|COMP|SYST)\s*\(')
PORTS_BLOCK = re.compile(r'INPUT_PORTS_START\s*\(\s*(\w+)\s*\)(.*?)INPUT_PORTS_END', re.S)
PORT_INCLUDE = re.compile(r'PORT_INCLUDE\s*\(\s*(\w+)\s*\)')
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def strip_comments(text):
    """Remove C/C++ comments, leaving string and character literals alone."""
    return re.sub(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: m.group(0) if m.group(0)[0] in '"\'' else ' ', text, flags=re.S)


def split_args(text, start):
    """Split the macro arguments starting just after '(' at text[start]."""
    args, depth, cur, i, instr = [], 0, [], start, None
    while i < len(text):
        c = text[i]
        if instr:
            cur.append(c)
            if c == '\\':
                cur.append(text[i + 1])
                i += 1
            elif c == instr:
                instr = None
        elif c in '"\'':
            instr = c
            cur.append(c)
        elif c == '(':
            depth += 1
            cur.append(c)
        elif c == ')':
            if depth == 0:
                args.append(''.join(cur).strip())
                return args
            depth -= 1
            cur.append(c)
        elif c == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return None


def parse_games(text):
    games = []
    for m in GAME_START.finditer(text):
        args = split_args(text, m.end())
        if not args or len(args) < 11:
            continue
        title = ' '.join(re.findall(r'"((?:[^"\\]|\\.)*)"', args[9]))
        arcade = m.group(1) in ('GAME', 'GAMEL')
        games.append(dict(name=args[1], parent=args[2], ports=args[4] if arcade else '',
                          title=title, flags=args[10], arcade=arcade))
    return games


def load_ports():
    """Map input port name -> list of (file, body) for every INPUT_PORTS in src/mame."""
    ports = collections.defaultdict(list)
    for dirpath, _, names in os.walk(SRC):
        for n in names:
            if n.endswith('.cpp'):
                path = os.path.join(dirpath, n)
                text = strip_comments(read(path))
                for name, body in PORTS_BLOCK.findall(text):
                    ports[name].append((path, body))
    return ports


def port_text(ports, name, path, seen=None):
    """Body of an input port definition with PORT_INCLUDEs expanded."""
    seen = seen if seen is not None else set()
    if name in seen or name not in ports:
        return ''
    seen.add(name)
    defs = ports[name]
    body = next((b for p, b in defs if p == path), defs[0][1])
    return body + ''.join(port_text(ports, inc, path, seen) for inc in PORT_INCLUDE.findall(body))


def classify(rel, game, ports, path):
    stem = rel[:-4]
    if stem.split('/')[0] in EXCLUDE_DIRS or stem in EXCLUDE_FILES:
        return 'pinball' if 'pinball' in stem else 'fruit/casino'
    if not game['arcade']:
        return 'non-arcade'
    if game['name'] in KEEP_GAMES:
        return None
    if 'MACHINE_IS_BIOS_ROOT' in game['flags']:
        return None
    if 'MACHINE_MECHANICAL' in game['flags']:
        return 'mechanical'
    body = port_text(ports, game['ports'], path)
    for cat, rx in INPUT_CATEGORIES:
        if rx.search(body):
            return cat
    for cat, rx in TITLE_CATEGORIES:
        if rx.search(game['title']):
            return cat
    return None


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    report = '--report' in sys.argv
    flt = argv[0] if argv else os.path.join(ROOT, 'fork-specific', 'arcade.flt')
    out_games = argv[1] if len(argv) > 1 else os.path.join(ROOT, 'fork-specific', 'arcade-games.txt')

    lines = read(flt).splitlines()
    header = []
    for l in lines:
        if not l.startswith('//'):
            break
        header.append(l)
    header = header[:len(header) - 3] if any('prune_arcade_filter' in l for l in header) else header
    entries = [l for l in lines if l.strip() and not l.lstrip().startswith(('-', '//'))]
    files = [(l, l.split('//')[0].strip()) for l in entries]

    # Classify every system in src/mame, so a clone whose parent lives in a file
    # an earlier run already dropped still sees it (keeps re-runs stable).
    ports = load_ports()
    cat, parent, bios, home = {}, {}, set(), {}
    per_file = {rel: [] for _, rel in files}
    for dirpath, _, names in os.walk(SRC):
        for n in names:
            if not n.endswith('.cpp'):
                continue
            path = os.path.join(dirpath, n)
            rel = os.path.relpath(path, SRC).replace(os.sep, '/')
            for g in parse_games(strip_comments(read(path))):
                c = classify(rel, g, ports, path)
                cat[g['name']], parent[g['name']], home[g['name']] = c, g['parent'], rel
                if 'MACHINE_IS_BIOS_ROOT' in g['flags']:
                    bios.add(g['name'])
                if rel in per_file:
                    per_file[rel].append((g, c))

    # Clones follow their parent: an excluded parent takes its clones with it.
    # BIOS roots and console/computer parents (the Neo-Geo BIOS is a CONS) don't.
    changed = True
    while changed:
        changed = False
        for name, p in parent.items():
            if cat[name] is None and cat.get(p) not in (None, 'non-arcade') and p not in bios:
                cat[name] = cat[p]
                changed = True
    # Whatever a kept game needs as its parent or BIOS stays.
    for name in list(cat):
        p = parent[name]
        while cat[name] is None and p in cat and cat[p] is not None and home[p] in per_file:
            cat[p] = None
            p = parent[p]

    # Decide which files go entirely.
    drop = set()
    for rel, gl in per_file.items():
        if gl and all(cat[g['name']] for g, _ in gl):
            drop.add(rel)
    # Zero-GAME helper files: drop them when their directory has no kept file left.
    kept_dirs = {rel.split('/')[0] for rel, gl in per_file.items() if gl and rel not in drop}
    for rel, gl in per_file.items():
        if not gl and rel.split('/')[0] not in kept_dirs:
            drop.add(rel)
    # Keep any dropped file whose header a kept file includes (it defines shared code).
    while True:
        kept_incs = set()
        for rel in per_file:
            if rel not in drop:
                d = os.path.dirname(rel)
                for inc in INCLUDE.findall(read(os.path.join(SRC, rel))):
                    kept_incs.add(os.path.normpath(os.path.join(d, inc)))
                    kept_incs.add(os.path.normpath(inc))
        rescued = {rel for rel in drop if rel[:-4] + '.h' in kept_incs}
        if not rescued:
            break
        drop -= rescued

    excluded_names = []
    kept = set()
    for rel, gl in per_file.items():
        for g, _ in gl:
            if cat[g['name']]:
                if rel not in drop:
                    excluded_names.append(g['name'])
            else:
                kept.add(g['name'])

    with open(flt, 'w', encoding='utf-8') as f:
        f.write('\n'.join(header) + '\n')
        f.write('// Pruned by fork-specific/tools/prune_arcade_filter.py: no fruit machines, casino,\n'
                '// mahjong, quiz, pinball or mechanical games. "-name" lines drop single games\n'
                '// from files that also hold kept games.\n')
        for line, rel in files:
            if rel not in drop:
                f.write(line + '\n')
        f.write('\n')
        for n in sorted(excluded_names):
            f.write('-%s\n' % n)
    with open(out_games, 'w', encoding='utf-8') as f:
        f.write('\n'.join(sorted(kept)) + '\n')

    counts = collections.Counter(cat[g['name']] for gl in per_file.values() for g, _ in gl if cat[g['name']])
    print('files: %d kept, %d dropped' % (len(files) - len(drop), len(drop)))
    print('games: %d kept, %d excluded %s' % (len(kept), sum(counts.values()), dict(counts)))
    print('single-game exclusions in kept files: %d' % len(excluded_names))
    if report:
        titles = {g['name']: (g['title'], rel) for rel, gl in per_file.items() for g, _ in gl}
        for n in sorted(titles, key=lambda n: (str(cat[n]), titles[n][1])):
            if cat[n] and titles[n][1] not in drop:
                print('%-12s %-14s %-28s %s' % (cat[n], n, titles[n][1], titles[n][0]))


if __name__ == '__main__':
    main()
