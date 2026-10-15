"""Compiler-backed pump analysis with branch-independent GCC include coverage."""
import collections
import io
import os
import re
import shlex
import shutil
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
  output = None
  try:
    # Probes belong to the admitted analyzer. Portage's PATH can contain
    # distcc wrappers; do not queue internal work behind the clients whose
    # include requests this process is serving.
    env = dict(os.environ, LC_ALL='C', _DISTCC_SAFEGUARD='1')
    library = env.get('DISTCC_CPP_LOOKUP_LIBRARY')
    if ('-M' in command and hasattr(os, 'memfd_create') and library and
        env.get('DISTCC_CPP_LOOKUP', '1') != '0' and os.access(library, os.R_OK)):
      # The lookup cache can replay a changed namespace only while stdout is
      # seekable and unpublished. Dependency output is small and stays in RAM.
      output = os.fdopen(os.memfd_create('distcc-dependencies', os.MFD_CLOEXEC), 'w+b')
      env['DISTCC_CPP_LOOKUP'] = '1'
      env['LD_PRELOAD'] = library + (':' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    result = subprocess.run(command, stdout=output if output is not None else subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=30, env=env)
    if output is not None:
      output.seek(0)
      result.stdout = output.read()
  except (OSError, subprocess.TimeoutExpired) as error:
    raise basics.NotCoveredError('Compiler dependency check failed: %s' % error) from error
  finally:
    if output is not None:
      output.close()
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
    self.system_includes = ()

  def DoCompilationCommand(self, cmd, *args, **kwargs):
    self.command = cmd
    try:
      files = super().DoCompilationCommand(cmd, *args, **kwargs)
      compression = kwargs.get('compression', args[2] if len(args) > 2 else 'lzo')
      directory = os.path.join(self.client_root_keeper.client_root, '.distcc-system')
      os.makedirs(directory, exist_ok=True)
      encoded = [(kind, os.fsencode(path)) for kind, path in self.system_includes]
      manifest = (b'DCSI' + struct.pack('!I', len(encoded)) +
                  b''.join(struct.pack('!II', kind, len(path)) + path for kind, path in encoded))
      name = xxhash.xxh3_64_hexdigest(manifest) + '.' + compression
      path = os.path.join(directory, name)
      if not os.path.exists(path):
        compress_files.WriteCompressed(path, io.BytesIO(manifest), compression)
      return files + [path]
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
    version_match = re.search(r'clang version (\S+)', banner)
    clang = version_match is not None
    if clang:
      if version_match[1] != clang_scan.version:
        raise basics.NotCoveredError('Clang compiler and libclang-cpp versions differ')
      options = []
      matching_search = False
    else:
      if 'Free Software Foundation' not in banner:
        raise basics.NotCoveredError('Unsupported compiler for header analysis')
      search = re.search(r'#include <\.\.\.> search starts here:\n(.*?)End of search list\.',
                         result.stderr.decode(errors='replace'), re.S)
      if search is None:
        raise basics.NotCoveredError('Cannot obtain GCC header search path')
      quote_search = re.search(r'#include "\.\.\." search starts here:\n(.*?)#include <\.\.\.>',
                               result.stderr.decode(errors='surrogateescape'), re.S)
      quote_dirs = tuple(line.strip() for line in quote_search[1].splitlines()) if quote_search else ()
      angle_dirs = tuple(line.strip() for line in search[1].splitlines())
      def directory_identities(paths):
        # GCC removes missing and duplicate search directories. The context
        # shares the graph's invalidation generation, including directory and
        # symlink changes, so compare their identities once per context.
        return tuple(dict.fromkeys(os.path.realpath(path) for path in paths
                                   if os.path.isdir(path)))
      matching_search = ('-I-' not in command and
          directory_identities(self.directory_map.string[index] for index in self.angle_dirs) == directory_identities(angle_dirs) and
          directory_identities(self.directory_map.string[index] for index in self.quote_dirs) == directory_identities((*quote_dirs, *angle_dirs)))
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
      # The graph and compiler contexts share a cache generation. Seed once;
      # later commands only add definitions when their context is new.
      for option in options:
        if option.startswith('-D'):
          name, _, value = option[2:].partition('=')
          parse_file.InsertMacroDefInTable(name, value, self.symbol_table,
                                          self.support_master.InvalidateRecords)
    # Probe only built-in search directories. Explicit -I/-isystem/-idirafter
    # arguments remain on the worker's command line with their original class
    # and ordering. Target, sysroot and -nostdinc options still apply here.
    search_options = ('-I', '-isystem', '-iquote', '-idirafter', '-iprefix',
                      '-iwithprefixbefore', '-iwithprefix', '-F', '-iframework')
    default_probe = []
    args = iter(probe)
    for arg in args:
      if arg in search_options:
        next(args)
      elif not arg.startswith(search_options):
        default_probe.append(arg)
    defaults = run([compiler, *default_probe, '-x', language, '-E', '-v', '/dev/null'])
    search = re.search(r'#include <\.\.\.> search starts here:\n(.*?)End of search list\.',
                       defaults.stderr.decode(errors='surrogateescape'), re.S)
    if search is None and '-nostdinc' not in default_probe:
      raise basics.NotCoveredError('Cannot obtain compiler default header search path')
    directories = tuple(line.strip() for line in search[1].splitlines()) if search else ()
    if len(directories) > 4096 or any(not os.path.isabs(path) for path in directories):
      raise basics.NotCoveredError('Unsupported compiler default header search path')
    if clang:
      # Clang distinguishes C++ system headers from implicit extern-C system
      # headers. Plain -isystem loses that distinction (including line flags).
      invocation = next((shlex.split(line) for line in defaults.stderr.decode().splitlines()
                         if ' -cc1 ' in line or ' "-cc1" ' in line), [])
      kinds = {invocation[i + 1]: (2 if arg == '-internal-externc-isystem' else 1)
               for i, arg in enumerate(invocation[:-1])
               if arg in ('-internal-isystem', '-internal-externc-isystem')}
      if any(path not in kinds for path in directories):
        raise basics.NotCoveredError('Cannot classify Clang default header directories')
      search_entries = [(kinds[path], path) for path in directories]
    else:
      search_entries = [(0, path) for path in directories]
      # -nostdinc suppresses GCC's implicit stdc-predef.h inclusion. Restore
      # the compiler-reported inputs explicitly, using their sent snapshots.
      for path in shlex.split(implicit.replace('\\\n', '').replace('$$', '$')):
        if path == '/dev/null':
          continue
        directory = next((directory for directory in directories
                          if path.startswith(directory.rstrip('/') + '/')), None)
        if directory is None:
          raise basics.NotCoveredError('Implicit header is outside compiler search paths')
        search_entries.append((3, path[len(directory.rstrip('/')) + 1:]))
    context = (clang, options, tuple(search_entries), matching_search)
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
    clang, options, self.system_includes, matching_search = self.context(command, self.translation_unit)
    if not clang:
      implicit = tuple(self.includepath_map.Index(options[i + 1],
                       ignore_absolute_path_warning=True)
                       for i, option in enumerate(options[:-1]) if option == '-include')
      parsed_command = (*parsed_command[:2], implicit + self.include_files, *parsed_command[3:])
    if not clang and self.timer and (self.currdir_idx, self.quote_dirs, self.angle_dirs) not in self.master_cache:
      # The first conservative GCC graph may search hundreds of include
      # directories. Let it finish populating the cache instead of repeatedly
      # timing out and discarding it. Warm requests retain the normal quota.
      self.timer.quota = 20.0
    # Reject unsupported GCC inputs before running another full preprocessor.
    # Generated forwarding headers often contain absolute include operands.
    conservative = (super().ProcessCompilationCommand(currdir, parsed_command)
                    if not clang else {})
    literal_graph = not clang and matching_search and all(
        all(isinstance(expr, parse_file.ProbeExpression) for expr in self.file_cache[index][2])
        for index in conservative)
    result = {'files': {}, 'dependencies': [], 'lookups': {}}
    if not literal_graph:
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
        # GCC can implicitly replace an ordinary header with its adjacent
        # precompiled image, whose captured macro state is not in this graph.
        precompiled = path + '.gch'
        if self.build_stat_cache.file_stamps.Observe(precompiled) is not None:
          raise basics.NotCoveredError('Precompiled header requires local preprocessing: ' + path)
        if literal_graph:
          # The compressor already owns immutable images for cached inputs.
          # Stream only new files there; the shared publication check below
          # compression rejects any source change during collection.
          stamp = self.build_stat_cache.file_stamps.Observe(path)
          if stamp is None:
            raise basics.NotCoveredError('Compiler input disappeared: ' + path)
          remaining -= stamp[3]
          if remaining < 0:
            raise basics.NotCoveredError('Compiler input snapshots exceed 64 MiB')
        elif path not in snapshots:
          with open(path, 'rb') as source:
            data = source.read(remaining + 1)
          if len(data) > remaining:
            raise basics.NotCoveredError('Compiler input snapshots exceed 64 MiB')
          snapshots[path] = data
          remaining -= len(data)
      # Literal include operands are covered in every branch by the lexical
      # graph, including branches selected only by GCC builtins. Computed
      # operands still need the real GCC check: the macro evaluator is an
      # overapproximation, not an implementation of every GCC extension.
      if not literal_graph:
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
      canonical = self.realpath_map.string[real_idx]
      if canonical not in snapshots:
        raise basics.NotCoveredError('Dependency has no input snapshot: ' + path)
      self.mirror_path.DoPath(absolute, self.currdir_idx, self.client_root_keeper.client_root)
      closure[real_idx] = ()
    self.snapshots = None if literal_graph else snapshots
    for kind, path in self.system_includes:
      if kind != 3:
        self.mirror_path.DoPath(os.path.join(path, 'forcing_technique_271828'),
                               self.currdir_idx, self.client_root_keeper.client_root)
    basics.Debug(basics.DEBUG_TRACE, 'libclang-cpp analysis (%s): %d inputs',
                 'clang' if clang else 'gcc literal graph' if literal_graph else 'gcc verified', len(closure))
    return closure
