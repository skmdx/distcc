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


"""Tests for parse_file."""

__author__ = "opensource@google.com"

import unittest
import tempfile
from pathlib import Path

import basics
import cache_basics
import parse_file
import include_server
import include_analyzer

class parse_file_Test(unittest.TestCase):

  def setUp(self):
    include_server.print_statistics = False
    client_root_keeper = basics.ClientRootKeeper()
    include_server.write_include_closure_file = True
    self.include_analyzer = include_analyzer.IncludeAnalyzer(client_root_keeper)

  def tearDown(self):
    pass

  def test_lexical_directives_and_macro_adjacency(self):
    with tempfile.TemporaryDirectory() as root:
      source = Path(root) / 'source.cpp'
      source.write_text(
          '/*\n#include "comment.h"\n*/\n'
          'const char *s = R"raw(\n#include "string.h"\n)raw";\n'
          '#\\\ninc\\\nlude "split.h"\n'
          '%:include <angle//header.h>\n'
          '# include_next <next.h>\n'
          '#define FUNCTION(x) x\n#define OBJECT /**/ (x)\n'
          '#if __has_include("probe.h")\n#endif\n')
      paths = cache_basics.MapToIndex()
      symbols = {}
      quote, angle, expr, following = parse_file.ParseFile(paths).Parse(str(source), symbols)
      self.assertEqual([paths.string[p] for p in quote], ['split.h'])
      self.assertEqual([paths.string[p] for p in angle], ['angle//header.h'])
      self.assertEqual([paths.string[p] for p in following], ['next.h'])
      self.assertEqual(expr, [parse_file.ProbeExpression('__has_include("probe.h")')])
      self.assertEqual(symbols['FUNCTION'], [(['x'], 'x')])
      self.assertEqual(symbols['OBJECT'], ['(x)'])

  def test_unsupported_inputs_are_not_silently_ignored(self):
    with tempfile.TemporaryDirectory() as root:
      source = Path(root) / 'source.c'
      for contents in ('#define NEXT "a.h"\n#include_next NEXT\n',
                       '# /* comment */ em\\\nbed "a.bin"\n',
                       '??=include "trigraph.h"\n'):
        with self.subTest(contents=contents):
          source.write_text(contents)
          with self.assertRaises(basics.NotCoveredError):
            parse_file.ParseFile(cache_basics.MapToIndex()).Parse(str(source), {})

  def test_ParseFile(self):

    includepath_map = cache_basics.MapToIndex()
    canonical_path = cache_basics.CanonicalPath()
    parse_file_obj = parse_file.ParseFile(includepath_map)

    symbol_table = {}
    self.assertEqual(parse_file_obj.Parse(
      "test_data/more_macros.c", symbol_table),
      ([], [], ['TEMPLATE_VARNAME(foo)'], []))
    self.assertEqual(set(symbol_table.keys()),
                     set(['ILLFORMED', 'AS_STRING_INTERNAL',
                          'TEMPLATE_VARNAME', 'AS_STRING', 'MULTI']))
    [([arg], val)] = symbol_table['TEMPLATE_VARNAME']
    self.assertEqual(arg, '_filename_')
    self.assertEqual(val, 'AS_STRING(maps/_filename_.tpl.varnames.h)')

    [(args, defn)] = symbol_table['MULTI']
    self.assertEqual(args, ['a', 'b', 'c'])
    self.assertEqual(defn, "a + b + c")

    self.assertEqual(parse_file_obj.Parse(
      "test_data/computed_includes.c", symbol_table),
      ([],
       [],
       [ 'A' , 'm(abc)' ],
       []))
    self.assertEqual(symbol_table['A'], ['"p1.h"'])
    [val] = symbol_table['ILLFORMED']
    self.assertEqual(val, "(_filename_,(x)) "
                + "AS_STRING(maps/_filename_.tpl.varnames.h, "
                + "NOTHANDLED(_filename_))")

unittest.main()
