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

"""Test include analysis, including computed includes, as carried out
by ProcessCompilationCommandLine. Also, test stat_reset_triggers."""

__author__ = "Nils Klarlund"

import os
import re
import shutil
import tempfile
import unittest
from unittest import mock

import basics
import cache_basics
import parse_command
import statistics
import include_analyzer_memoizing_node

class IncludeAnalyzerTest(unittest.TestCase):

  def setUp(self):

    statistics.StartTiming()
    self.global_dirs = []
    basics.opt_print_statistics = False
    basics.opt_debug_pattern = 1
    client_root_keeper = basics.ClientRootKeeper()
    if algorithm == basics.MEMOIZING:
      self.include_analyzer = (
        include_analyzer_memoizing_node.IncludeAnalyzerMemoizingNode(
            client_root_keeper))
    else:
      self.fail("Algorithm not known.")

    statistics.StartTiming()

    self.directory_map = self.include_analyzer.directory_map
    self.compiler_defaults = self.include_analyzer.compiler_defaults
    self.canonical_path = self.include_analyzer.canonical_path

  def tearDown(self):
    if basics.opt_print_statistics:
      statistics.EndTiming()
      statistics.PrintStatistics(self.include_analyzer)

  def ProcessCompilationCommandLine(self, cmd, cwd):
    return (
      self.include_analyzer.ProcessCompilationCommand(
        cwd,
        parse_command.ParseCommandArgs(
          parse_command.ParseCommandLine(cmd),
          cwd,
          self.include_analyzer.includepath_map,
          self.include_analyzer.directory_map,
          self.include_analyzer.compiler_defaults)))

  def CanonicalPathsForTestData(self, dirs, test_data_dir='test_data'):
    """Absolutize names relative to test_data_dir of current directory."""
    return set([ self.canonical_path.Canonicalize(test_data_dir + '/' + f)
                 for f in dirs ])


  def RetrieveCanonicalPaths(self, files):
    return set([ self.include_analyzer.realpath_map.string[f] for f in files ])


  def test_AdvancedComputedIncludes(self):

    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        "gcc test_data/test_computed_includes/src.c",
        os.getcwd()))

    system = self.include_analyzer.compiler_defaults.system_dirs_default_all
    system_inputs = {path for path in includes
                     if any(path.startswith(directory.rstrip('/') + '/') for directory in system)}
    self.assertTrue(any(path.endswith('/stdio.h') for path in system_inputs))
    self.assertEqual(
      includes - system_inputs,
      self.CanonicalPathsForTestData(['test_computed_includes/src.c',
                                      'test_computed_includes/helper.c',
                                      'test_computed_includes/incl.h']))

    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        "gcc test_data/test_computed_includes/srcA.c",
        os.getcwd()))

    self.assertTrue(system_inputs <= includes)
    self.assertEqual(
      includes - system_inputs,
      self.CanonicalPathsForTestData(['test_computed_includes/srcA.c',
                                      'test_computed_includes/helper.c',
                                      'test_computed_includes/incl.h',
                                      'test_computed_includes/inclA.h']))

    # Test: FindNode is called once only if previous query is repeated. That is,
    # include graph is not calculated for this query.
    old_FindNode = self.include_analyzer.FindNode
    class mock_FindNode(object):
      def __init__(self):
        self.count = 0
      def FindNode(self, *_):
        self.count += 1
        if self.count == 2:
          raise Exception("Did not expect 2 calls of FindNode.")
        return old_FindNode(*_)

    self.include_analyzer.FindNode = mock_FindNode().FindNode
    try:
      includes = self.RetrieveCanonicalPaths(
        self.ProcessCompilationCommandLine(
        "gcc test_data/test_computed_includes/srcA.c",
        os.getcwd()))
    finally:
      self.include_analyzer.FindNode = old_FindNode

    # Test: if a -D option affecting the value of the computed include is
    # presented then include graph is recalculated --- and correctly so.
    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        r"""gcc -DINCL=\"../dfoo/foo2.h\" """ +
        " test_data/test_computed_includes/src.c",
        os.getcwd()))

    self.assertEqual(
      includes - system_inputs,
      self.CanonicalPathsForTestData(['test_computed_includes/src.c',
                                      'test_computed_includes/helper.c',
                                      'test_computed_includes/incl.h',
                                      'test_computed_includes/inclA.h',
                                      'dfoo/foo2.h']))


    # Test: functional macros can be passed on the command line.
    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        """gcc  -D"STR(X)=# X" """
        + """-D"FINCLUDE(P)=STR(../MY_TEST_DATA/dfoo/P)" """
        + """-DMY_TEST_DATA=test_data """
        + "test_data/func_macro.c",
        os.getcwd()))

    self.assertEqual(
      includes - system_inputs,
      self.CanonicalPathsForTestData(
        ['func_macro.c',
         'dfoo/foo.h',
         'dfoo/foo2.h',
         'dbar/dbar1/bar.h'
         ]))


  def test_AbsoluteIncludes(self):

    try:
      opt_unsafe_absolute_includes = basics.opt_unsafe_absolute_includes
      basics.opt_unsafe_absolute_includes = True

      tmp_dir = tempfile.mkdtemp()

      def WriteProgram(name, text):
        base = os.path.dirname(os.path.join(tmp_dir, name))
        if not os.path.isdir(base):
          os.makedirs(base)
        fd = open(os.path.join(tmp_dir, name), 'w')
        fd.write(text)
        fd.close()

      WriteProgram('foo.c',
                   """#include "%s/bar/baz.h"
                      blah blah.
                   """ % tmp_dir)

      # We don't want to get entangled in absolute includes.
      # The baz.h program is not supposed to be found (although
      # real preprocessing will).

      WriteProgram('baz.h',
                   """#include "../foobar.h"
                      blah blah.
                   """)

      includes = self.RetrieveCanonicalPaths(
          self.ProcessCompilationCommandLine(
          "gcc %s" % os.path.join(tmp_dir, 'foo.c'),
          os.getcwd()))

      self.assertEqual(
          includes,
          set([os.path.realpath('%s/foo.c' % tmp_dir)]))

    finally:
      basics.opt_unsafe_absolute_includes = opt_unsafe_absolute_includes
      shutil.rmtree(tmp_dir)

  def test_StatResetTriggers(self):
    """Explicit triggers rebuild the graph against actual generated headers."""
    def CheckFiles(files, expected, generation):
      names = [os.path.basename(os.path.dirname(path)) + '/' +
               os.path.basename(path)[:-4] for path in files if path.endswith('.lzo')]
      self.assertEqual(names, ['test_data/stat_triggers.c', expected])
      self.assertEqual(self.include_analyzer.generation, generation)
      for path in files:
        self.assertRegex(path, r'[.]include_server-[0-9]+-%d/' % generation)

    previous = os.getcwd()
    with tempfile.TemporaryDirectory() as root:
      data = os.path.join(root, 'test_data')
      os.makedirs(os.path.join(data, 'dfoo'))
      with open(os.path.join(data, 'stat_triggers.c'), 'w') as source:
        source.write('#include "stat_triggers.h"\n')
      with open(os.path.join(data, 'dfoo', 'stat_triggers.h'), 'w') as header:
        header.write('#define VALUE 1\n')
      trigger = os.path.join(root, 'ate')
      with open(trigger, 'w') as stream:
        stream.write('initial\n')
      self.include_analyzer.stat_reset_triggers = {
          os.path.join(root, 'seven*'): {},
          trigger: {trigger: basics.Stamp(trigger)},
          os.path.join(root, 'nine'): {}}
      command = 'gcc -Itest_data/dfoo test_data/stat_triggers.c'.split()
      try:
        files = self.include_analyzer.DoCompilationCommand(
            command, root, self.include_analyzer.client_root_keeper)
        CheckFiles(files, 'dfoo/stat_triggers.h', 1)

        generated = os.path.join(data, 'stat_triggers.h')
        with open(generated, 'w') as header:
          header.write('#define VALUE 2\n')
        stamp = os.stat(trigger)
        os.utime(trigger, ns=(stamp.st_atime_ns, stamp.st_mtime_ns + 1000000000))
        # Prove the explicit trigger resets the generation before normal
        # per-request filesystem invalidation gets an opportunity to do so.
        self.include_analyzer.DoStatResetTriggers()
        self.assertEqual(self.include_analyzer.generation, 2)
        files = self.include_analyzer.DoCompilationCommand(
            command, root, self.include_analyzer.client_root_keeper)
        CheckFiles(files, 'test_data/stat_triggers.h', 2)

        os.unlink(generated)
        with open(os.path.join(root, 'seventy'), 'w') as stream:
          stream.write('new trigger\n')
        self.include_analyzer.DoStatResetTriggers()
        self.assertEqual(self.include_analyzer.generation, 3)
        files = self.include_analyzer.DoCompilationCommand(
            command, root, self.include_analyzer.client_root_keeper)
        CheckFiles(files, 'dfoo/stat_triggers.h', 3)
      finally:
        os.chdir(previous)


  def test_DotdotInInclude(self):
    """Set up tricky situation involving an "#include "../foo" occurring in a
    file accessed through a symbolic link.  This include is to be resolved
    relative to the file directory, namely where the link is, not where the file
    is.  Also, the include processor tries to identify files by their
    relatives names. This may lead to an infinite recursion according to an
    increasing sequence of filepath names involving "../" So if the included
    file includes the original file, an infinite loop may occur."""

    # In test_data, we find
    #
    #   symlink_farm/sub_farm/link_to_dd_dd_dfoo_include_dotdot_foo
    #
    # which is a link to ../../dfoo/include_dotdot_foo, that is, to
    #
    #   dfoo/include_dotdot_foo
    #
    # which is a file that contains:
    #
    #   #include "../foo"
    #
    # Here ../foo must refer to the file in location symlink_farm/sub_farm/foo,
    # which we have made a real file (not a symlink).
    #
    # We check that this "foo" is included. Moreover, we have put:
    #
    #   #include "sub_farm/link_to_dd_dd_dfoo_include_dotdot_foo"
    #
    #  into symlink_farm/sub_farm/foo, so that a nice infinite inclusion chain
    #  is modeled. That should not faze the include server. In particular, we
    #  do not want to see an infinitude of paths of the form:
    #
    #      symlink_farm/sub_farm/../sub_farm/../
    #      ...
    #      /sub_farm/../sub_farm/foo

    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        "gcc -xc test_data/symlink_farm/sub_farm/"
        + "link_to_dd_dd_dfoo_include_dotdot_foo",
        os.getcwd()))
    self.assertEqual(
      includes,
      self.CanonicalPathsForTestData(
        ['dfoo/include_dotdot_foo', 'symlink_farm/foo'],
        "test_data"))


  def helper_test_IncludeAnalyzer(self, test_data_dir):
    """Test basic functionality assuming test data is in test_data_dir."""

    # Simple stuff: quoted and angled directories, recursion
    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        "gcc -Itest_data/dfoo test_data/parse.c",
        os.getcwd()))
    self.assertEqual(
      includes,
      self.CanonicalPathsForTestData(
        ['parse.c',
         'dfoo/foo2.h',
         'dfoo/foo.h',
         'dfoo/../dbar/dbar1/bar.h'],
        test_data_dir))

    # Computed includesgg

    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        "gcc -Itest_data/dfoo test_data/computed_includes.c",
        os.getcwd()))

    # The path to the computed angle includes was not provided, so
    # they were not found.
    self.assertEqual(
      includes,
      self.CanonicalPathsForTestData(
        ['computed_includes.c', 'p1.h'],
        test_data_dir))

    # Check that symbol_table has been updated.
    self.assertEqual(self.include_analyzer.symbol_table,
                     {'dfoo_foo2_h': [None],
                      'A': ['"p1.h"'],
                      'dbar_dbar1_bar_h': [None],
                      'm': [(['a'], '<a##_pre.c>'), (['a'], '<a##_post.c>')]})


    # The include path to the angle includes, -Itest_data, is
    # provided at our next try. This should include abc_pre.c and
    # abc_post.c.  This is especially challenging for incremental
    # analysis because the previous cached result of include analyzing
    # computed_includes.c cannot in fact be reused here. That is an
    # unusual case.
    includes = self.RetrieveCanonicalPaths(
      self.ProcessCompilationCommandLine(
        "gcc -Itest_data/dfoo -Itest_data"
        + " test_data/computed_includes.c",
        os.getcwd()))

    self.assertEqual(
      includes,
      self.CanonicalPathsForTestData(
        ['computed_includes.c', 'p1.h', 'abc_post.c', 'abc_pre.c'],
        test_data_dir))

   # The inclusion chain is baz/start_x -> baz/x.h -> foo/x.h ->
   # bar/x.h -> baz.h.  Only the final #include_next "x.h" in baz/x.h
   # does not Resolve because we just used -Ibaz for resolution, the
   # last in the list. This should not raise an exception, because the
   # failure may in reality not happen: the bad include_next could be
   # guarded by a conditional for example.
    self.assertEqual(
      self.RetrieveCanonicalPaths(self.ProcessCompilationCommandLine(
          "gcc -E  -Itest_data/test_include_next/foo"
          + " -Itest_data/test_include_next/bar"
          + " -Itest_data/test_include_next/baz"
          + " test_data/test_include_next/baz/start_x.c",
          os.getcwd())),
      self.CanonicalPathsForTestData(
        ['test_include_next/baz/start_x.c',
         'test_include_next/baz/x.h',
         'test_include_next/foo/x.h',
         'test_include_next/bar/x.h',
         'test_include_next/baz/x.h'],
        test_data_dir))

    # In contrast to previous example, the #final include_next in baz
    # resolves this time, namely to biz/x.h.
    self.assertEqual(
      self.RetrieveCanonicalPaths(self.ProcessCompilationCommandLine(
          "gcc -E  -Itest_data/test_include_next/foo"
          + " -Itest_data/test_include_next/bar"
          + " -Itest_data/test_include_next/baz"
          + " -Itest_data/test_include_next/biz"
          + " test_data/test_include_next/baz/start_x.c",
          os.getcwd())),
      self.CanonicalPathsForTestData(
        ['test_include_next/baz/start_x.c',
         'test_include_next/baz/x.h',
         'test_include_next/foo/x.h',
         'test_include_next/bar/x.h',
         'test_include_next/biz/x.h'],
        test_data_dir))


    includes = self.RetrieveCanonicalPaths(self.ProcessCompilationCommandLine(
          "gcc -E  -Itest_data/test_include_next/foo"
          + " -Itest_data/test_include_next/bar"
          + " -Itest_data/test_include_next/baz"
          + " test_data/test_include_next/baz/start_y.c",
          os.getcwd()))

    self.assertEqual(
      includes,
      self.CanonicalPathsForTestData(
        ['test_include_next/baz/start_y.c',
         'test_include_next/baz/../foo/y.h',
         'test_include_next/bar/y.h',
         'test_include_next/foo/y.h',
         'test_include_next/baz/y.h'],
        test_data_dir))

    # Test that a directory that has a name matching an include is not picked.
    # Here the directory is test_data/i_am_perhaps_a_directory.h, which is in
    # the file directory of the translation unit. Instead,
    # test_data/dfoo/i_am_perhaps_a_directory.h should be picked: it is a
    # regular file.
    includes = self.RetrieveCanonicalPaths(self.ProcessCompilationCommandLine(
          "gcc -E  -Itest_data/dfoo test_data/test_directory_probing.c",
          os.getcwd()))

    self.assertEqual(
      includes,
      self.CanonicalPathsForTestData(
        ['test_directory_probing.c', 'dfoo/i_am_perhaps_a_directory.h'],
        test_data_dir))

  def test_IncludeAnalyzer(self):
    """Run helper_test_IncludeAnalyzer 'directly' without complications
    of symbolic links."""
    self.helper_test_IncludeAnalyzer('test_data')


  def test_IncludeAnalyzer_from_symlink_farm(self):
    """Run helper_test_IncludeAnalyzer through a link farm to exercise include
    processors ability to handle links."""
    self.helper_test_IncludeAnalyzer('test_data/symlink_farm')


class FileStampCacheTest(unittest.TestCase):
  def test_directory_replacement_is_detected(self):
    with tempfile.TemporaryDirectory() as root:
      directory = os.path.join(root, 'include')
      os.mkdir(directory)
      cache = cache_basics.FileStampCache()
      cache.Observe(directory)
      os.rename(directory, directory + '-old')
      os.mkdir(directory)
      self.assertTrue(cache.Changed())

  def test_directory_probe_ignores_outputs_but_tracks_headers(self):
    for notifications in (False, True):
      with self.subTest(notifications=notifications), tempfile.TemporaryDirectory() as root:
        cache = cache_basics.FileStampCache()
        if not notifications and cache.watch_fd >= 0:
          os.close(cache.watch_fd)
          cache.watch_fd = -1
        cache.Observe(root)
        header = os.path.join(root, 'generated.h')
        cache.Observe(header)
        with open(os.path.join(root, 'unrelated.o'), 'w') as output:
          output.write('output')
        self.assertFalse(cache.Changed())
        with open(header, 'w') as output:
          output.write('header')
        self.assertTrue(cache.Changed())

  def test_dangling_symlink_target_created_elsewhere(self):
    with tempfile.TemporaryDirectory() as root:
      os.mkdir(os.path.join(root, 'targets'))
      target = os.path.join(root, 'targets', 'generated')
      alias = os.path.join(root, 'include')
      os.symlink(target, alias)
      cache = cache_basics.FileStampCache()
      cache.Observe(os.path.join(alias, 'header.h'))
      self.assertFalse(cache.Changed())
      os.mkdir(target)
      self.assertTrue(cache.Changed())

  def test_negative_lookups_use_notifications(self):
    with tempfile.TemporaryDirectory() as root:
      cache = cache_basics.FileStampCache()
      for index in range(256):
        cache.Observe(os.path.join(root, 'missing%d.h' % index))
      if cache.watch_fd < 0:
        self.skipTest('filesystem notifications unavailable')
      with open(os.path.join(root, 'unrelated.o'), 'w') as output:
        output.write('output')
      with mock.patch.object(cache, 'Stamp', wraps=cache.Stamp) as stamp:
        self.assertFalse(cache.Changed())
        self.assertLess(stamp.call_count, 8)
      with open(os.path.join(root, 'missing17.h'), 'w') as header:
        header.write('generated')
      self.assertTrue(cache.Changed())

  def test_hardlink_write_and_missing_ancestor(self):
    with tempfile.TemporaryDirectory() as root:
      header = os.path.join(root, 'header.h')
      with open(header, 'w') as output:
        output.write('one')
      alias_dir = os.path.join(root, 'other')
      os.mkdir(alias_dir)
      alias = os.path.join(alias_dir, 'alias.h')
      os.link(header, alias)
      cache = cache_basics.FileStampCache()
      cache.Observe(header)
      self.assertFalse(cache.Changed())
      with open(alias, 'w') as output:
        output.write('two')
      self.assertTrue(cache.Changed())
      cache = cache_basics.FileStampCache()
      cache.Observe(os.path.join(root, 'generated', 'header.h'))
      self.assertFalse(cache.Changed())
      os.mkdir(os.path.join(root, 'generated'))
      self.assertTrue(cache.Changed())


for algorithm in [ basics.MEMOIZING ]:
  try:
    print("TESTING ALGORITHM %s" % algorithm)
    unittest.main()
  except:
    raise
