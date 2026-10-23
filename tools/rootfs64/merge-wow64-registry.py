#!/usr/bin/env python3
"""merge-wow64-registry.py -- give the tuned 64-bit prefix the 32-bit (WoW64)
registry it needs for installers, without the boot cost of a full WoW64 prefix.

Background (roadmap Log 2026-10-23): the shipped prefix64 registry is heavily
stripped (one service: MountMgr) and boots fast. A fresh WoW64
`wineboot --init` registry runs 32-bit programs fine but auto-starts
Eventlog/NDIS/PlugPlay/nsiproxy, which cost ~15s per boot and made boots flaky.
Plain 32-bit programs need only windows/syswow64.link (build-prefix64.sh), but
installers need the 32-bit COM registrations (e.g. CLSID_ShellLink for
shortcuts) and the MSI service.

This merges, from NEW (fresh WoW64 registry) into OLD (shipped registry):
  * every key whose path contains Wow6432Node (the 32-bit view),
  * values missing from keys both files have (e.g. ProgramFilesDir (x86)),
  * the service keys named with --service (whole subtree), forced to
    demand-start (Start=3) so they never run at boot.
Keys only in NEW and not selected above are NOT copied. Everything before the
first key (the header and #arch=win64) is kept from OLD verbatim -- dropping
#arch makes wineserver treat the prefix as 32-bit.

Usage:
  merge-wow64-registry.py OLD_DIR NEW_DIR OUT_DIR [--service MSIServer ...]
(each *_DIR holds system.reg + user.reg; userdef.reg is copied from OLD)
"""
import argparse, os, re, shutil

KEY_RE = re.compile(r'^\[(.*?)\](.*)$')


def parse(path):
    text = open(path, encoding='utf-8', errors='surrogateescape').read()
    head, sections, cur = [], [], None
    for line in text.split('\n'):
        m = KEY_RE.match(line)
        if m:
            cur = [line, m.group(1), []]
            sections.append(cur)
        elif cur is None:
            head.append(line)
        else:
            cur[2].append(line)
    return '\n'.join(head), sections


def value_name(line):
    if line.startswith('@='):
        return '@'
    m = re.match(r'^("(?:[^"\\]|\\.)*")=', line)
    return m.group(1) if m else None


def merge(old_path, new_path, out_path, services):
    head, old = parse(old_path)
    _, new = parse(new_path)
    index = {s[1]: s for s in old}
    svc_prefixes = tuple('System\\\\CurrentControlSet\\\\Services\\\\' + s for s in services)
    added_keys = added_vals = added_svc = 0
    for hdr, key, lines in new:
        is_service = key.startswith(svc_prefixes)
        if is_service:
            lines = [('"Start"=dword:00000003' if l.startswith('"Start"=dword:') else l) for l in lines]
        if key in index:
            o = index[key]
            have = {value_name(l) for l in o[2] if value_name(l)}
            extra = [l for l in lines if value_name(l) and value_name(l) not in have]
            while o[2] and o[2][-1] == '':
                o[2].pop()
            o[2].extend(extra)
            o[2].append('')
            added_vals += len(extra)
        elif 'Wow6432Node' in key or is_service:
            old.append([hdr, key, list(lines)])
            index[key] = old[-1]
            if is_service:
                added_svc += 1
            else:
                added_keys += 1
    out = head + '\n' + '\n'.join(h + '\n' + '\n'.join(l) for h, _, l in old)
    if not out.endswith('\n'):
        out += '\n'
    open(out_path, 'w', encoding='utf-8', errors='surrogateescape').write(out)
    print(f"{out_path}: +{added_keys} Wow6432Node keys, +{added_svc} service keys, "
          f"+{added_vals} values into shared keys")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('old_dir')
    ap.add_argument('new_dir')
    ap.add_argument('out_dir')
    ap.add_argument('--service', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    for reg in ('system.reg', 'user.reg'):
        merge(os.path.join(a.old_dir, reg), os.path.join(a.new_dir, reg),
              os.path.join(a.out_dir, reg), a.service if reg == 'system.reg' else [])
    shutil.copy2(os.path.join(a.old_dir, 'userdef.reg'), os.path.join(a.out_dir, 'userdef.reg'))


if __name__ == '__main__':
    main()
