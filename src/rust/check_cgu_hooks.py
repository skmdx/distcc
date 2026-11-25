"""Check the driver host and module against the target LLVM using only ELF tables."""
import os
import subprocess
import sys


def rows(*args):
    output = subprocess.check_output(['objdump', *args], text=True, env=dict(os.environ, LC_ALL='C'))
    return [line.split() for line in output.splitlines() if line.strip()]


def llvm_imports(table):
    return {row[-1] for row in table if '*UND*' in row and
            (row[-1].startswith('LLVM') or (row[-1].startswith('_Z') and '4llvm' in row[-1]))}


def check(driver, impl, llvm):
    for path in (driver, impl):
        needed = [r[1] for r in rows('-p', path) if r[0] == 'NEEDED' and
                  (r[1].startswith('libLLVM') or r[1] == 'distcc-rust-cgu-llvm.so')]
        if needed:
            raise SystemExit(f'{path} links unexpected libraries: {needed}')
    soname, = [row[1] for row in rows('-p', llvm) if row[0] == 'SONAME']
    table = rows('-T', driver)
    if imports := llvm_imports(table):
        raise SystemExit(f'CGU driver imports LLVM symbols: {sorted(imports)}')
    allocators = {'__malloc_hook', '__free_hook', '__realloc_hook', '__memalign_hook',
                  'malloc', 'free', 'calloc', 'realloc'}
    defined = {r[-1] for r in table if '*UND*' not in r}
    if hooks := allocators & defined:
        raise SystemExit(f'CGU driver defines allocator symbols: {sorted(hooks)}')
    prefix = 'dcc_hook_'
    sections = {r[1] for r in rows('-h', driver) if r[0].isdigit() and r[1].startswith(prefix)}
    exports = [(r[3], r[-1]) for r in table if len(r) >= 6 and r[1:3] == ['g', 'DF'] and r[3] in sections]
    exported_sections = {section for section, name in exports}
    if len(sections) != 14 or sections != exported_sections or len(exports) != len(sections):
        raise SystemExit(f'expected 14 CGU hooks, one per section; found {exports} for {sorted(sections)}')
    available = {r[-1] for r in rows('-T', llvm) if '*UND*' not in r}
    missing = ({name for section, name in exports} | llvm_imports(rows('-T', impl))) - available
    if missing:
        raise SystemExit(f'missing LLVM entry points in {soname}: ' + ', '.join(sorted(missing)))


if __name__ == '__main__':
    check(*sys.argv[1:])
