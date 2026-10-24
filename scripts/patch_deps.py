"""Apply hash-checked YOUYOU adaptations to pinned dependency sources.

Source licenses/notices remain in every overlay. LF/CRLF is normalized for hash
checks only. Preflight all files before writing; repeated application is safe.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'external/libdatachannel'
OVERLAYS = ROOT / 'scripts/dependency-overrides'

def canonical(path):
    return path.read_bytes().replace(b'\r\n', b'\n')

def digest(data):
    return hashlib.sha256(data).hexdigest()

def safe_child(base, relative):
    path = (base / relative).resolve()
    if not path.is_relative_to(base.resolve()):
        raise ValueError('Invalid overlay path')
    return path

def main():
    lock = json.loads((ROOT / 'scripts/sources.lock.json').read_text(encoding='utf-8'))
    for entry in lock:
        marker = ROOT / 'external' / entry['path'] / '.fetched'
        if not marker.is_file() or marker.read_text().splitlines()[:2] != [entry['ref'], entry['sha256']]:
            raise RuntimeError('Run fetch_deps.py for the pinned sources first: ' + entry['name'])
    manifest = json.loads((OVERLAYS / 'manifest.json').read_text(encoding='utf-8'))
    pending = []
    for entry in manifest:
        source = safe_child(OVERLAYS, entry['path'])
        target = safe_child(BASE, entry['path'])
        data = canonical(source)
        if digest(data) != entry['sha256']:
            raise RuntimeError('Overlay hash mismatch: ' + entry['path'])
        current = digest(canonical(target)) if target.is_file() else None
        if current == entry['sha256']:
            continue
        if current != entry['upstream_sha256']:
            raise RuntimeError('Unexpected dependency edits; refusing overwrite: ' + entry['path'])
        pending.append((target, data))
    for target, data in pending:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print(f'Verified {len(manifest)} adaptations; applied {len(pending)} files.')

if __name__ == '__main__':
    main()
