"""Collect input identities from GCC's own preprocessing state."""
import os
from pathlib import Path
import subprocess


def driver_options(compiler, commands):
  """Decode driver and frontend arguments using the selected GCC's option tables."""
  env = dict(os.environ, LC_ALL='C', _DISTCC_SAFEGUARD='1')
  library = env.get('DISTCC_GCC_SCAN_LIBRARY')
  if not library:
    directory = subprocess.check_output([compiler, '-print-file-name=plugin'],
                                        env=env, text=True).strip()
    library = str(Path(directory) / 'distcc_gcc_scan.so')
  with os.fdopen(os.memfd_create('distcc-driver-options', os.MFD_CLOEXEC), 'w+b') as stream:
    for command in commands:
      stream.write(str(len(command)).encode() + b'\0')
      for arg in command:
        stream.write(os.fsencode(arg) + b'\0')
    stream.flush()
    stream.seek(0)
    subprocess.run([compiler, '-E', '-x', 'c', '/dev/null', '-o', '/dev/null',
                    '-fplugin=' + library,
                    '-fplugin-arg-' + Path(library).stem + '-driver-fd=' + str(stream.fileno())],
                   env=env, pass_fds=(stream.fileno(),), check=True, stderr=subprocess.PIPE)
    stream.seek(0)
    fields = iter(stream.read().split(b'\0'))
    result = []
    for command in commands:
      options = []
      for _ in range(int(next(fields))):
        kind, count = next(fields), int(next(fields))
        options.append((kind == b'I', [os.fsdecode(next(fields)) for _ in range(count)]))
      result.append(options)
    assert list(fields) == [b''], 'incomplete native GCC option result'
    return result


def scan(command, cwd, library):
  """Use the selected GCC frontend; never interpret C/C++ in Python."""
  import distcc_pump_c_extensions
  import basics
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
    invocation = [*command, '-E', '-P', '-fpch-preprocess', '-ftrack-macro-expansion=0',
                  '-fplugin=' + library,
                  '-fplugin-arg-' + Path(library).stem + '-output-fd=' + str(fd)]
    # Full preprocessing can outlive a fixed wall-clock deadline under load.
    # The analyzer worker owns this subprocess and its cancellation lifetime.
    result = subprocess.run(invocation, cwd=cwd, env=env, pass_fds=(fd,),
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    output.seek(0)
    data = output.read()
  if result.returncode >= 0 and data == b'DCGL1\0':
    raise basics.LocalCompilation('GCC selected a precompiled header')
  # Source diagnostics (for example #error) need not prevent input collection.
  # A completed manifest lets the worker reproduce both diagnostics and partial
  # output. Startup failures, signals and incomplete collection remain failures.
  if (result.returncode < 0 or
      not data.startswith(b'DCGS2\0') or not data.endswith(b'\0\0')):
    raise RuntimeError('Incomplete or unsupported GCC dependency scan (exit=' + str(result.returncode) +
                       ', manifest=' + str(len(data)) + ' bytes, prefix=' + repr(data[:16]) + '): ' +
                       result.stderr.decode(errors='replace')[-2048:])
  # Decode in one pass without allocating split-field lists or repeatedly
  # crossing Python's path/identity helpers for every failed header candidate.
  return distcc_pump_c_extensions.ParseGccInputs(data, cwd)
