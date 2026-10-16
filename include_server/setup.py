#!/usr/bin/env python3

# Copyright 2007 Google Inc.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
# USA.

"""Build the include server module.

The version number should be passed to this script through the environment
variable DISTCC_VERSION.  Also, the CPPFLAGS of the Makefile must be passed
through the environment.  This is how we figure out the locations of include
directories. SRCDIR must be passed as well; it explains where to find the C
sources and the include_server directory of Python source files and C
extensions.  Because SRCDIR is appended to build location and we don't want to
end up outside this location through a relative SRCDIR path with '..'s, SRCDIR
is absolutized.
"""

__author__ = 'Manos Renieris and Nils Klarlund'

import setuptools
import doctest
import glob
import os
import shlex
import shutil
import subprocess
import sys

OPTIONS_NOT_ALLOWED = ['-Iquote', '-Isystem', '-I-']

# We include a partial command line parser instead of using the more the more
# complicated one in parse_command.py.  This cuts down on dependencies in the
# build system itself.


def GetIncludes(flags):
  """Parse a flags string for includes of the form -I<DIR>.

  Args:
    flags: a string in shell syntax denoting compiler options
  Returns:
    a list of <DIR>s of the includes
  Raises:
    ValueError:

  In the doctests below, note that a single quoted backslash takes four
  backslashes to represent if it is inside a single quoted string inside this
  present triple-quoted string.

  >>> GetIncludes('-I x -X -I"y" -Y')
  ['x', 'y']
  >>> GetIncludes('-Ix -Dfoo -Iy')
  ['x', 'y']
  >>> GetIncludes(r'-Ix -I"y\\z" -I"y\\\\z" -Y')
  ['x', 'y\\\\z', 'y\\\\z']
  >>> GetIncludes('-DX -Iquote Y')
  Traceback (most recent call last):
        ...
  ValueError: These options are not allowed: -Iquote, -Isystem, -I-.
  >>> GetIncludes('-DX -I x -I')
  Traceback (most recent call last):
        ...
  ValueError: Argument expected after '-I'.
  """
  flags = shlex.split(flags)
  if set(OPTIONS_NOT_ALLOWED) & set(flags):
    raise ValueError('These options are not allowed: %s.'
                     % ', '.join(OPTIONS_NOT_ALLOWED))
  # Fish out the directories of '-I' options.
  i = 0
  inc_dirs = []
  while i < len(flags):
    if flags[i].startswith('-I'):
      inc_dir = flags[i][len('-I'):]
      if inc_dir:
        # "-Idir"
        inc_dirs.append(inc_dir)
        i += 1
        continue
      else:
        # "-I dir"
        if i == len(flags) - 1:
          raise ValueError("Argument expected after '-I'.")
        inc_dirs.append(flags[i+1])
        i += 2
    else:
      i += 1
  return inc_dirs

cpp_flags_env = os.getenv('CPPFLAGS', '')
if not cpp_flags_env:
  # Don't quit; perhaps the user is asking for help using '--help'.
  # CPPFLAGS checking.
  print('setup.py: CPPFLAGS must be defined.', sys.stderr)
# CPPFLAGS is passed to us as it's used in the Makefile: a string that the shell
# will interpret.  GetInclude uses shlex to do the same kind of interpretation
# in order to identify the include directory options.
cpp_flags_includes = GetIncludes(cpp_flags_env)

# SRCDIR checking.
srcdir_env = os.getenv('SRCDIR')
if not srcdir_env:
  # Don't quit; perhaps the user is asking for help using '--help'.
  print('setup.py: SRCDIR must be defined.', sys.stderr)
  srcdir = 'UNDEFINED'
  srcdir_include_server = 'UNDEFINED'
else:
  # The distutils build system appends the source location to the build
  # location, and so to avoid that relative source paths with '..' make built
  # files end up outside the build location, the location is changed to an
  # absolute path.
  srcdir = os.path.abspath(srcdir_env)
  if not os.path.isdir(srcdir):
    sys.exit("""Could not cd to SRCDIR '%s'.""" % srcdir)
  srcdir_include_server = os.path.join(srcdir, 'include_server')

libdir = next(flag.partition('=')[2] for flag in shlex.split(cpp_flags_env)
              if flag.startswith('-DLIBDIR='))

# Specify extension.
ext = setuptools.Extension(
    name='include_server.distcc_pump_c_extensions',
    sources=[os.path.join(srcdir, source)
             for source in
             ['src/clirpc.c',
              'src/clinet.c',
              'src/state.c',
              'src/srvrpc.c',
              'src/pump.c',
              'src/rpc.c',
              'src/io.c',
              'src/lock.c',
              'src/trace.c',
              'src/snprintf.c',
              'src/util.c',
              'src/tempfile.c',
              'src/filename.c',
              'src/bulk.c',
              'src/sendfile.c',
              'src/compress.c',
              'src/compress_stream.c',
              'src/file_cache.c',
              'src/argutil.c',
              'src/cleanup.c',
              'src/emaillog.c',
              'src/timeval.c',
              'src/netutil.c',
              'lzo/minilzo.c',
              'include_server/c_extensions/distcc_pump_c_extensions_module.c',
             ]],
    include_dirs=cpp_flags_includes,
    depends=[__file__, *glob.glob(os.path.join(srcdir, 'src', '*.h'))],
    define_macros=[('_GNU_SOURCE', '1'),
                   ('DCC_CPP_LOOKUP_LIBRARY', '"' + libdir + '/distcc/cpp_lookup.so"')],
    library_dirs=[],
    libraries=['lz4', 'xxhash'],
    runtime_library_dirs=[],
    extra_objects=[],
    extra_compile_args=[]
    )

# The scanner uses the C++ API from the selected LLVM installation. Do not
# silently build a regex-only pump when its declared analysis dependency is absent.
llvm_config = os.environ.get('LLVM_CONFIG') or shutil.which('llvm-config')
if not llvm_config:
  clang = shutil.which('clang')
  if clang:
    llvm_config = os.path.join(os.path.dirname(os.path.realpath(clang)), 'llvm-config')
if not llvm_config or not os.path.isfile(llvm_config):
  sys.exit('libclang-cpp header analysis requires LLVM_CONFIG pointing to llvm-config (LLVM 19, 22 or 23).')
def llvm_option(option):
  return subprocess.check_output([llvm_config, option], text=True).strip()
if llvm_option('--version').split('.')[0] not in ('19', '22', '23'):
  sys.exit('libclang-cpp header analysis requires LLVM 19, 22 or 23 headers and libraries.')
llvm_lib = llvm_option('--libdir')
# Search the target sysroot while linking, but retain its installed path in
# the extension so a cross-built package does not depend on the build machine.
llvm_runtime_lib = llvm_lib
sysroot = os.environ.get('SYSROOT', '').rstrip('/')
if sysroot and llvm_lib.startswith(sysroot + '/'):
  llvm_runtime_lib = llvm_lib[len(sysroot):]
scanner = setuptools.Extension(
    name='include_server.clang_scan',
    sources=[os.path.join(srcdir_include_server, 'c_extensions', 'clang_scan.cpp')],
    language='c++', include_dirs=[llvm_option('--includedir')],
    library_dirs=[llvm_lib], runtime_library_dirs=[llvm_runtime_lib],
    libraries=['clang-cpp', 'LLVM'], extra_compile_args=['-std=c++17', '-g0'],
    define_macros=[('DCC_GCC_SCAN_DIR', '"' + libdir + '/distcc/gcc-scan"')],
)

args = {
    'name': 'include_server',
    # The 'include_server' package is in the srcdir_include_server.
    'package_dir': {'include_server': srcdir_include_server},
    'version': os.getenv('DISTCC_VERSION') or 'unknown',
    'description': """Include server for distcc's pump-mode""",
    'author': 'Nils Klarlund',
    'author_email': 'opensource@google.com',
    'url': 'http://code.google.com/p/distcc',
    'long_description': 'The include server is part of distcc.',
    'packages': ['include_server'],
    'package_data': {'include_server': ['*.pyi']},
    'ext_modules': [ext, scanner],
    'install_requires': ['xxhash>=3.0.0'],
}

# First, do a little self-testing.
doctest.testmod()
# Second, do the setup.
setuptools.setup(**args)
