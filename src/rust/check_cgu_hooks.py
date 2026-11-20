"""Check the interposer against the target LLVM without loading either ELF."""
import os
import subprocess
import sys


def rows(*args):
    output = subprocess.check_output(['objdump', *args], text=True, env=dict(os.environ, LC_ALL='C'))
    return [line.split() for line in output.splitlines() if line.strip()]


def symbols(path):
    return {row[-1] for row in rows('-T', path) if 'DF' in row and '*UND*' not in row}


def check(shim, llvm):
    needed = {row[1] for row in rows('-p', shim) if row[0] == 'NEEDED'}
    soname, = [row[1] for row in rows('-p', llvm) if row[0] == 'SONAME']
    if soname not in needed:
        raise SystemExit(f'CGU shim does not link {soname}')
    prefix = 'dcc_hook_'
    sections = {r[1] for r in rows('-h', shim) if r[0].isdigit() and r[1].startswith(prefix)}
    sections.discard('dcc_hook_entries')
    exports = [(r[3], r[-1]) for r in rows('-T', shim) if len(r) >= 6 and r[1:3] == ['g', 'DF']]
    exported_sections = {section for section, name in exports}
    if not sections or sections != exported_sections or len(exports) != len(sections):
        raise SystemExit(f'expected one CGU hook per section; found {exports} for {sorted(sections)}')
    missing = {name for section, name in exports} - symbols(llvm)
    if missing:
        raise SystemExit(f'missing LLVM entry points in {soname}: ' + ', '.join(sorted(missing)))


if __name__ == '__main__':
    check(*sys.argv[1:])
