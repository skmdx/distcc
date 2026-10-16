#!/usr/bin/env python3
"""Build the GCC dependency module against the selected compiler's source/ABI."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--gcc', default='gcc', help='compiler that will load the module')
  parser.add_argument('--source', type=Path, required=True, help='matching GCC source tree')
  parser.add_argument('--plugin-headers', type=Path,
                      help='target-native headers when cross-building the module')
  parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
  parser.add_argument('--output', type=Path, required=True,
                      help='module root; target/version subdirectories are created')
  args = parser.parse_args()
  gcc = shlex.split(args.gcc)
  def query(option):
    return subprocess.check_output([*gcc, option], text=True).strip()
  target, version = query('-dumpmachine'), query('-dumpfullversion')
  headers = args.plugin_headers or Path(query('-print-file-name=plugin')) / 'include'
  source = args.source.resolve()
  if (source / 'gcc/BASE-VER').read_text().strip() != version:
    parser.error('GCC source and compiler versions differ')
  date = (source / 'gcc/DATESTAMP').read_text().strip()
  if len(date) != 8 or not date.isdecimal():
    parser.error('Invalid GCC source date')
  if (source / 'libcpp/include/cpplib.h').read_bytes() != (headers / 'cpplib.h').read_bytes():
    parser.error('GCC source and installed plugin headers differ')
  for name in (target, version):
    if not name or '/' in name or name in ('.', '..'):
      parser.error('Invalid GCC target/version')
  output = args.output.resolve() / target / version
  output.mkdir(parents=True, exist_ok=True)
  compiler = shlex.split(args.cxx)
  # Do not inherit distcc's include paths: its config.h is not GCC's config.h.
  flags = ['-std=gnu++17', '-O2', '-fPIC', '-fno-rtti', '-fno-exceptions',
           '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections',
           '-DDCC_GCC_SOURCE_DATE="' + date + '"',
           '-I' + str(headers), '-I' + str(source / 'libcpp'),
           '-I' + str(source / 'libcpp/include'), '-I' + str(source / 'gcc')]
  root = Path(__file__).resolve().parent.parent
  objects = []
  for name in ('gcc_scan', 'gcc_scan_inputs'):
    obj = output / (name + '.o')
    subprocess.run([*compiler, *flags, '-c', str(root / 'src' / (name + '.cc')),
                    '-o', str(obj)], check=True)
    objects.append(str(obj))
  module = output / 'distcc_gcc_scan.so'
  subprocess.run([*compiler, '-shared', '-Wl,--gc-sections', *objects,
                  '-o', str(module)], check=True)
  print(module)


if __name__ == '__main__':
  main()
