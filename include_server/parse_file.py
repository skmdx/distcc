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

"""A very fast directives-only parser for C and C++ source code.

We parse only the following directives:
  #include       (the standard C/C++ inclusion mechanism)
  #include_next  (a GNU C/C++ extension)
  #import        (an Objective-C feature, similar to #include)
  #define        (because #defines can affect the results of '#include MACRO')
"""

__author__ = 'Nils Klarlund'

import re
import time
import clang_scan
import basics
import cache_basics
import statistics


class ProbeExpression(str):
  """A conditional expression whose expanded existence probes are inputs."""

Debug = basics.Debug
DEBUG_TRACE = basics.DEBUG_TRACE
DEBUG_TRACE2 = basics.DEBUG_TRACE2
NotCoveredError = basics.NotCoveredError

NOT_COMMA_OR_PARENS = "([^(),])"

# For parsing macro expressions of the form:
#  symbol
#  symbol  (something, ..., something), where something is not ',', '(', or ')'
MACRO_EXPR = r"""
 (?P<symbol>\w+)                   # the symbol, named 'symbol'
  ( \s*
    [(] \s*                        # beginning parenthesis
    (?P<args>                      # a parenthesized expression (with no
                                   # containing expressions -- a limitation)
                                   # named 'args'
    %(NOT_COMMA_OR_PARENS)s*        # the first argument (if it exists)
     ([,]%(NOT_COMMA_OR_PARENS)s*)* # subsequent arguments
    )
  [)]                              # ending parenthesis
  )?""" % {'NOT_COMMA_OR_PARENS': NOT_COMMA_OR_PARENS}

MACRO_EXPR_RE = re.compile(MACRO_EXPR, re.VERBOSE)

# Function-like definitions require adjacency, unlike macro invocations.
MACRO_DEFINITION_RE = re.compile(r'(\w+(?:\([^()]*\))?)\s*(.*)', re.S)

#
INCLUDE_STRING_RE = re.compile(r"""
  ^
  \s*
  ( "\s*(?P<quote> (\w|[\\_/.,+-])*)\s*"            |
    <\s*(?P<angle> (\w|[\\_/.,+-])*)\s*>
  )
  \s*
  $
""", re.VERBOSE)

# Clang provides directive tokens; remove phase-two continuations inside a
# token too (for example a split macro name or header name).
BACKSLASH_RE = re.compile(r'\\[ \t\v\f]*(?:\r\n|\n|\r)')


def InsertMacroDefInTable(lhs, rhs, symbol_table, callback_function):
  """Insert the definition of a pair (lhs, rhs) into symbol table.

  Arguments:
    lhs: a string, of the form "symbol" or "symbol(param1, ..., paramN)"
    rhs: a string
    symbol_table: where the definition will be inserted
    callback_function: a function called with value "symbol"
  """
  m_expr = MACRO_EXPR_RE.match(lhs)
  if m_expr is None or m_expr.end(0) != len(lhs):
    raise NotCoveredError(
      "Unexpected macro definition with LHS: '%s'." % lhs)
  # Calculate the definition df, either
  # - a pair ([arg_1, .., arg_n], rhs) where arg_i is the
  #   i'th formal parameter (function-like macro definition), or
  # - just a symbol (object-like macro definition)
  if m_expr.group('args') != None:  # perhaps ''
    # A function-like macro definition.
    # Construct pair (list of formal parameters, rhs).
    args = [param.strip() for param in m_expr.group('args').split(',')]
    df = args, rhs
    # lhs is adjusted to be just the 'function' name
    lhs = m_expr.group('symbol')
  else: # m_expr.group('args')
    # An object-like macro definition
    assert m_expr.group('symbol') == lhs
    df = rhs
  if lhs not in symbol_table:
    symbol_table[lhs] = [df]
  else:
    if df in symbol_table[lhs]:
      return
    symbol_table[lhs].append(df)
  callback_function(lhs)


class ParseFile(object):
  """Parser class for syntax understood by CPP, the C and C++
  preprocessor. An instance of this class defines the Parse method."""

  def __init__(self, includepath_map):
    """Constructor. Make a parser.

    Arguments:
      includepath_map: string-to-index map for includepaths
    """
    assert isinstance(includepath_map, cache_basics.MapToIndex)
    self.includepath_map = includepath_map
    self.raw_strings = True
    self.define_callback = lambda x: None

  def SetDefineCallback(self, callback_function):
    """Set a callback function, which is invoked for '#define's.

     The function is called as callback_function(symbol), whenever a '#define'
     of symbol is parsed.  The callback allows an include processor to adjust
     its notion of which expressions are still current. If we (the include
     processor) already met

             #define A B

     and later meet

             #define B

     whether this is the first definition of B or not, then the possible
     meanings of A have changed.  We set up a callback to identify such
     situations."""

    self.define_callback = callback_function

  def Parse(self, filepath, symbol_table):
    """Parse filepath for preprocessor directives and update symbol table.

    Arguments:
      filepath: a string
      symbol_table: a dictionary, see module macro_expr

    Returns:
      (quote_includes, angle_includes, expr_includes, next_includes), where
      all are lists of filepath indices, except for expr_includes, which is a
      list of expressions.
    """
    Debug(DEBUG_TRACE, "ParseFile %s", filepath)

    assert isinstance(filepath, str)
    self.filepath = filepath
    parse_file_start_time = time.perf_counter()
    statistics.parse_file_counter += 1

    includepath_map_index = self.includepath_map.Index

    quote_includes, angle_includes, expr_includes, next_includes = [], [], [], []
    try:
      with open(filepath, 'rb') as source:
        directives = clang_scan.directives(source.read(), self.raw_strings)
      for directive, operand in directives:
        operand = BACKSLASH_RE.sub('', operand).strip()
        if directive in ('if', 'elif'):
          # Expand complete conditions: aliases and token pasting may introduce
          # existence operators whose names are absent from this source file.
          expr_includes.append(ProbeExpression(operand))
        elif directive == 'define':
          definition = MACRO_DEFINITION_RE.fullmatch(operand)
          if not definition:
            raise NotCoveredError('Cannot parse macro definition: ' + operand)
          lhs, rhs = definition.groups()
          InsertMacroDefInTable(lhs, rhs or None, symbol_table, self.define_callback)
        else:
          literal = re.fullmatch(r'"([^"\n]+)"|<([^>\n]+)>', operand)
          if literal:
            quote, angle = literal.groups()
            destination = (next_includes if directive == 'include_next' else
                           quote_includes if quote else angle_includes)
            destination.append(includepath_map_index(quote or angle))
          elif directive == 'include_next':
            raise NotCoveredError('Computed include_next requires local preprocessing')
          else:
            expr_includes.append(operand)
    except (OSError, ValueError) as error:
      raise NotCoveredError(str(error), filepath, send_email=False) from error

    statistics.parse_file_total_time += time.perf_counter() - parse_file_start_time
    return quote_includes, angle_includes, expr_includes, next_includes
