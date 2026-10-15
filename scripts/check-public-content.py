# SPDX-License-Identifier: GPL-2.0-or-later
"""Check public source for accidental private files, instance data and language."""
from pathlib import Path
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
IGNORED = {'.git', 'work', 'outputs', 'build', 'dist', 'runtime', '__pycache__', '.venv'}
FORBIDDEN_SUFFIXES = {'.pfx', '.p12', '.key', '.pem', '.bin', '.sys', '.exe', '.dll',
                      '.pdb', '.dmp', '.etl', '.evtx', '.pcap', '.pcapng', '.pyc', '.log'}


def files():
    try:
        if not (ROOT / '.git').exists():
            raise OSError('Source archive has no repository metadata')
        result = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-co',
                                 '--exclude-standard', '-z'], check=True, capture_output=True)
        return sorted(set(p.decode('utf-8') for p in result.stdout.split(b'\0') if p))
    except (OSError, subprocess.CalledProcessError):
        return sorted(str(p.relative_to(ROOT)).replace('\\', '/') for p in ROOT.rglob('*')
                      if p.is_file() and not (set(p.relative_to(ROOT).parts) & IGNORED))


def violations(name, text):
    result = []
    if re.search(r'[\u0400-\u052f]', text):
        result.append('Cyrillic text in public source; use English or escaped locale patterns')
    if re.search(r'(?i)\b[A-Z]:[\\/]Users[\\/](?!Public\b|Default\b|<)[^\s\\/`"<>]+', text):
        result.append('literal personal Windows profile path')
    if re.search(r'\bS-1-5-21-\d+-\d+-\d+-\d+\b', text):
        result.append('literal user/machine SID')
    # Match identities by context, not arbitrary hex protocol payloads or GUIDs.
    for match in re.finditer(r'''(?im)\b(?:mac|address|peer|peers)\b[^\n]{0,30}?["']([0-9a-f]{12})["']''', text):
        value = match.group(1).lower()
        if value != '000000000000' and not value.startswith(('010203', '020000')):
            result.append('non-synthetic six-byte identity literal')
    for match in re.finditer(r'(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])', text):
        value = re.sub('[:-]', '', match.group()).lower()
        if value != '000000000000' and not value.startswith(('010203', '020000')):
            result.append('non-synthetic MAC address literal')
    return sorted(set(result))


def main():
    problems = []
    names = files()
    for name in names:
        path = ROOT / name
        if not path.exists():  # A tracked deletion is expected before commit.
            continue
        parts = set(Path(name).parts)
        if parts & IGNORED or '.local.' in path.name or path.suffix.lower() in FORBIDDEN_SUFFIXES:
            problems.append(f'{name}: private/generated/binary file in public source')
            continue
        try:
            text = path.read_text(encoding='utf-8-sig')
        except (UnicodeDecodeError, OSError):
            problems.append(f'{name}: expected readable UTF-8 source')
            continue
        problems.extend(f'{name}: {problem}' for problem in violations(name, text))
    template = json.loads((ROOT / 'config/devices.example.json').read_text(encoding='utf-8-sig'))
    if template.get('Devices') != [] or template.get('AdapterInstanceId') or not template.get('AutoDetect'):
        problems.append('Public configuration must be automatic with no mapped instances')
    if violations('fixture', 'MAC = "' + 'aa'*6 + '"') == []:
        problems.append('Identity scanner self-check failed')
    if violations('fixture', 'MAC = "020000000001"'):
        problems.append('Synthetic fixture scanner self-check failed')
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        return 1
    print(f'PASS public-content hygiene: {len(names)} source paths, English text and automatic template')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
