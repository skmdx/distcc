"""Compiler-backed pump analysis with an authoritative GCC dependency check."""
import collections
import io
import os
import re
import shlex
import shutil
import stat
import struct
import subprocess
import xxhash

import basics
import clang_scan
import compress_files
import include_analyzer_memoizing_node
import parse_file


def dependency_command(command, source):
  """Remove output/depfile actions without reordering preprocessing options."""
  result = [command[0]]
  args = iter(command[1:])
  for arg in args:
    if arg in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
      next(args)
    elif arg in ('-c', '-S', '-E', '-M', '-MM', '-MD', '-MMD', '-MP', '-MG', '-dM', '-dD', '-P'):
      continue
    elif any(arg.startswith(prefix) and arg != prefix
             for prefix in ('-o', '-MF', '-MT', '-MQ', '-MJ')):
      continue
    else:
      result.append(arg)
  if source not in result:
    raise basics.NotCoveredError('Cannot identify compiler input')
  return result


def run(command, allow_errors=False):
  try:
    # Probes belong to the admitted analyzer. Portage's PATH can contain
    # distcc wrappers; do not queue internal work behind the clients whose
    # include requests this process is serving.
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=30, env=dict(os.environ, LC_ALL='C', _DISTCC_SAFEGUARD='1'))
  except (OSError, subprocess.TimeoutExpired) as error:
    raise basics.NotCoveredError('Compiler dependency check failed: %s' % error) from error
  if result.returncode and not allow_errors:
    raise basics.NotCoveredError('Compiler dependency check failed: %s' %
                                 result.stderr.decode(errors='replace')[-2048:])
  return result


class CompilerAnalyzer(include_analyzer_memoizing_node.IncludeAnalyzerMemoizingNode):
  def __init__(self, *args, **kwargs):
    super().__init__(*args, **kwargs)
    self.contexts = collections.OrderedDict()
    self.command = None
    self.snapshots = None
    self.system_checks = None
    self.system_records = {}

  def DoCompilationCommand(self, cmd, *args, **kwargs):
    self.command = cmd
    try:
      files = super().DoCompilationCommand(cmd, *args, **kwargs)
      compression = kwargs.get('compression', args[2] if len(args) > 2 else 'lzo')
      directory = os.path.join(self.client_root_keeper.client_root, '.distcc-system')
      os.makedirs(directory, exist_ok=True)
      assert self.system_checks is not None
      name = xxhash.xxh3_64_hexdigest(self.system_checks) + '.' + compression
      path = os.path.join(directory, name)
      if not os.path.exists(path):
        compress_files.WriteCompressed(path, io.BytesIO(self.system_checks), compression)
      return files + [path]
    finally:
      self.snapshots = None
      self.system_checks = None
      self.command = None

  def SystemChecks(self, snapshots):
    """Describe default-header inputs and probes omitted from the sent tree.

    Include the conservative GCC graph's lookups too: a GCC-only existence
    probe need not appear in Clang's active dependency list. Stamps are checked
    again before publishing the immutable request. Encoded records share the
    graph's invalidation generation so warm requests do not reread headers.
    """
    directories = tuple(path.rstrip('/') + '/' for path in
                        (*self.systemdir_prefix_cache.systemdirs,
                         *self.compiler_defaults.missing_system_dirs))
    def system_path(path):
      return (path.rstrip('/') + '/').startswith(directories)

    paths = self.build_stat_cache.file_stamps.paths
    parents = {}
    inputs = []
    for (path, ancestor), stamp in paths.items():
      if ancestor:
        continue
      if system_path(path):
        canonical = self.canonical_path.Canonicalize(path)
      else:
        # Most failed lookups belong to project include directories. Resolve
        # their shared parent once, rather than realpath'ing every missing
        # header. A leaf symlink still needs its own target checked.
        leaf = paths.get((path, True))
        if leaf is not None and stat.S_ISLNK(leaf[2]):
          canonical = os.path.realpath(path)
        else:
          parent = os.path.dirname(path)
          if parent not in parents:
            # libc realpath cannot resolve a nonexistent tail. Missing
            # probes through directory aliases still need system checks.
            parents[parent] = os.path.realpath(parent)
          canonical = os.path.normpath(os.path.join(parents[parent], os.path.basename(path)))
      if system_path(path) or system_path(canonical):
        inputs.append((path, canonical, stamp))
    records = []
    remaining = 64 * 1024 * 1024
    for path, canonical, stamp in sorted(inputs):
      if path in self.system_records:
        record, size = self.system_records[path]
        records.append(record)
        remaining -= size
        if remaining < 0:
          raise basics.NotCoveredError('System header checks exceed 64 MiB')
        continue
      kind, size, digest = 0, 0, bytes(8)
      if stamp is not None:
        if stat.S_ISDIR(stamp[2]):
          kind = 2
        elif stat.S_ISREG(stamp[2]):
          kind = 1
          data = snapshots.get(canonical)
          if data is None:
            with open(path, 'rb') as source:
              data = source.read(remaining + 1)
          remaining -= len(data)
          if remaining < 0:
            raise basics.NotCoveredError('System header checks exceed 64 MiB')
          size, digest = len(data), xxhash.xxh3_64_digest(data)
        else:
          raise basics.NotCoveredError('Unsupported system header input: ' + path)
      encoded = os.fsencode(path)
      record = struct.pack('!IIQ8s', len(encoded), kind, size, digest) + encoded
      self.system_records[path] = record, size
      records.append(record)
    result = b'DCSH' + struct.pack('!I', len(records)) + b''.join(records)
    if len(result) > 16 * 1024 * 1024 or len(records) > 65536:
      raise basics.NotCoveredError('Too many system header checks')
    return result

  def ClearStatCaches(self):
    self.contexts.clear()
    self.system_records.clear()
    super().ClearStatCaches()

  def context(self, command, source):
    language = ('c++' if source.endswith(('.cc', '.cpp', '.cxx', '.C')) or
                os.path.basename(command[0]).endswith('++') else 'c')
    probe = []
    args = iter(command[1:])
    for arg in args:
      if arg == source:
        continue
      if arg == '-x':
        language = next(args)
        probe += ['-x', language]
        continue
      if arg in ('-include', '-imacros'):
        next(args)
        continue
      probe.append(arg)
    compiler = shutil.which(command[0]) or command[0]
    identity = os.stat(compiler)
    key = (compiler, identity.st_ino, identity.st_mtime_ns, language, tuple(probe), os.getcwd(),
           tuple((k, os.environ.get(k)) for k in
                 ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH',
                  'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'SDKROOT')))
    if key in self.contexts:
      self.contexts.move_to_end(key)
      return self.contexts[key]
    result = run([compiler, *probe, '-x', language, '-E', '-dM', '-v',
                  '-MD', '-MF', '-', '-MT', 'distcc', '/dev/null'])
    output = result.stdout.decode()
    sections = re.split(r'^distcc:', output, maxsplit=1, flags=re.M)
    if len(sections) != 2:
      raise basics.NotCoveredError('Compiler did not report implicit headers')
    macros, implicit = sections
    banner = run([compiler, '--version']).stdout.decode(errors='replace')
    version_match = re.search(r'clang version (\S+)', banner)
    clang = version_match is not None
    if clang:
      if version_match[1] != clang_scan.version:
        raise basics.NotCoveredError('Clang compiler and libclang-cpp versions differ')
      context = (True, [])
    else:
      if 'Free Software Foundation' not in banner:
        raise basics.NotCoveredError('Unsupported compiler for header analysis')
      search = re.search(r'#include <\.\.\.> search starts here:\n(.*?)End of search list\.',
                         result.stderr.decode(errors='replace'), re.S)
      if search is None:
        raise basics.NotCoveredError('Cannot obtain GCC header search path')
      options = ['--no-default-config', '-undef', '-nostdinc']
      for line in macros.splitlines():
        if not line.startswith('#define '):
          raise basics.NotCoveredError('Unexpected compiler macro output')
        name, _, value = line[8:].partition(' ')
        options.append('-D' + name + '=' + value)
      for directory in search[1].splitlines():
        options += ['-isystem', directory.strip()]
      for header in shlex.split(implicit.replace('\\\n', '').replace('$$', '$')):
        if header != '/dev/null':
          options += ['-include', header]
      context = (False, options)
      # The graph and compiler contexts share a cache generation. Seed once;
      # later commands only add definitions when their context is new.
      for option in options:
        if option.startswith('-D'):
          name, _, value = option[2:].partition('=')
          parse_file.InsertMacroDefInTable(name, value, self.symbol_table,
                                          self.support_master.InvalidateRecords)
    self.contexts[key] = context
    if len(self.contexts) > 8:
      self.contexts.popitem(last=False)
    return context

  def ProcessCompilationCommand(self, currdir, parsed_command):
    (self.quote_dirs, self.angle_dirs, self.include_files, self.translation_unit,
     self.result_file_prefix, self.d_opts) = parsed_command
    self.currdir_idx = self.directory_map.Index(currdir)
    for path in self.compiler_defaults.missing_system_dirs:
      self.build_stat_cache.file_stamps.Observe(path)
      if self.build_stat_cache.file_stamps.Stamp(path) is not None:
        raise basics.NotCoveredError('Compiler default include directory appeared: ' + path)
    command = dependency_command(self.command, self.translation_unit)
    frontend_args = [command[i + 1] for i, arg in enumerate(command[:-1])
                     if arg == '-Xclang']
    # Native CPU expansion uses paired cc1 option/value arguments. They affect
    # predefined macros and must reach the same frontend that scans the input.
    cpu_options = {'-target-cpu', '-tune-cpu', '-target-feature'}
    safe_frontend = (len(frontend_args) % 2 == 0 and
                     all(frontend_args[i] in cpu_options and frontend_args[i + 1]
                         for i in range(0, len(frontend_args), 2)))
    if not safe_frontend or command[-1] == '-Xclang' or any(
        (arg.startswith('-Xclang') and arg != '-Xclang') or
        arg.startswith(('-fmodule', '-include-pch', '-fplugin', '@',
                        '-Wp,', '-save-temps', '--save-temps', '-ftime-trace',
                        '-serialize-diagnostics', '--serialize-diagnostics'))
        for arg in command[1:]):
      raise basics.NotCoveredError('Modules, PCH, plugins and frontend overrides require local preprocessing')
    clang, options = self.context(command, self.translation_unit)
    if not clang and self.timer and (self.currdir_idx, self.quote_dirs, self.angle_dirs) not in self.master_cache:
      # The first conservative GCC graph may search hundreds of include
      # directories. Let it finish populating the cache instead of repeatedly
      # timing out and discarding it. Warm requests retain the normal quota.
      self.timer.quota = 20.0
    # Reject unsupported GCC inputs before running another full preprocessor.
    # Generated forwarding headers often contain absolute include operands.
    conservative = (super().ProcessCompilationCommand(currdir, parsed_command)
                    if not clang else {})
    try:
      compiler = shutil.which(command[0]) or command[0]
      scan_args = command[1:]
      if not clang:
        # These GCC switches have no Clang driver equivalents.
        # Predefined macros come from GCC above; its dependency verification
        # below and the actual compilation retain the original switches.
        scan_args = [arg for arg in scan_args if arg not in
                     ('-mabm', '-mno-abm', '-mhle', '-mno-hle',
                      '-mdirect-extern-access', '-mno-direct-extern-access',
                      '-mamx-tf32', '-mno-amx-tf32',
                      '-mamx-transpose', '-mno-amx-transpose',
                      '-ftree-pre', '-fno-tree-pre')]
      result = clang_scan.scan([compiler, *options, *scan_args, '-c'], currdir)
    except (RuntimeError, ValueError) as error:
      raise basics.NotCoveredError(str(error)) from error
    canonicalize = self.canonical_path.Canonicalize
    snapshots = {canonicalize(os.path.join(currdir, path)): data
                 for path, data in result['files'].items()}
    paths = set(result['dependencies'])
    if not clang:
      # -M omits existence-only probes. GCC may choose a different branch for
      # __has_builtin/__has_attribute, so also retain the branch-independent
      # include graph. Probe tokenization itself uses libclang-cpp.
      remaining = 64 * 1024 * 1024 - sum(map(len, snapshots.values()))
      for index in conservative:
        path = self.realpath_map.string[index]
        if path not in snapshots:
          with open(path, 'rb') as source:
            data = source.read(remaining + 1)
          if len(data) > remaining:
            raise basics.NotCoveredError('Compiler input snapshots exceed 64 MiB')
          snapshots[path] = data
          remaining -= len(data)
      # GCC builtins such as __has_builtin are not defined by -dM. Clang is
      # therefore never the sole authority for a GCC input closure.
      native = run([*command, '-E', '-M', '-MT', 'distcc', '-MF', '-'], allow_errors=True)
      dependencies = native.stdout.decode(errors='surrogateescape').replace('\\\n', '')
      if not dependencies.startswith('distcc:'):
        raise basics.NotCoveredError('Unexpected GCC dependency output')
      native_paths = shlex.split(dependencies[len('distcc:'):].replace('$$', '$'))
      for path in native_paths:
        canonical = canonicalize(os.path.join(currdir, path))
        if canonical not in snapshots:
          raise basics.NotCoveredError('GCC and Clang dependencies differ: ' + path)
      paths.update(native_paths)
    stamps = self.build_stat_cache.file_stamps
    for path, expected in result['lookups'].items():
      stamps.Observe(path)
      if stamps.Stamp(path) != expected:
        raise basics.NotCoveredError('Input changed after Clang dependency scan: ' + path)
    if not clang and stamps.Changed():
      raise basics.NotCoveredError('Input changed during GCC dependency verification')
    # Keep source bytes unchanged. A shared header can be reached through both
    # relative and absolute spellings; one injected #line cannot represent both.
    # The worker remaps its private root with compiler prefix-map options.
    closure = dict.fromkeys(conservative, ())
    for path in paths:
      absolute = path if os.path.isabs(path) else os.path.join(currdir, path)
      real_idx = self.realpath_map.Index(absolute)
      assert real_idx is not None  # A non-null path always has an index.
      if self.systemdir_prefix_cache.StartsWithSystemdir(real_idx, self.realpath_map):
        continue
      canonical = self.realpath_map.string[real_idx]
      if canonical not in snapshots:
        raise basics.NotCoveredError('Dependency has no input snapshot: ' + path)
      self.mirror_path.DoPath(absolute, self.currdir_idx, self.client_root_keeper.client_root)
      closure[real_idx] = ()
    self.snapshots = snapshots
    self.system_checks = self.SystemChecks(snapshots)
    basics.Debug(basics.DEBUG_TRACE, 'libclang-cpp analysis (%s): %d inputs',
                 'clang' if clang else 'gcc verified', len(closure))
    return closure
