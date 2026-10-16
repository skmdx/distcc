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

__author__ = "Nils Klarlund"

# See also tests in include_server_test.py.

import os
import re
import basics
import parse_file
import cache_basics
import macro_eval
import include_analyzer_memoizing_node
import shutil
import tempfile
import unittest

NotCoveredError = basics.NotCoveredError

class MacroEvalTest(unittest.TestCase):

  def setUp(self):

    basics.opt_debug_pattern = 1

    self.tmp = tempfile.mkdtemp()
    caches = cache_basics.SetUpCaches(self.tmp)

    self.includepath_map = caches.includepath_map
    self.canonical_path = caches.canonical_path
    self.directory_map = caches.directory_map
    self.realpath_map = caches.realpath_map


  def tearDown(self):
    basics.opt_consider_unexpanded_macro_fns = True  # reset to the default state
    shutil.rmtree(self.tmp)


  def test__SubstituteSymbolInString(self):
    self.assertEqual(
      macro_eval._SubstituteSymbolInString("X", "f(y)", "X+(X, X##Y)"),
      "f(y)+(f(y), f(y)##Y)")
    self.assertEqual(
      macro_eval._SubstituteSymbolInString("a", "b", "c(a, aa)"),
      "c(b, aa)")

  def test_MassageAccordingToPoundSigns(self):
    self.assertEqual(macro_eval._MassageAccordingToPoundSigns('#aa ## bb'),
                     '"aabb"')
    self.assertEqual(macro_eval._MassageAccordingToPoundSigns('# a(.)'),
                     '"a(.)"')

  def test__ParseArgs(self):

    self.assertEqual(macro_eval._ParseArgs("(a,m(c, n(d)), c)", 0),
                     (["a", "m(c, n(d))", "c"], 17))

    self.assertEqual(macro_eval._ParseArgs("""(a","m(c, n(d)), c)""", 0),
                     (["""a","m(c, n(d))""", "c"], 19))


  def test__PrependToSet(self):
    self.assertEqual(
      macro_eval._PrependToSet("x", set(["y", "z"])),
      set(["xy", "xz"]))


  def test__BigUnion(self):
    self.assertEqual(macro_eval._BigUnion([set([]), set([1,2]), set([3])]),
		     set([1,2,3]))


  def test_EvalExprDirs(self):

    self.assertEqual(
      macro_eval.EvalExpression("A", { 'A': ['b'] }),
      set(['A', 'b']))

    self.assertEqual(
      macro_eval.EvalExpression("A", { 'A': ['B'], 'B': ['A'] }),
      set(['A', 'B']))

    self.assertEqual(
      macro_eval.EvalExpression("A", { 'A': ['B'], 'B': ['c'] }),
      set(['A', 'B', 'c']))

    self.assertEqual(
      macro_eval.EvalExpression("max(2, 4)",
		      { 'max': [ ( ['x', 'y'], "x < y? y: x") ] }),
      set(['max(2, 4)', '2 < 4? 4: 2']))

    self.assertEqual(
      macro_eval.EvalExpression("F(2, 4)",
		      { 'F': ['max'],
			'max': [ ( ['x', 'y'], "x < y? y: x") ] }),
      set(['max(2, 4)', 'F(2, 4)', '2 < 4? 4: 2']))

    self.assertEqual(
      macro_eval.EvalExpression("max(max(1,2), 3)",
		      { 'max': [ ( ['x', 'y'], "(x < y? y: x)") ] }),
       set(['((1 < 2? 2: 1) < 3? 3: (1 < 2? 2: 1))',
            'max(max(1,2), 3)',
            '(max(1,2) < 3? 3: max(1,2))',
            'max((1 < 2? 2: 1), 3)']))

    self.assertEqual(
      macro_eval.EvalExpression("A", { 'A': ['"a.c"'] }),
      set(['A', '"a.c"']))


    self.assertEqual(
      macro_eval.EvalExpression(
	"CAT(cfgdir,/)##hdrfile",
	{ "CAT" : [ ( ['a','b'], "a ## b" ) ],
	  "hdrfile" : [ "abc.h" ]}),
      set(["CAT(cfgdir,/)##hdrfile", "CAT(cfgdir,/)##abc.h",
           "cfgdir/abc.h", "cfgdir/hdrfile"]))

    self.assertEqual(
      macro_eval.EvalExpression(
	"STR(foo)",
	{ "STR" : [ ( ['text'], "STR_A((text))" ) ],
	  "STR_A" : [ ( ['arg'], "STR_I arg" ) ],
	  "STR_I" : [ ( ['text'], "#text" ) ] }),
      set(["STR(foo)", "STR_A((foo))", "STR_I (foo)", "\"foo\""]))

    # The ## operator only works in rhs of function-like macros. Check
    # that it doesn't work stand-alone.
    self.assertEqual(
      macro_eval.EvalExpression("A##A", { 'A': ['a.c'] }),
      set(['A##A', 'a.c##A', 'A##a.c', 'a.c##a.c']))

    self.assertEqual(
      macro_eval.EvalExpression("A(y)A(z)", { 'A': [(['x'], 'x##a.c')] }),
      set(['A(y)A(z)', 'A(y)za.c', 'ya.cza.c', 'ya.cA(z)']))

    self.assertEqual(
      macro_eval.EvalExpression("m(abc)", { 'm': [( ['a'], "<a##_post.c>" )] }),
      set(['m(abc)', '<abc_post.c>']))

    self.assertEqual(
      macro_eval.EvalExpression("myfile(hello)",
                                { 'myfile': [(['x'], "myquote(myplace/x)")],
                                  'myquote': [(['y'], """#y""")] }),
      set(['myfile(hello)',
           '"myplace/hello"',
           'myquote(myplace/hello)']))

  def test_EvalExprDirs_UnsafeUnexpandedFunctions(self):
    # Subset of the EvalExprDirs test to show how the parser behaves if
    # opt_consider_unexpanded_macro_fns is set to False
    basics.opt_consider_unexpanded_macro_fns = False

    self.assertEqual(
      macro_eval.EvalExpression("A", { 'A': ['b'] }),
      set(['A', 'b']))

    self.assertEqual(
      macro_eval.EvalExpression("A", { 'A': ['B'], 'B': ['A'] }),
      set(['A', 'B']))

    self.assertEqual(
      macro_eval.EvalExpression("A", { 'A': ['B'], 'B': ['c'] }),
      set(['A', 'B', 'c']))

    self.assertEqual(
      macro_eval.EvalExpression("max(2, 4)",
		      { 'max': [ ( ['x', 'y'], "x < y? y: x") ] }),
      set(['2 < 4? 4: 2']))

    self.assertEqual(
      macro_eval.EvalExpression("F(2, 4)",
		      { 'F': ['max'],
			'max': [ ( ['x', 'y'], "x < y? y: x") ] }),
      set(['F(2, 4)', '2 < 4? 4: 2']))

    self.assertEqual(
      macro_eval.EvalExpression("max(max(1,2), 3)",
		      { 'max': [ ( ['x', 'y'], "(x < y? y: x)") ] }),
       set(['((1 < 2? 2: 1) < 3? 3: (1 < 2? 2: 1))']))

    # Edge Case 1: function macros are still unexpanded if they are after
    # a string macro; the complexity is still reduced, but it's worth
    # calling out that this option doesn't prevent all lack of expansion
    self.assertEqual(
      macro_eval.EvalExpression("foo max(max(1,2), 3)",
              { 'foo': ["42"], 'max': [ ( ['x', 'y'], "(x < y? y: x)") ] }),
       set([
           'foo max(max(1,2), 3)',
           '42 ((1 < 2? 2: 1) < 3? 3: (1 < 2? 2: 1))',
           'foo ((1 < 2? 2: 1) < 3? 3: (1 < 2? 2: 1))'
       ]))

    # Edge Case 2: function macros being unexpanded could cause an issue
    # in a case where we have a token that we want unexpanded, because the
    # parser will believe it to be a function (even if its not due to #if)
    ec2_symtab = {"JOIN": [(['x', 'y'], "STR(EXP(x)EXP(y))")],
                                 "STR": [(['x'], "STR1(x)")],
                                 "STR1": [(['x'], "#x")],
                                 "EXP": [(['x'], "x")],}
    # With no definition of foo, it works as expected
    self.assertEqual(
      macro_eval.EvalExpression("JOIN(foo,.h)", ec2_symtab), set(['"foo.h"']))
    # If foo happens to be defined as a string, it also works
    ec2_symtab["foo"] = ["bar"]
    self.assertEqual(
      macro_eval.EvalExpression("JOIN(foo,.h)", ec2_symtab), set(['"foo.h"', '"bar.h"']) )
    # But if foo is a function, it fails to have anything, which could
    # cause problems if a spurious #define foo(x) elsewhere in the codebase
    # could trample a #include JOIN(foo,.h) (unlikely, but possible)
    ec2_symtab["foo"] = [(["x"], "blah")]
    self.assertEqual(
      macro_eval.EvalExpression("JOIN(foo,.h)", ec2_symtab), set() )


  def test_FromCPPInternals(self):
    # This little example works.
    #
    #   #define foo(x) bar x
    #
    #   foo(foo) (2) == bar foo (2)
    #
    # Let us check that.
    self.assertEqual(
      macro_eval.EvalExpression("foo(foo) (2)",
                                {'foo':[(['x'], "bar x")]}),
      set(['bar foo (2)', 'foo(foo) (2)']))

    # The next one does not work, because we are not inserting spaces.
    #
    # From :
    #   http://gcc.gnu.org/onlinedocs/cppinternals/Token-Spacing.html#Token-Spacing
    #
    #   #define PLUS +
    #   #define EMPTY
    #   #define f(x) =x=
    #
    #   +PLUS -EMPTY- PLUS+ f(=)
    #     ==> + + - - + + = = =
    #
    # We do not insert spaces as CPP does. But we generate a lot of
    # combinations!
    self.assertEqual(
      macro_eval.EvalExpression("+PLUS -EMPTY- PLUS+ f(=)",
                                { 'PLUS':['+'],
                                  'EMPTY':[""],
                                  'f':[(['x'], '=x=')] }),
      set(['++ -EMPTY- ++ ===',
           '++ -EMPTY- PLUS+ ===',
           '+PLUS -- ++ f(=)',
           '+PLUS -EMPTY- ++ ===',
           '++ -EMPTY- PLUS+ f(=)',
           '+PLUS -EMPTY- PLUS+ f(=)',
           '+PLUS -- ++ ===',
           '++ -EMPTY- ++ f(=)',
           '+PLUS -- PLUS+ ===',
           '+PLUS -- PLUS+ f(=)',
           '++ -- PLUS+ ===',
           '++ -- ++ ===',
           '+PLUS -EMPTY- PLUS+ ===',
           '++ -- PLUS+ f(=)',
           '+PLUS -EMPTY- ++ f(=)',
           '++ -- ++ f(=)']))


  def test_ResolveExpr(self):
    # Erect the edifice of caches.
    caches = cache_basics.SetUpCaches(self.tmp)
    parse_file_obj = parse_file.ParseFile(caches.includepath_map)

    symbol_table = {}
    # Set up symbol_table by parsing test_data/more_macros.c.
    self.assertEqual(parse_file_obj.Parse(
      "test_data/more_macros.c", symbol_table),
      ([], [], ['TEMPLATE_VARNAME(foo)'], []))

    # Check what we got in symbol_table.
    self.assertEqual(
      macro_eval.EvalExpression("TEMPLATE_VARNAME(foo)", symbol_table),
      set(['TEMPLATE_VARNAME(foo)',
           '"maps/foo.tpl.varnames.h"',
           'AS_STRING(maps/foo.tpl.varnames.h)',
           'AS_STRING_INTERNAL(maps/foo.tpl.varnames.h)']))

    self.assertEqual(
      macro_eval.EvalExpression("MULTI(12, 34, 56)", symbol_table),
      set(['MULTI(12, 34, 56)', '12 + 34 + 56']))

    # Verify that resolving this expression yields one actual file (which we
    # have placed in test_data/map).
    [((d, ip), rp)], symbols = macro_eval.ResolveExpr(
      caches.includepath_map.Index,
      caches.build_stat_cache.Resolve,
      'TEMPLATE_VARNAME(foo)',
      caches.directory_map.Index(os.getcwd()), # current dir
      caches.directory_map.Index(""), # file directory
      [caches.directory_map.Index("test_data")], # search directory
      [],
      symbol_table,
      include_analyzer_memoizing_node.SupportMaster(symbol_table).MatchingSymbols)
    self.assertEqual(caches.directory_map.string[d], "test_data/")
    self.assertEqual(caches.includepath_map.string[ip],
                     "maps/foo.tpl.varnames.h")
    self.assertEqual(symbols,
                     set(['TEMPLATE_VARNAME', 'maps',
                          'AS_STRING', 'AS_STRING_INTERNAL',
                          'tpl', 'varnames', 'h', 'foo']))

  def test_defined_operands_do_not_expand_or_track_macro_bodies(self):
    table = {'INT64_C': [(['x'], 'x ## L')],
             'UNRELATED_L': ['__has_include("unrelated.h")'],
             'CHECK': ['defined(INT64_C)']}
    for expr in ('defined(INT64_C)', 'defined INT64_C', 'CHECK'):
      with self.subTest(expr=expr):
        files, symbols = macro_eval.ResolveExpr(
            None, None, parse_file.ProbeExpression(expr), 0, 0, (), (), table,
            include_analyzer_memoizing_node.SupportMaster(table).MatchingSymbols)
        self.assertEqual(files, [])
        self.assertNotIn('UNRELATED_L', symbols)
        self.assertFalse(any(isinstance(symbol, re.Pattern) for symbol in symbols))

  def test_defined_does_not_hide_probes_or_change_header_literals(self):
    table = {'HAS': [(['h'], '__has_include(h)')],
             'CHECK': [(['x'], 'defined(x) && HAS("probe.h")')]}
    for expr in ('defined(HAS) && HAS("probe.h")', 'CHECK(HAS)'):
      values = macro_eval.EvalExpression(expr, table, preserve_functions=True)
      self.assertTrue(any('__has_include("probe.h")' in value for value in values))
      self.assertFalse(any('defined(' in value for value in values))
    for header in ('defined(INT64_C).h', 'path/defined(INT64_C).h'):
      expr = '__has_include("%s")' % header
      self.assertIn(expr, macro_eval.EvalExpression(expr, {}, preserve_functions=True))
    table['defined_alias'] = table['HAS']
    self.assertIn('__has_include("probe.h")', macro_eval.EvalExpression(
        'defined_alias("probe.h")', table, preserve_functions=True))

  def test_nested_module_pastes_reuse_expansions_within_condition(self):
    table = {'PASTE_NAME1': [(['a', 'b'], 'a##b')],
             'PASTE_NAME': [(['a', 'b'], 'PASTE_NAME1 (a,b)')],
             'IN_MODULE': ['PASTE_NAME (MODULE_, MODULE_NAME)', '(-1)'],
             'MODULE_NAME': ['libc'],
             'IS_IN': [(['lib'], '(IN_MODULE == MODULE_##lib)'), (['lib'], '0')],
             'MODULE_libc': ['18'], 'MODULE_testsuite': ['14'],
             'MODULE_testsuite_internal': ['15']}
    files, symbols = macro_eval.ResolveExpr(None, None, parse_file.ProbeExpression(
        'IS_IN (testsuite) || IS_IN (testsuite_internal)'), 0, 0, (), (), table,
        include_analyzer_memoizing_node.SupportMaster(table).MatchingSymbols)
    self.assertEqual(files, [])
    self.assertIn('MODULE_NAME', symbols)
    # Each request observes the current definitions; memoization cannot hide a
    # newly defined probe or share results between different disabled macros.
    table['MODULE_libc'].append('__has_include("new.h")')
    resolved = []
    macro_eval.ResolveExpr(lambda value: value,
        lambda *args: resolved.append(args[0]), parse_file.ProbeExpression('IS_IN (libc)'),
        0, 0, (), (), table,
        include_analyzer_memoizing_node.SupportMaster(table).MatchingSymbols)
    self.assertIn('new.h', resolved)

  def test_nested_build_flags_do_not_expand_partial_call_products(self):
    table = {'CAT_IMPL': [(['a', 'b'], 'a##b')],
             'CAT': [(['a', 'b'], 'CAT_IMPL(a, b)')],
             'FLAG': [(['flag'], '(CAT(FLAG_, flag)())')],
             'CONFIG': [(['flag'], '(CONFIG_##flag())')],
             'CONFIG_ATFORK': [([''], '(FLAG(APPLE) || FLAG(LINUX) || FLAG(OS))')],
             'CONFIG_MOVE': [([''], '1'), ([''], '0')],
             'FLAG_ENABLED': [([''], '(0)')]}
    for name in ('APPLE', 'LINUX', 'OS'):
      table['FLAG_' + name] = [([''], '(1)'), ([''], '(0)')]
    expr = parse_file.ProbeExpression('FLAG(ENABLED) && CONFIG(ATFORK) || CONFIG(MOVE)')
    master = include_analyzer_memoizing_node.SupportMaster(table)
    files, symbols = macro_eval.ResolveExpr(None, None, expr, 0, 0, (), (), table,
                                          master.MatchingSymbols)
    self.assertEqual(files, [])
    self.assertIn('FLAG_LINUX', symbols)
    table['FLAG_LINUX'].append(([''], '__has_include("found.h")'))
    resolved = []
    macro_eval.ResolveExpr(lambda value: value, lambda *args: resolved.append(args[0]),
                          expr, 0, 0, (), (), table, master.MatchingSymbols)
    self.assertIn('found.h', resolved)

  def test_probe_functions_keep_fallbacks_and_function_arguments(self):
    table = {'__has_include': [(['h'], '0')],
             'CALL': [(['f', 'h'], 'f(h)')],
             'CHECK': [(['h'], '__has_include(h)')],
             'ID': [(['x'], 'x')],
             'NOT_CALLED': [(['x'], '0')]}
    for expr in ('CALL(CHECK, "found.h")', 'ID(__has_include)("found.h")',
                 'NOT_CALLED || CHECK("found.h")'):
      with self.subTest(expr=expr):
        resolved = []
        macro_eval.ResolveExpr(lambda value: value,
            lambda *args: resolved.append(args[0]), parse_file.ProbeExpression(expr),
            0, 0, (), (), table,
            include_analyzer_memoizing_node.SupportMaster(table).MatchingSymbols)
        self.assertIn('found.h', resolved)


unittest.main()
