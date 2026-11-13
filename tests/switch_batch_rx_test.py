"""Compare the real adapter with pinned libnx code using a mocked BSD boundary."""
from pathlib import Path
import argparse, hashlib, os, re, subprocess

p = argparse.ArgumentParser()
p.add_argument('--sdk-source', type=Path, required=True)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--work-dir', type=Path, required=True)
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
sdk = args.sdk_source.read_bytes()
assert hashlib.sha256(sdk).hexdigest() == '090e8e40f843e2cfd6f9614229a946774ddb713a0b79456ffeb584ab60cbf1ae'
s = sdk.decode()
helpers = s[s.index('static int _socketGetFd(int fd) {'):s.index('\nstatic int _socketOpen(struct _reent *r, void *fdptr, const char *path, int flags, int mode) {')]
common = s[s.index('static int _serializeMmsg('):s.index('\nint sendmmsg(')]
recv = s[s.index('int recvmmsg('):]
actual = (root/'source/switch_batch_rx.c').read_text()
# Boundary helpers and on-wire codecs must still be the original SDK source.
assert helpers in actual
assert common[:common.index('static int _mmsgInitCommon(')] in actual
actual = re.sub(r'^#include .+\n', '', actual, flags=re.M)
source = (root/'tests/switch_batch_rx_mock.hpp').read_text() + '\nnamespace sdk {\n' + helpers + common + recv + '\n}\n' + actual + '\n' + (root/'tests/switch_batch_rx_test.cpp').read_text()
# C->C++ pointer spelling only; production file is compiled as C by devkitA64.
source = source.replace('struct cmsghdr *cmsg = msgvec[i].msg_hdr.msg_control;', 'struct cmsghdr *cmsg = (struct cmsghdr*)msgvec[i].msg_hdr.msg_control;')
args.work_dir.mkdir(parents=True,exist_ok=True)
(args.work_dir/'generated.cpp').write_text(source)
env = os.environ.copy()
env['PATH'] = str(args.compiler.parent) + os.pathsep + env['PATH']
env['TEMP'] = env['TMP'] = str(args.work_dir)
compiled = subprocess.run([str(args.compiler),'-std=c++17','-O2',str(args.work_dir/'generated.cpp'),'-o',str(args.work_dir/'test.exe')],env=env,capture_output=True,text=True)
(args.work_dir/'build.log').write_text(compiled.stdout+compiled.stderr)
assert compiled.returncode==0,compiled.stderr
run = subprocess.run([str(args.work_dir/'test.exe')],env=env,capture_output=True,text=True)
(args.work_dir/'results.txt').write_text(run.stdout+run.stderr)
assert run.returncode==0,run.stdout+run.stderr
print(run.stdout)
