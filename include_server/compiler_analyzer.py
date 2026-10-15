"""Compiler-backed pump analysis with an authoritative GCC dependency check."""
import collections
import os
import re
import shlex
import shutil
import subprocess

import basics
import clang_scan
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
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=30, env=dict(os.environ, LC_ALL='C'))
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

  def DoCompilationCommand(self, cmd, *args, **kwargs):
    self.command = cmd
    try:
      return super().DoCompilationCommand(cmd, *args, **kwargs)
    finally:
      self.snapshots = None
      self.command = None

  def ClearStatCaches(self):
    self.contexts.clear()
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
    version_match = re.search(r'clang version (\d+\.\d+\.\d+)', banner)
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
    self.contexts[key] = context
    if len(self.contexts) > 8:
      self.contexts.popitem(last=False)
    return context

  def ProcessCompilationCommand(self, currdir, parsed_command):
    (self.quote_dirs, self.angle_dirs, self.include_files, self.translation_unit,
     self.result_file_prefix, self.d_opts) = parsed_command
    self.currdir_idx = self.directory_map.Index(currdir)
    command = dependency_command(self.command, self.translation_unit)
    if any(arg.startswith(('-fmodule', '-include-pch', '-fplugin', '-Xclang', '@',
                           '-Wp,', '-save-temps', '--save-temps', '-ftime-trace',
                           '-serialize-diagnostics', '--serialize-diagnostics'))
           for arg in command[1:]):
      raise basics.NotCoveredError('Modules, PCH, plugins and frontend overrides require local preprocessing')
    clang, options = self.context(command, self.translation_unit)
    try:
      result = clang_scan.scan([command[0], *options, *command[1:], '-c'], currdir)
    except (RuntimeError, ValueError) as error:
      raise basics.NotCoveredError(str(error)) from error
    snapshots = {os.path.realpath(path): data for path, data in result['files'].items()}
    paths = set(result['dependencies'])
    conservative = {}
    if not clang:
      # The branch-independent graph must see the same effective GCC macros
      # as the Clang scan, including command-line aliases and function macros.
      for option in options:
        if option.startswith('-D'):
          name, _, value = option[2:].partition('=')
          parse_file.InsertMacroDefInTable(name, value, self.symbol_table,
                                          self.support_master.InvalidateRecords)
      # -M omits existence-only probes. GCC may choose a different branch for
      # __has_builtin/__has_attribute, so also retain the branch-independent
      # include graph. Probe tokenization itself uses libclang-cpp.
      conservative = super().ProcessCompilationCommand(currdir, parsed_command)
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
        canonical = os.path.realpath(path)
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
    closure = conservative.copy()
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
      # Preserve the existing transfer format's logical source names.
      closure[real_idx] = [(self.directory_map.Index(os.path.dirname(absolute)),
                            self.includepath_map.Index(os.path.basename(absolute)))]
    self.snapshots = snapshots
    basics.Debug(basics.DEBUG_TRACE, 'libclang-cpp analysis (%s): %d inputs',
                 'clang' if clang else 'gcc verified', len(closure))
    return closure
