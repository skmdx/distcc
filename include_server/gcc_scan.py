"""Collect input identities from GCC's own preprocessing state."""
import os
from pathlib import Path
import subprocess
import stat
import distcc_pump_c_extensions


def scan(command, cwd, library):
  """Use the selected GCC frontend; never interpret C/C++ in Python."""
  env = dict(os.environ, LC_ALL='C', _DISTCC_SAFEGUARD='1', CCACHE_DISABLE='1')
  with os.fdopen(os.memfd_create('distcc-gcc-inputs', os.MFD_CLOEXEC), 'w+b') as output:
    fd = output.fileno()
    lookup = (env.get('DISTCC_CPP_LOOKUP_LIBRARY') or
              distcc_pump_c_extensions.cpp_lookup_library)
    if env.get('DISTCC_CPP_LOOKUP', '1') == '1' and os.access(lookup, os.R_OK):
      env['LD_PRELOAD'] = lookup + (':' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
      env['DISTCC_CPP_LOOKUP'] = '1'
      env['DISTCC_CPP_LOOKUP_OUTPUT_FD'] = str(fd)
    # Dependency-only mode skips ordinary tokens, including __COUNTER__ uses
    # that can change a later include condition. Run the full preprocessor;
    # only diagnostic expansion provenance can be omitted during collection.
    invocation = [*command, '-E', '-ftrack-macro-expansion=0',
                  '-fplugin=' + library,
                  '-fplugin-arg-' + Path(library).stem + '-output-fd=' + str(fd)]
    result = subprocess.run(invocation, cwd=cwd, env=env, pass_fds=(fd,),
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
    output.seek(0)
    data = output.read(64 * 1024 * 1024 + 1)
  # Source diagnostics (for example #error) need not prevent input collection.
  # A completed manifest lets the worker reproduce both diagnostics and partial
  # output. Startup failures, signals and incomplete collection remain failures.
  if (result.returncode < 0 or len(data) > 64 * 1024 * 1024 or
      not data.startswith(b'DCGS1\0') or not data.endswith(b'\0\0')):
    raise RuntimeError('Incomplete or unsupported GCC dependency scan: ' +
                       result.stderr.decode(errors='replace')[-2048:])
  fields = data[6:-1].split(b'\0')
  if fields.pop() != b'' or len(fields) % 2:
    raise RuntimeError('Malformed GCC dependency scan')
  # None denotes a failed header candidate, including a directory. The common
  # publication cache records its identity and verifies namespace changes.
  lookups = {}
  for path, metadata in zip(fields[::2], fields[1::2]):
    path = os.path.join(cwd, os.fsdecode(path))
    expected = None if metadata == b'-' else tuple(map(int, metadata.split()))
    if expected is not None and len(expected) != 6:
      raise RuntimeError('Malformed GCC dependency metadata')
    if expected is not None and stat.S_ISDIR(expected[2]):
      expected = expected[:3]
    if path in lookups and lookups[path] != expected:
      raise RuntimeError('Input changed during GCC dependency scan: ' + path)
    lookups[path] = expected
  files = []
  remaining = 64 * 1024 * 1024
  for path, expected in lookups.items():
    if expected is None or stat.S_ISDIR(expected[2]):
      continue
    remaining -= expected[3]
    if remaining < 0:
      raise RuntimeError('Compiler input snapshots exceed 64 MiB')
    files.append(path)
  if not files:
    raise RuntimeError('GCC dependency scan returned no inputs')
  # The common publication path validates these identities before and after
  # compression. Its immutable image cache reads only new or changed files.
  return {'files': None, 'dependencies': files, 'lookups': lookups}
