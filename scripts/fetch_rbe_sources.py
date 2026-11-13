"""Prepare or verify pinned RBE sources. Offline unless --download is explicit.

Only the source files named in the repository lock are read/written. No APK,
credentials, user settings, system packages, or historical build objects.
"""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'cmake/webrtc-rbe/upstream.lock.json'
REVISION = '218b56e516386cd57c7513197528c3124bcd7ef3'
ABSL_REVISION = 'fc733299410a7104a0848539baab0131b8a616b8'


def read_lock():
    lock = json.loads(LOCK.read_text(encoding='utf-8'))
    if (lock['schema'] != 1 or lock['webrtc_revision'] != REVISION or
            lock['chromium_third_party_revision'] != ABSL_REVISION):
        raise ValueError('Unexpected RBE source lock revision')
    for name, digest in lock['files_sha256'].items():
        path = PurePosixPath(name)
        if (path.is_absolute() or '..' in path.parts or path.parts[0] not in ('webrtc', 'abseil') or
                len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest)):
            raise ValueError('Invalid source lock entry')
    return lock


def verify_sources(source_root, lock):
    bad = [name for name, digest in lock['files_sha256'].items()
           if not (source_root / name).is_file() or
           hashlib.sha256((source_root / name).read_bytes()).hexdigest() != digest]
    if bad:
        raise ValueError(f'RBE sources missing/changed: {len(bad)}; first: {bad[0]}. '
                         'Use --seed for an existing source bundle or --download explicitly.')
    return {'webrtc_revision': REVISION, 'chromium_third_party_revision': ABSL_REVISION,
            'files_verified': len(lock['files_sha256']),
            'lock_sha256': hashlib.sha256(LOCK.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT / '.tools/rbe' / REVISION[:12])
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument('--seed', type=Path, help='Existing cache or standalone study root')
    actions.add_argument('--download', action='store_true', help='Fetch fixed public upstream sources')
    actions.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    lock = read_lock()
    expected = lock['files_sha256']
    source_root = args.root.resolve()

    # Used by both the local seed and public archive readers. Only locked,
    # verified bytes are written, and archive paths never become output paths.
    def store(name, data):
        if hashlib.sha256(data).hexdigest() != expected[name]:
            raise ValueError('Source SHA256 mismatch: ' + name)
        target = source_root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    if args.seed:
        seed = args.seed.resolve()
        if (seed / 'third_party/webrtc').is_dir():
            seed = seed / 'third_party'
        if seed == source_root:
            raise ValueError('Seed and destination must be different directories')
        for name in expected:
            store(name, (seed / name).read_bytes())
    elif args.download:
        # Fixed directory archives avoid thousands of individual requests.
        groups = [('webrtc/' + part, f'https://webrtc.googlesource.com/src/+archive/{REVISION}/{part}.tar.gz')
                  for part in ('api', 'rtc_base', 'system_wrappers', 'modules/remote_bitrate_estimator',
                               'modules/congestion_controller/goog_cc', 'modules/include',
                               'modules/rtp_rtcp/include', 'modules/rtp_rtcp/source')]
        groups.append(('abseil', 'https://chromium.googlesource.com/chromium/src/third_party/+archive/' +
                       ABSL_REVISION + '/abseil-cpp.tar.gz'))
        remaining = set(expected)
        for prefix, url in groups:
            wanted = {name for name in remaining if name.startswith(prefix + '/')}
            if not wanted:
                continue
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read()
            with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
                for member in archive:
                    relative = member.name.removeprefix('./')
                    name = prefix + '/' + relative
                    if name in wanted and member.isfile():
                        store(name, archive.extractfile(member).read())
                        remaining.remove(name)
                        wanted.remove(name)
            if wanted:
                raise ValueError('Archive missing locked file: ' + sorted(wanted)[0])
        for name in sorted(remaining):
            if not name.startswith('webrtc/'):
                raise ValueError('Unmapped source group: ' + name)
            url = 'https://webrtc.googlesource.com/src/+/' + REVISION + '/' + name[len('webrtc/'):] + '?format=TEXT'
            with urllib.request.urlopen(url, timeout=60) as response:
                store(name, base64.b64decode(response.read().strip(), validate=True))
    print(json.dumps(verify_sources(source_root, lock), sort_keys=True))


if __name__ == '__main__':
    main()
