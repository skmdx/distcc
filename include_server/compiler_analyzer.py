"""Compiler-backed pump analysis and immutable dependency snapshots."""
import collections
import io
import os
import re
import shlex
import shutil
import stat
import struct
import subprocess
import time
import xxhash

import basics
import clang_scan
import gcc_scan
import compress_files
import include_analyzer
import parse_command
import scan_statistics as statistics


def dependency_command(command, source):
  """Remove output/depfile actions without reordering preprocessing options."""
  result = [command[0]]
  args = iter(command[1:])
  for arg in args:
    if arg in ('-Xclang', '-mllvm'):
      result.extend((arg, next(args)))
    elif arg in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
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
    env = dict(os.environ, LC_ALL='C', _DISTCC_SAFEGUARD='1', CCACHE_DISABLE='1')
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


class CompilerAnalyzer(include_analyzer.IncludeAnalyzer):
  def __init__(self, *args, **kwargs):
    super().__init__(*args, **kwargs)
    self.contexts = collections.OrderedDict()
    self.frontends = {}
    self.command = None
    self.snapshots = None
    self.system_includes = ()

  def DoCompilationCommand(self, cmd, *args, **kwargs):
    self.command = cmd
    try:
      # Earlier jobs may have observed generated files unrelated to this one.
      # If they change during the native scan, retire the cache generation and
      # restart once. Never reuse path mappings from an invalidated generation.
      for _ in range(2):
        try:
          files = super().DoCompilationCommand(cmd, *args, **kwargs)
          break
        except include_analyzer.CacheInvalidated:
          self.ClearStatCaches()
      else:
        raise basics.NotCoveredError('Inputs keep changing during dependency analysis',
                                     send_email=False)
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
    language = 'none'
    probe = []
    args = iter(command[1:])
    for arg in args:
      if arg in ('-Xclang', '-mllvm'):
        # This operand belongs to cc1. In particular -xop is a target
        # feature, not the driver's joined language option -x<language>.
        probe.extend((arg, next(args)))
        continue
      if arg == source:
        continue
      if arg == '-x':
        language = next(args)
        probe += ['-x', language]
        continue
      if arg.startswith('-x'):
        language = arg[2:]
      if arg in ('-include', '-imacros'):
        next(args)
        continue
      probe.append(arg)
    language = parse_command.CompilerLanguage(command[0], source, language)
    compiler = shutil.which(command[0]) or command[0]
    identity = os.stat(compiler)
    key = (compiler, identity.st_ino, identity.st_mtime_ns, language, tuple(probe), os.getcwd(),
           self.quote_dirs, self.angle_dirs,
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
    _, implicit = sections
    # Identify the selected frontend independently of job macro overrides.
    # Shader preprocessing commonly uses -undef; -U/-D can also change the
    # compiler's predefined names without changing the compiler itself.
    identity_key = (compiler, identity.st_ino, identity.st_mtime_ns)
    if identity_key not in self.frontends:
      macros = run([compiler, '-x', 'c', '-E', '-dM', '/dev/null']).stdout.decode()
      if '#define __clang__ ' in macros:
        self.frontends[identity_key] = True
      elif '#define __GNUC__ ' in macros:
        self.frontends[identity_key] = False
      else:
        raise basics.NotCoveredError('Unsupported compiler for header analysis')
    clang = self.frontends[identity_key]
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
      invocation = next((clang_scan.tokenize_command(line) for line in defaults.stderr.decode().splitlines()
                         if ' -cc1 ' in line or ' "-cc1" ' in line), [])
      kinds = {invocation[i + 1]: (2 if arg == '-internal-externc-isystem' else 1)
               for i, arg in enumerate(invocation[:-1])
               if arg in ('-internal-isystem', '-internal-externc-isystem')}
      if any(path not in kinds for path in directories):
        raise basics.NotCoveredError('Cannot classify Clang default header directories')
      search_entries = [(kinds[path], path) for path in directories]
      # LibTooling may use another Clang release's builtin headers. Scan the
      # selected compiler's search paths, exactly as the worker will use them.
      options = ['-nostdinc']
      for kind, path in search_entries:
        options.extend(['-Xclang', '-internal-isystem' if kind == 1 else
                        '-internal-externc-isystem', '-Xclang', path])
    else:
      options = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
      if not options:
        target = run([compiler, '-dumpmachine']).stdout.decode().strip()
        version = run([compiler, '-dumpfullversion']).stdout.decode().strip()
        options = os.path.join(clang_scan.gcc_scan_dir, target, version, 'distcc_gcc_scan.so')
      if not os.path.isfile(options):
        raise basics.NotCoveredError('GCC dependency module is not installed for this compiler')
      search_entries = [(4, 'gcc')] + [(0, path) for path in directories]
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
    context = (clang, options, tuple(search_entries))
    self.contexts[key] = context
    if len(self.contexts) > 8:
      self.contexts.popitem(last=False)
    return context

  def ProcessCompilationCommand(self, currdir, parsed_command):
    (self.quote_dirs, self.angle_dirs, self.include_files, self.translation_unit,
     self.result_file_prefix, self.d_opts) = parsed_command
    self.currdir_idx = self.directory_map.Index(currdir)
    self.include_dir_pairs.add((self.quote_dirs, self.angle_dirs))
    self.quote_dirs_set.add(self.quote_dirs)
    self.angle_dirs_set.add(self.angle_dirs)
    statistics.quote_path_total += len(self.quote_dirs)
    statistics.angle_path_total += len(self.angle_dirs)
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
    clang, options, self.system_includes = self.context(command, self.translation_unit)
    started = time.perf_counter()
    try:
      compiler = shutil.which(command[0]) or command[0]
      if clang:
        result = clang_scan.scan([compiler, *command[1:], *options, '-c'], currdir)
        statistics.preamble_hits += result['preamble_hit']
        statistics.preamble_builds += result['preamble_built']
        statistics.preamble_bytes = result['preamble_bytes']
      else:
        native = gcc_scan.scan([compiler, *command[1:]], currdir, options)
        # Preserve GCC's actual default directory classes.
        # -isystem alone changes C++ directories into extern-C system headers.
        classes = {1: 5, 2: 0}
        if any(kind == 0 and native['directories'].get(path) not in classes
               for kind, path in self.system_includes):
          raise RuntimeError('GCC reported an unsupported default include directory')
        self.system_includes = tuple(
            (classes[native['directories'][path]] if kind == 0 else kind, path)
            for kind, path in self.system_includes)
        result = native
    except clang_scan.InputChangedError as error:
      raise include_analyzer.CacheInvalidated(str(error)) from error
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
      raise basics.NotCoveredError(str(error), send_email=False) from error
    finally:
      statistics.scan_seconds += time.perf_counter() - started
      statistics.scan_counter += 1
    canonicalize = self.canonical_path.Canonicalize
    snapshots = (None if result['files'] is None else
                 {canonicalize(os.path.join(currdir, path)): data
                  for path, data in result['files'].items()})
    paths = set(result['dependencies'])
    stamps = self.build_stat_cache.file_stamps
    self.current_input_paths, changes = stamps.InputChanges(result['lookups'])
    for path, expected in changes.items():
      observed = stamps.Observe(path)
      # Failed header candidates include directories. Reuse their observed
      # absence: Changed below checks creation/replacement notifications (or
      # stats every path when notifications cannot be trusted). Positive files
      # still need a fresh stamp for mmap and hard-link writes.
      changed = (observed is not None and not stat.S_ISDIR(observed[2])
                 if expected is None else observed != expected)
      if changed:
        current = stamps.Stamp(path)
        matches_scan = ((current is None or stat.S_ISDIR(current[2]))
                        if expected is None else current == expected)
        if matches_scan:
          # Cached bytes/path mappings belong to a previous input generation.
          raise include_analyzer.CacheInvalidated()
        raise include_analyzer.CacheInvalidated('Input changed after compiler dependency scan: ' + path)
    if stamps.Changed(()):
      raise include_analyzer.CacheInvalidated()
    # Keep source bytes unchanged. A shared header can be reached through both
    # relative and absolute spellings; one injected #line cannot represent both.
    # The worker remaps its private root with compiler prefix-map options.
    closure = {}
    for path in paths:
      absolute = path if os.path.isabs(path) else os.path.join(currdir, path)
      real_idx = self.realpath_map.Index(absolute)
      assert real_idx is not None  # A non-null path always has an index.
      canonical = self.realpath_map.string[real_idx]
      if snapshots is not None and canonical not in snapshots:
        raise basics.NotCoveredError('Dependency has no input snapshot: ' + path)
      self.mirror_path.DoPath(absolute, self.currdir_idx, self.client_root_keeper.client_root)
      stamp = stamps.Observe(canonical)
      if stamp is None:
        raise include_analyzer.CacheInvalidated('Dependency disappeared: ' + path)
      target = self.mirror_path.FileIdentity(
          canonical, stamp[:2], self.client_root_keeper.client_root)
      if target != canonical:
        if snapshots is not None:
          snapshots[target] = snapshots[canonical]
        real_idx = self.realpath_map.Index(target)
      closure[real_idx] = ()
    self.snapshots = snapshots
    # Explicit search paths remain in argv even when no header is read from
    # them. Mirror their directory/link structure too, otherwise the worker
    # invents -Wmissing-include-dirs diagnostics (fatal under -Werror).
    # Observe absent paths as well so generated/removed directories invalidate
    # the mirror, but never manufacture directories missing on the client.
    directories = {os.path.join(currdir, self.directory_map.string[index]).rstrip('/') or '/'
                   for index in self.quote_dirs}
    directories.update(path for kind, path in self.system_includes if kind not in (3, 4))
    for path in directories:
      observed = stamps.Observe(path)
      if observed is not None and stat.S_ISDIR(observed[2]):
        self.mirror_path.DoPath(os.path.join(path, 'forcing_technique_271828'),
                               self.currdir_idx, self.client_root_keeper.client_root)
    basics.Debug(basics.DEBUG_TRACE, 'Compiler dependency analysis (%s): %d inputs',
                 'clang' if clang else 'gcc', len(closure))
    return closure
