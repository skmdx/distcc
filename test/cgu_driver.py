"""Construct a direct driver invocation independently of distcc's ELF reader."""
import subprocess
from pathlib import Path


def driver_command(compiler, binary, *args, env, prefix=()):
    compiler = Path(compiler).resolve()
    rows = subprocess.check_output(['ldd', compiler], text=True).splitlines()
    driver, = [line.split(' => ')[1].rsplit(' (', 1)[0] for line in rows
               if line.lstrip().startswith('librustc_driver-')]
    llvm, = [line.split(' => ')[1].rsplit(' (', 1)[0] for line in rows if line.lstrip().startswith('libLLVM')]
    imports = subprocess.check_output(['nm', '-D', '-u', compiler], text=True)
    entry, = [line.split()[-1] for line in imports.splitlines()
              if line.endswith(('17rustc_driver_impl4main', '12rustc_driver4main'))]
    headers = subprocess.check_output(['readelf', '-lW', driver, llvm], text=True)
    tls = sum(int(line.split()[5], 16) for line in headers.splitlines() if line.lstrip().startswith('TLS '))
    previous = env.get('GLIBC_TUNABLES', '')
    settings = [s for s in previous.split(':') if s and not s.startswith('glibc.rtld.optional_static_tls=')]
    settings.append(f'glibc.rtld.optional_static_tls={max(65536, (tls + 32768 + 4095) & ~4095)}')
    return dict(args=[*prefix, 'env', '-a', compiler, binary, *args],
                env=dict(env, DISTCC_RUST_CGU_DRIVER=driver, DISTCC_RUST_CGU_ENTRY=entry,
                         DISTCC_RUST_CGU_PREV_TUNABLES=previous, GLIBC_TUNABLES=':'.join(settings)))
