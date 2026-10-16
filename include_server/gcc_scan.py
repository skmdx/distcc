"""Collect input identities from GCC's own preprocessing state."""
import os
from pathlib import Path
import subprocess
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
    # the module consumes its tokens without serializing preprocessed text.
    invocation = [*command, '-E', '-P', '-ftrack-macro-expansion=0',
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
      not data.startswith(b'DCGS2\0') or not data.endswith(b'\0\0')):
    raise RuntimeError('Incomplete or unsupported GCC dependency scan: ' +
                       result.stderr.decode(errors='replace')[-2048:])
  # Decode in one pass without allocating split-field lists or repeatedly
  # crossing Python's path/identity helpers for every failed header candidate.
  return distcc_pump_c_extensions.ParseGccInputs(data, cwd)
