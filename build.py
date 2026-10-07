#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / 'build'
DIST = ROOT / 'dist'
TEMPLATE = ROOT / 'module'
STAGE = BUILD / 'module'
parser = argparse.ArgumentParser(description='Build backdownload for arm64 Android')
parser.add_argument('--ndk', type=Path, help='Android NDK directory on Linux')
args = parser.parse_args()
NDK = args.ndk.expanduser().resolve() if args.ndk else None
TOOLBIN = NDK / 'toolchains/llvm/prebuilt/linux-x86_64/bin' if NDK else None
CXX = TOOLBIN / 'clang++' if NDK else os.environ.get('CXX', 'clang++')
TARGET = ['--target=aarch64-linux-android28'] if NDK else []
STRIP = TOOLBIN / 'llvm-strip' if NDK else 'llvm-strip'
READELF = TOOLBIN / 'llvm-readelf' if NDK else 'readelf'
if NDK and not all(tool.is_file() for tool in [CXX, STRIP, READELF]):
    parser.error('Expected a Linux Android NDK containing Clang and LLVM tools')


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def output(*args):
    return subprocess.check_output([str(arg) for arg in args], cwd=ROOT, text=True).strip()


def compiler_file(name):
    path = Path(output(CXX, '-print-file-name=' + name))
    if not path.is_file():
        raise SystemExit('Clang could not locate ' + name)
    return path


def zip_files(destination, files):
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path, name in sorted(files, key=lambda item: item[1]):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 6, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if path.suffix == '.sh' or name == 'build.py' else 0o644
            info.external_attr = (0o100000 | mode) << 16
            archive.writestr(info, path.read_bytes())


BUILD.mkdir(exist_ok=True)
DIST.mkdir(exist_ok=True)
properties = dict(line.split('=', 1) for line in (TEMPLATE / 'module.prop').read_text().splitlines()
                  if line and not line.startswith('#'))
version = properties['version']
if not version or any(c not in '0123456789.' for c in version):
    raise SystemExit('Expected a numeric module version')
if STAGE.exists():
    shutil.rmtree(STAGE)
(STAGE / 'zygisk').mkdir(parents=True)
module_files = ['module.prop', 'customize.sh', 'post-fs-data.sh', 'service.sh',
                'action.sh', 'status.sh', 'update-status.sh']
for name in module_files:
    shutil.copy2(TEMPLATE / name, STAGE / name)

common = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
          '-fno-rtti', '-fno-threadsafe-statics', '-nostdlib++', '-I', ROOT / 'src']
test = BUILD / 'policy_test'
run(CXX, *TARGET, *common, '-O2', ROOT / 'tests/policy_test.cpp', '-o', test)
if not NDK:
    run(test)
else:
    print('Policy tests compiled for Android; execution requires an Android device')

status_test = BUILD / 'status_test'
run(CXX, *TARGET, *common, '-O2', ROOT / 'tests/status_test.cpp', '-o', status_test)
if not NDK:
    run(status_test)
run('python3', ROOT / 'tests/status_test.py')

obj = BUILD / 'module.o'
run(CXX, *TARGET, *common, '-Oz', '-fPIC', '-fvisibility=hidden', '-fstack-protector-strong',
    '-I', ROOT / 'third_party', '-c', ROOT / 'src/module.cpp', '-o', obj)

library = STAGE / 'zygisk/arm64-v8a.so'
if NDK:
    run(CXX, *TARGET, '-shared', '-nostdlib++', obj,
        '-Wl,-z,now,-z,relro,-z,noexecstack,-z,max-page-size=16384',
        '-Wl,--hash-style=gnu,--no-undefined,--build-id=sha1,-soname,backdownload.so',
        '-o', library)
else:
    # Native Android builds link explicitly to avoid app-private search paths.
    builtins = Path(output(CXX, '--print-libgcc-file-name'))
    run('ld.lld', '-shared', '-z', 'now', '-z', 'relro', '-z', 'noexecstack',
        '-z', 'max-page-size=16384', '--hash-style=gnu', '--no-undefined',
        '--build-id=sha1', '-soname', 'backdownload.so',
        compiler_file('crtbegin_so.o'), obj, builtins,
        '-L/system/lib64', '--as-needed', '-lc', '-ldl', '-lm',
        compiler_file('crtend_so.o'), '-o', library)
run(STRIP, '--strip-unneeded', library)

dynamic = output(READELF, '-d', library)
symbols = output(READELF, '--dyn-syms', '--wide', library)
headers = output(READELF, '-h', library)
segments = output(READELF, '-l', library)
if 'AArch64' not in headers or 'zygisk_module_entry' not in symbols:
    raise SystemExit('Invalid Zygisk arm64 entry point')
if 'RPATH' in dynamic or 'RUNPATH' in dynamic or 'libc++' in dynamic:
    raise SystemExit('Unexpected private RUNPATH/C++ runtime dependency')
if '/data/data/' in dynamic:
    raise SystemExit('An app-private directory leaked into the library dependency table')
if '0x4000' not in segments:
    raise SystemExit('Expected 16 KiB ELF alignment')
if 'liblog.so' in dynamic or '__android_log' in symbols:
    raise SystemExit('Unexpected runtime logging dependency')
sections = output(READELF, '-S', '--wide', library)
if '.debug' in sections:
    raise SystemExit('Unexpected debug sections in the release library')
if not NDK:
    native = ctypes.CDLL(str(library), mode=os.RTLD_NOW)
    if not native.zygisk_module_entry:
        raise SystemExit('Android linker could not resolve the Zygisk entry point')
    print('PASS: Android linker resolved native dependencies and Zygisk entry point')
for name in module_files:
    if name.endswith('.sh'):
        run('sh', '-n', TEMPLATE / name)

artifact = DIST / ('backdownload-v' + version + '-arm64.zip')
files = [(p, str(p.relative_to(STAGE))) for p in STAGE.rglob('*') if p.is_file()]
zip_files(artifact, files)
with zipfile.ZipFile(artifact) as archive:
    if archive.testzip() is not None:
        raise SystemExit('ZIP integrity check failed')
    required = set(module_files) | {'zygisk/arm64-v8a.so'}
    if set(archive.namelist()) != required:
        raise SystemExit('Unexpected installer contents')

report = {
    'compiler': output(CXX, '--version').splitlines()[0],
    'name': properties['name'], 'author': properties['author'], 'version': version,
    'target': 'arm64-v8a', 'zygisk_api': 4,
    'artifact': artifact.name,
    'artifact_sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(),
    'checks': [('Android policy test compilation' if NDK else 'native policy/error/read-back tests'),
               ('Android status test compilation' if NDK else 'native policy snapshot and status handoff tests'),
               'status freshness, failure and Action request/response tests',
               'ELF machine and exported entry',
               '16 KiB segment alignment', 'no private RUNPATH or libc++ dependency',
               'no liblog dependency, Android logging imports, or debug sections',
               'installer shell syntax',
               'ZIP structure and integrity'],
    'live_zygisk_validation': False,
    'policy_tests_executed': not bool(NDK),
    'android_linker_validation': not bool(NDK),
}
if not NDK:
    report['checks'].append('Android linker dependency resolution')

source_zip = DIST / ('backdownload-v' + version + '-source.zip')
source_files = [(ROOT / name, name) for name in
                ['.gitignore', '.gitattributes', 'build.py', 'README.md', 'LICENSE']]
for directory in ['src', 'tests', 'third_party']:
    source_files.extend((p, str(p.relative_to(ROOT))) for p in (ROOT / directory).rglob('*')
                        if p.is_file())
source_files.extend((TEMPLATE / name, 'module/' + name)
                    for name in module_files)
zip_files(source_zip, source_files)
with zipfile.ZipFile(source_zip) as archive:
    if archive.testzip() is not None:
        raise SystemExit('Source ZIP integrity check failed')
report['source_archive'] = source_zip.name
report['source_sha256'] = hashlib.sha256(source_zip.read_bytes()).hexdigest()
(DIST / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
(DIST / 'SHA256SUMS').write_text(
    report['artifact_sha256'] + '  ' + artifact.name + '\n' +
    report['source_sha256'] + '  ' + source_zip.name + '\n')
print('BUILT:', artifact)
print('SHA256:', report['artifact_sha256'])
print('SOURCE:', source_zip)
