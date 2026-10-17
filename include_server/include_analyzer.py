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

"""The skeleton for an include analyzer.

This module defines the basic caches and helper functions for an
include analyzer.
"""

__author__ = "Nils Klarlund"

import os
import glob

import basics
import parse_command
import cache_basics
import mirror_path
import compress_files
import include_server

Debug = basics.Debug
DEBUG_TRACE = basics.DEBUG_TRACE
NotCoveredError = basics.NotCoveredError

class CacheInvalidated(Exception):
  """An input generation changed before its manifest could be published."""

class IncludeAnalyzer(object):
  """The skeleton, including caches, of an include analyzer."""

  def _InitializeAllCaches(self):
    # Erect the edifice of caches.
    caches = self.caches = (
        cache_basics.SetUpCaches(self.client_root_keeper.client_root))

    # Migrate the cache stuff to self namespace.
    self.includepath_map = caches.includepath_map
    self.directory_map = caches.directory_map
    self.realpath_map = caches.realpath_map

    self.canonical_path = caches.canonical_path
    self.dirname_cache = caches.dirname_cache
    self.compiler_defaults = caches.compiler_defaults
    self.systemdir_prefix_cache = caches.systemdir_prefix_cache

    self.simple_build_stat = caches.simple_build_stat
    self.build_stat_cache = caches.build_stat_cache

    self.IsIncludepathIndex = caches.IsIncludepathIndex
    self.IsSearchdirIndex = caches.IsSearchdirIndex
    self.IsCurrdirIndex = caches.IsCurrdirIndex
    self.IsRealpathIndex = caches.IsRealpathIndex
    self.IsFilepathPair = caches.IsFilepathPair

    # Make a cache for the symbolic links encountered; also for their
    # replication into root directory.
    self.mirror_path = mirror_path.MirrorPath(self.simple_build_stat,
                                              self.canonical_path)
    # Make a compressor for source files.
    self.compress_files = compress_files.CompressFiles(self.includepath_map,
                                                       self.directory_map,
                                                       self.realpath_map,
                                                       self.mirror_path)

    # For statistics only. We measure the different search lists
    # (search paths) by accumulating them all in sets.
    self.quote_dirs_set = set([]) # quote search lists
    self.angle_dirs_set = set([]) # angle searchlists
    self.include_dir_pairs = set([]) # the pairs (quote search list,
                                     # angle search lists)

  def __init__(self, client_root_keeper, stat_reset_triggers={}):
    self.generation = 1
    self.client_root_keeper = client_root_keeper
    self.client_root_keeper.ClientRootMakedir(self.generation)
    self.stat_reset_triggers = stat_reset_triggers
    self.translation_unit = "unknown translation unit"
    self.current_input_paths = ()
    self.include_server_cwd = os.getcwd()
    self._InitializeAllCaches()

  def DoStatResetTriggers(self):
    """Reset stat caches if a glob evaluates differently from earlier.

    More precisely, if a path of a glob comes in or out of existence or has a
    new stamp, then reset stat caches."""

    trigger_map = self.stat_reset_triggers
    old_paths = [ path
                  for glob_expr in trigger_map
                  for path in trigger_map[glob_expr] ]
    for glob_expr in trigger_map:
      for path in glob.glob(glob_expr):
        try:
          old_paths.remove(path)
        except ValueError:
          pass
        new_stamp = basics.Stamp(path)
        if path in trigger_map[glob_expr]:
          if new_stamp != trigger_map[glob_expr][path]:
            Debug(basics.DEBUG_WARNING,
                  "Path '%s' changed. Clearing caches.",
                  path)
            trigger_map[glob_expr][path] = new_stamp
            self.ClearStatCaches()
            return
        else:
          Debug(basics.DEBUG_WARNING,
                "Path '%s' came into existence. Clearing caches.",
                path)
          trigger_map[glob_expr][path] = basics.Stamp(path)
          self.ClearStatCaches()
          return
    if old_paths:
      path = old_paths[0]
      Debug(basics.DEBUG_WARNING,
            "Path '%s' no longer exists. Clearing caches.",
            path)
      self.ClearStatCaches()

  def DoCompilationCommand(self, cmd, currdir, client_root_keeper,
                           compression='lzo'):
    """Parse and and process the command; then gather files and links."""

    self.translation_unit = "unknown translation unit"  # don't know yet

    # Any relative paths in the globs in the --stat_reset_trigger argument
    # must be evaluated relative to the include server's original working
    # directory.
    os.chdir(self.include_server_cwd)
    self.DoStatResetTriggers()
    # The compiler will report fresh content identities. Retire stale path
    # mappings now, then validate this request's contents before publication.
    if self.build_stat_cache.file_stamps.Changed(()):
      self.ClearStatCaches()

    # Now change to the distcc client's working directory.
    # That'll let us use os.path.join etc without including currdir explicitly.
    os.chdir(currdir)

    parsed_command = (
        parse_command.ParseCommandArgs(cmd,
                                       currdir,
                                       self.includepath_map,
                                       self.directory_map,
                                       self.compiler_defaults))
    (unused_quote_dirs, unused_angle_dirs, unused_include_files, source_file,
     result_file_prefix, unused_Dopts) = parsed_command

    # Do the real work.
    include_closure = (
      self.ProcessCompilationCommand(currdir, parsed_command))
    # Get name of the initial source file
    translation_unit = self.translation_unit
    # Links are accumulated intra-build (across different compilations in a
    # build). We send all of 'em every time.  This will potentially lead to
    # performance degradation for large link farms. We expect at most a
    # handful. We add put the system links first, because there should be very
    # few of them.
    links = self.mirror_path.Links()
    files = self.compress_files.Compress(include_closure, client_root_keeper,
                                         self.currdir_idx, '-E' in cmd, compression,
                                         getattr(self, 'snapshots', None))

    files_and_links = files + links

    # Note that the performance degradation comment above applies especially
    # to forced include directories, unless disabled with --no_force_dirs
    if basics.opt_no_force_dirs == False:
      files_and_links += self._ForceDirectoriesToExist()

    # Never publish a manifest assembled from changing inputs. Previous
    # manifests keep their immutable generation until the build finishes.
    if self.build_stat_cache.file_stamps.Changed(self.current_input_paths):
      raise CacheInvalidated()

    realpath_map = self.realpath_map

    if basics.opt_verify:
      # Invoke the real preprocessor.
      exact_dependencies = (
        include_server.ExactDependencies(" ".join(cmd),
                          realpath_map,
                          translation_unit))
      if basics.opt_write_include_closure:
        include_server.WriteDependencies(exact_dependencies,
                          self.result_file_prefix + '.d_exact',
                          realpath_map)
      include_server.VerifyExactDependencies(include_closure,
                              exact_dependencies,
                              realpath_map,
                              translation_unit)
    if basics.opt_write_include_closure:
      include_server.WriteDependencies(include_closure,
                        self.result_file_prefix + '.d_approx',
                        realpath_map)
    return files_and_links

  def _ForceDirectoriesToExist(self):
    """Force any needed directories to exist.

    In rare cases, the source files may contain #include "foo/../bar",
    but may not contain any other files from the "foo" directory.
    In such cases, we invent a dummy file in (the mirrored copy of)
    each such directory, just to force the distccd server to create the
    directory, so that the C compiler won't get an error when it tries
    to resolve that #include.

    Returns:
      A list of files to pass as dummy inputs.
    """

    must_exist_dirs = self.mirror_path.MustExistDirs()
    # Note: distcc's --scan-includes option needs to
    # know about this name; see ../src/compile.c.
    special_name = 'forcing_technique_271828'
    forcing_files = [d + '/' + special_name
                     for d in must_exist_dirs]
    # Note: we used to create these here, but as of v3.4, we let
    # the distcc client ask the server to create them to avoid
    # a bottleneck in include processing due to many open() calls
    return forcing_files

  def ClearStatCaches(self):
    """Clear caches used for, or dependent on, stats."""
    self.generation += 1
    # Tabula rasa: for this analysis, we must forget everything recorded in the
    # client_root directory about source files, directories, and symbolic links.
    # But we cannot delete any such information, because slow-poke distcc
    # clients that have received earlier include manifests perhaps only now get
    # around to reading a previous generation client root directory.
    self.client_root_keeper.ClientRootMakedir(self.generation)
    self._InitializeAllCaches()
