"""Builds bbport-kbm: the proxy SDL3.dll, BloodborneControls.exe and KeyTest.exe.

    python build.py --bbport "D:\\Games\\Bloodborne PC"            # build into dist\\
    python build.py --bbport "D:\\Games\\Bloodborne PC" --install  # and install into that folder
    python build.py --bbport "D:\\Games\\Bloodborne PC" --zip      # and pack the release zip

--bbport is the Bloodborne PC (bbport) package: the .def file of the proxy is generated from the
exports of its SDL3.dll (out\\SDL3_real.dll once installed), so the proxy matches that SDL build.
Needs Python 3, `pip install ziglang` (C compiler) and the .NET Framework 4 C# compiler that is
part of Windows 10/11.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / 'src'
DIST = HERE / 'dist'
BUILD = HERE / 'build'
VERSION = '2.0.1'


def pe_exports(path):
    d = path.read_bytes()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    secs = [struct.unpack_from('<IIII', d, opt + optsz + 40 * i + 8) for i in range(nsec)]

    def off(rva):
        for vs, va, rs, ra in secs:
            if va <= rva < va + max(vs, rs):
                return ra + rva - va
        raise ValueError(hex(rva))

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b'\0', o)].decode()

    o = off(struct.unpack_from('<I', d, opt + 112)[0])
    count, names = struct.unpack_from('<I', d, o + 24)[0], struct.unpack_from('<I', d, o + 32)[0]
    return [cstr(struct.unpack_from('<I', d, off(names) + 4 * i)[0]) for i in range(count)]


def is_proxy(path):
    return b'bbport kbm' in path.read_bytes()


def build_dll(sdl3):
    source = SRC / 'kbm_sdl3_proxy.c'
    overridden = set(re.findall(r'^EXPORT [^(]*?\b(SDL_\w+)\(', source.read_text(encoding='utf-8'), re.M))
    lines = ['LIBRARY SDL3.dll', 'EXPORTS']
    for name in pe_exports(sdl3):
        lines.append(f'    {name}' if name in overridden else f'    {name}=SDL3_real.{name}')
    (BUILD / 'SDL3.def').write_text('\n'.join(lines) + '\n')
    print(f'SDL3.dll: {len(lines) - 2} exports, {len(overridden)} overridden')
    subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-target', 'x86_64-windows-gnu', '-shared', '-O2',
                    '-Wall', '-Wno-unused-function', '-o', str(DIST / 'SDL3.dll'), str(source),
                    str(BUILD / 'SDL3.def')], check=True)


def build_keytest():
    subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-target', 'x86_64-windows-gnu', '-O2',
                    '-o', str(DIST / 'KeyTest.exe'), str(SRC / 'keytest.c'), '-luser32'], check=True)


def build_controls():
    windir = Path(os.environ.get('WINDIR', r'C:\Windows'))
    csc = windir / r'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if not csc.exists():
        csc = windir / r'Microsoft.NET\Framework\v4.0.30319\csc.exe'
    cmd = [str(csc), '/nologo', '/target:winexe', '/optimize+', '/codepage:65001',
           f'/out:{DIST / "BloodborneControls.exe"}', '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll',
           '/r:System.Core.dll', str(SRC / 'BloodborneControls.cs')]
    subprocess.run(cmd, check=True)


def install(bbport):
    out = bbport / 'out'
    real, current = out / 'SDL3_real.dll', out / 'SDL3.dll'
    if not real.exists():
        if is_proxy(current):
            sys.exit('out\\SDL3.dll is already a proxy but out\\SDL3_real.dll is missing')
        shutil.copy2(current, real)
        print('Original SDL3.dll saved as out\\SDL3_real.dll')
    shutil.copy2(DIST / 'SDL3.dll', current)
    for exe in ('BloodborneControls.exe', 'KeyTest.exe'):
        shutil.copy2(DIST / exe, bbport / exe)
    print('Installed into', bbport)


def pack():
    zip_path = DIST / f'bbport-kbm-{VERSION}.zip'
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        # Extracted into the bbport folder: keep its own README.md / LICENSE untouched.
        for name in ('BloodborneControls.exe', 'KeyTest.exe'):
            z.write(DIST / name, name)
        for name in ('install-kbm.bat', 'uninstall-kbm.bat'):
            z.write(HERE / name, name)
        z.write(DIST / 'SDL3.dll', 'kbm/SDL3.dll')
        for name in ('README.md', 'README.ru.md', 'LICENSE'):
            z.write(HERE / name, 'kbm/' + name)
    print('Packed', zip_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bbport', required=True, type=Path, help='Bloodborne PC (bbport) package folder')
    ap.add_argument('--install', action='store_true')
    ap.add_argument('--zip', action='store_true')
    args = ap.parse_args()
    out = args.bbport / 'out'
    sdl3 = out / 'SDL3_real.dll' if (out / 'SDL3_real.dll').exists() else out / 'SDL3.dll'
    if not sdl3.exists() or is_proxy(sdl3):
        sys.exit(f'No original SDL3.dll in {out}')
    DIST.mkdir(exist_ok=True)
    BUILD.mkdir(exist_ok=True)
    build_dll(sdl3)
    build_keytest()
    build_controls()
    if args.install:
        install(args.bbport)
    if args.zip:
        pack()


if __name__ == '__main__':
    main()
