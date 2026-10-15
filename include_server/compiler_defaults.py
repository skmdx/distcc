#! /usr/bin/env python3

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


"""Discover default compiler include directories for conservative traversal."""

__author__ = "Nils Klarlund"


import os
import re
import sys
import basics
import subprocess

Debug = basics.Debug
DEBUG_TRACE = basics.DEBUG_TRACE
DEBUG_DATA = basics.DEBUG_DATA
NotCoveredError = basics.NotCoveredError


def _SystemSearchdirsGCC(compiler, sysroot, language, canonical_lookup):
  """Run gcc on empty file; parse output to figure out default paths.

  This function works only for gcc, and only some versions at that.

  Arguments:
    compiler: a filepath (the first argument on the distcc command line)
    sysroot: the --sysroot passed to the compiler ("" to disable)
    language: 'c' or 'c++' or other item in basics.LANGUAGES
    canonical_lookup: a function that maps strings to their realpaths
  Returns:
    Existing system search dirs and default directories rejected as missing.

  """

  # We are trying to wring the following kind of text out of the
  # compiler:
  #--------------------
  # blah. blah.
  # ...
  # blah. blah.
  # #include "..." search starts here:
  # #include <...> search starts here:
  #  /usr/local/include
  #  /usr/lib/gcc/i486-linux-gnu/4.0.3/include
  #  /usr/include
  # End of search list.
  # blah. blah.
  #------------

  command = [compiler]
  if sysroot:
    command += ["--sysroot=" + sysroot]
  # Include discovery needs no object output. Preprocessing also lets ccache
  # pass through without opening a cache outside the build sandbox.
  command += ["-x", language, "-v", "-E", "/dev/null", "-o", "/dev/null"]
  Debug(DEBUG_DATA, "system search dirs command: %s" % command)

  try:
    # We clear the environment, because otherwise, directories
    # declared by CPATH, for example, will be incorporated into the
    # result. (See the CPP manual for the meaning of CPATH.)  The only
    # thing we keep is PATH, so we can be sure to find the compiler.
    # The recursion guard makes probes local without taking compilation slots
    # when PATH contains distcc's compiler symlinks.
    # TODO(csilvers): it's possible we could need to pass in some
    # other environment vars, like LD_LIBRARY_PATH.  Instead of adding
    # in more env-vars by hand, consider just removing from os.environ
    # all the env-vars that are meaningful to gcc, such as CPATH.  See
    # http://docs.freebsd.org/info/gcc/gcc.info.Environment_Variables.html,
    # or the "Environment Variables Affecting GCC" section of the gcc
    # info page.
    #
    trimmed_env = {'_DISTCC_SAFEGUARD': '1'}

    if 'PATH' in os.environ:
      trimmed_env['PATH'] = os.environ['PATH']

    # DEVELOPER_DIR is needed to locate compiler on macOS; preserve it
    if 'DEVELOPER_DIR' in os.environ:
      trimmed_env['DEVELOPER_DIR'] = os.environ['DEVELOPER_DIR']

    p = subprocess.Popen(command,
                         shell=False,
                         stdin=None,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT,
                         env=trimmed_env,universal_newlines=True)
    out = p.communicate()[0]
  except (IOError, OSError) as why:
    raise NotCoveredError (
             ( "Couldn't determine default system include directories\n"
             + "for compiler '%s', language '%s':\n"
             + "error executing '%s': %s.")
             % (compiler, language, command, why))

  if p.returncode != 0:
    raise NotCoveredError(
             ( "Couldn't determine default system include directories\n"
             + "for compiler '%s', language '%s':\n"
             + "command '%s' exited with status '%d'.\n Command output:\n%s") %
             (compiler, language, command, p.returncode, out))

  match_obj = re.search(
    "%s\n(.*?)\n%s"  # don't ask
    % ("#include <...> search starts here:", "End of search list"),
    out,
    re.MULTILINE + re.DOTALL)
  if match_obj == None:
    raise NotCoveredError(
             ( "Couldn't determine default system include directories\n"
             + "for compiler '%s', language '%s':\n"
             + "couldn't parse output of '%s'.\nReceived:\n%s") %
             (compiler, language, command, out))
  directories = [ canonical_lookup(directory)
           for line in match_obj.group(1).split("\n")
           for directory in line.split()
           # Ignore Apple-modified MacOS gcc's "framework" directories.
           if not line.endswith(" (framework directory)")
           ]
  missing = {canonical_lookup(directory) for directory in
             re.findall(r'^\s*ignoring nonexistent directory "([^"]+)"', out, re.M)}
  return directories, missing
           # TODO: Rather than just ignoring framework directories, we
           # should handle them properly, fully emulating the search
           # algorithm used by Apple's modified GCC.
           # The search algorithm used for framework directories is not very
           # well documented, as far as I can tell, but the source code is in
           # gcc/config/darwin-c.c in the Apple GCC sources.
           # From a quick glance, I think it looks like this:
           # - For each #include of the form Foo/bar.h,
           #        For each framework directory Baz,
           #            Look in Baz/Foo.framework/Headers/bar.h
           #            and in Baz/Foo.framework/PrivateHeaders/bar.h
           # - If the regular search fails, look for subframeworks.
           #     For each #include of the form Foo/bar.h
           #       from Baz/Quux.framework/Headers/whatever.h
           #            Look in Baz/Quux.framework/Frameworks/Foo/Headers/bar.h.

class CompilerDefaults(object):
  """Cache compiler default directories for conservative include traversal."""

  def __init__(self, canonical_lookup):
    """Constructor.

    Instance variables:
      system_dirs_real_paths: a dictionary such that
        system_dirs_real_paths[c][lang] is a list of directory paths
        (strings) for compiler c and language lang
      system_dirs_default: a list of all such strings, subjected to
        realpath-ification, for all c and lang
    """
    self.canonical_lookup = canonical_lookup
    self.system_dirs_default_all = set([])
    self.system_dirs_default = {}
    self.missing_system_dirs = set()

  def SetSystemDirsDefaults(self, compiler, sysroot, language, timer=None):
    """Cache default search directories for this compiler and target.

    Arguments:
      compiler: a filepath (the first argument on the distcc command line)
      sysroot: the --sysroot passed to the compiler ("" to disable)
      language: 'c' or 'c++' or other item in basics.LANGUAGES
      timer: a basis.IncludeAnalyzerTimer or None

    The timer will be disabled during this routine because the select involved
    in Popen calls does not handle SIGALRM.

    See also the class documentation for this class.
    """
    assert isinstance(compiler, str)
    assert isinstance(language, str)
    Debug(DEBUG_TRACE,
          "SetSystemDirsDefaults with CC, SYSROOT, LANG: %s, %s, %s" %
          (compiler, sysroot, language))
    if compiler in self.system_dirs_default:
      if sysroot in self.system_dirs_default[compiler]:
        if language in self.system_dirs_default[compiler][sysroot]:
          return
      else:
        self.system_dirs_default[compiler][sysroot] = {}
    else:
      self.system_dirs_default[compiler] = {sysroot: {}}
    try:
      if timer:
        # We have to disable the timer because the select system call that is
        # executed when calling the compiler through Popen gives up if presented
        # with a SIGALRM.
        timer.Stop()
      directories, missing = _SystemSearchdirsGCC(compiler, sysroot, language,
                                                 self.canonical_lookup)
      self.system_dirs_default[compiler][sysroot][language] = directories
      self.missing_system_dirs.update(missing)
      Debug(DEBUG_DATA,
            "system_dirs_default[%s][%s][%s]: %s" %
            (compiler, sysroot, language,
             self.system_dirs_default[compiler][sysroot][language]))
      # Now summarize what we know and add to system_dirs_default_all.
      self.system_dirs_default_all |= (
          set(self.system_dirs_default[compiler][sysroot][language]))
    finally:
      if timer:
        timer.Start()
