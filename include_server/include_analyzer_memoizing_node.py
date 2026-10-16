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

"""A graph-based algorithm for memoizing include closure calculations."""

__author__ = "Nils Klarlund"

# TODO(klarlund) For computed includes, some static analysis checks must be
# introduced to verify soundness of node reutilization in FindNode.

import os
import re

import basics
import macro_eval
import parse_file
import statistics
import include_analyzer

Debug = basics.Debug
DEBUG_TRACE = basics.DEBUG_TRACE
DEBUG_DATA = basics.DEBUG_DATA
NotCoveredError = basics.NotCoveredError

# RESOLUTION MODES

RESOLVED = 1  # filepath already resolved and denotes an existing file
QUOTE    = 2  # filepath to be resolved against quote directories
ANGLE    = 3  # filepath to be resolved against angle directories
NEXT     = 4  # filepath to be resolved against each and every quote
              # directory; this is how we handle #include_next
RESOLUTION_MODES = [ RESOLVED, QUOTE, ANGLE, NEXT ]
# Textual representation of RESOLUTION_MODES.
RESOLUTION_MODES_STR = [ None, 'RESOLVED', 'QUOTE', 'ANGLE', 'NEXT' ]


# MAIN COURSE

class SupportRecord(object):
  """Direct macro dependencies and reverse edges of the include graph.

  A macro change invalidates its users and their parents. Keeping graph edges
  avoids copying every descendant's macro set into each ancestor. Edges may
  remain after rebuilding a node: macro definitions only accumulate until the
  graph and symbol table are cleared together.
  """

  def __init__(self, support_master):
    self.support_master = support_master
    self.valid = True
    self.symbols = set()
    self.parents = set()

  def DependsOn(self, child):
    child.parents.add(self)
    if not child.valid:
      self.Invalidate()

  def Invalidate(self):
    pending = [self]
    while pending:
      record = pending.pop()
      if record.valid:
        record.valid = False
        pending.extend(record.parents)

  def UpdateSet(self, symbols):
    """Register only this node's new macro dependencies."""
    added = set(symbols) - self.symbols
    if added:
      self.support_master.SupportRecordDependencyAdd(added, self)
      self.symbols.update(added)


class SupportMaster(object):
  """Record the support records that depend on a given symbol.

  A map symbol_to_records is maintained. For each symbol s
  self.symbol_to_records[s] is the set of records that directly reference s.
  Parent records are reached through the reverse include edges."""

  def __init__(self, symbol_table):
    """Constructor.

    Instance variables:
      symbol_to_records: a mapping to record sets
    """
    self.symbol_to_records = {}
    self.symbol_table = symbol_table
    self.patterns = {}

  def MatchingSymbols(self, pattern):
    """Reuse paste-name matches until the existing definition callback updates them."""
    if pattern not in self.patterns:
      self.patterns[pattern] = {
          name for name in self.symbol_table.keys() | macro_eval.INCLUDE_PROBE_NAMES
          if pattern.fullmatch(name)}
    return self.patterns[pattern]

  def SupportRecordDependencyAdd(self, symbols, support_record):
    """Add dependency of support record on symbol."""
    for symbol in symbols:
      if isinstance(symbol, re.Pattern):
        self.MatchingSymbols(symbol)
      if symbol not in self.symbol_to_records:
        self.symbol_to_records[symbol] = set([])
      self.symbol_to_records[symbol].add(support_record)

  def InvalidateRecords(self, symbol):
    """Mark as invalid all support records whose set contains symbol."""
    # Token pasting can form a previously unknown alias. Invalidate only the
    # patterns that can produce this name, not every node using a paste.
    keys = [symbol]
    for pattern, names in self.patterns.items():
      if pattern.fullmatch(symbol):
        names.add(symbol)
        keys.append(pattern)
    for key in keys:
      for support_record in self.symbol_to_records.get(key, ()):
        support_record.Invalidate()


class IncludeAnalyzerMemoizingNode(include_analyzer.IncludeAnalyzer):
  """A memoizing algorithm for include analysis based on a graph construction.

  Instance variables:

    master_cache: a two-level node cache

  The key of the top-level cache is an include configuration of the form
     (currdir_idx, quote_dirs, angle_dirs, raw_strings)

  The value of the top-level cache is a node-cache (defined next).

  The key of the second-level (node) cache has the form
     (filepath_idx, resolution_mode, file_dir_idx)

  A node is the value of the second-level (node) cache. It has the form
     [filepath_real_idx, filepath_resolved_pair, [node_0, ...node_n-1],
     support_record]

  where each node_i, for 1 <= i < n, is a node representing a direct include
  dependency of node. The direct include dependencies are the edges of a
  directed graph called the summary graph.

  TODO(csilvers): document what the values of the node-cache mean.

  In this class, the top-level key is referred to as 'incl_config', the
  top-level value is referred to as 'node_cache_for_incl_config', the
  second-level key is referred to as 'key', and the second-level value is
  referred to as 'node'.

  There are many disjoint summary graphs, one for each include configuration.
  Each node of a summary graph is the image of a key, that is, there are values
  incl_config and key such that node == master_cache[incl_config][key].

  As stated the node cache works pre-resolution. But it may well be that, say,
  two occurrences of #include "foo.h" in files with different file directories
  (that is, the files containing the foo.h includes are in different
  directories) actually resolve to the same foo.h file. In that case, we should
  reuse the foo.h node -- with a catch: all though the file may be the same
  real file, their containing directories may be different. For example, the
  file may be in the real location /D/foo.h, but it may also be known as
  /E/foo.h, where E is a directory containing a symbolic link foo.h pointing to
  /D/foo.h. If file foo.h has a quoted include of bar.h, that is, contains the
  directive

    #include "bar.h"

  then bar.h is looked for in /D if the file directory is /D, but it is looked
  for in /E if the file directory is /E. That is the real file directory of
  /E/foo.h is *not* the directory component of the realpath of /E/foo.h.
  Rather, it is the realpath of the directory component of /E/foo.h, that is,
  the realpath of /E.

  Thus, if we memoize files according to their real location, then the file
  directory as understood above must also be taken into account.

  In particular, we also use as keys pairs of the form:

     (realpath index of resolved file, real path index of filedir).

  This realpath-oriented memoization is not a frivolous attempt at optimization.
  It is essential to avoiding infinite loops as in:

          D/mem.h
          D/../D/mem.h
          D/../D/../D/mem.h

  generated by an include of the form "#include ../D/mem.h" in file mem.h.

  One would think that obviosly these prefixes denote the same location. But
  they need not! For D of the first line could be a symbolic link to a real
  directory dir1_D. And, the second D could be another symbolic link in
  dir1_D/ to dir2_D, etc...

  So, when the include processor is lead astray by includes that resolve this
  way it is by no means obvious how to investigate the paths with symbolic links
  of the form

           (D/..)*

  This will diverge even if there is just one mem.h file with an include of
  ../D/mem.h started in the real directory D.  [Remember that the include
  processor does not heed include guards.]

  For termination, we rely on the fact that eventually the pair (realpath of
  file, real path of file directory) will be seen again (because there are
  finitely many files and directories). In practice, with this technique, the
  recursion is stopped at the second attempt to include mem.h.
  """

  # The magic '3' selects the fourth component of a node, see the class
  # documentation.
  SUPPORT_RECORD = 3

  def _InitializeAllCachesMemoizing(self):
    self.master_cache = {}
    # Keep track of the support of each included file. The support of the file
    # is the union of the support of expressions in computed includes in the
    # file or in recursively included file.
    self.support_master = SupportMaster(self.symbol_table)
    # Enable the mechanism that invalidates all support records that contain a
    # symbol that is being defined or redefined.
    self.parse_file.SetDefineCallback(self.support_master.InvalidateRecords)

  def __init__(self, client_root_keeper, stat_reset_triggers={}):
    """Constructor."""
    include_analyzer.IncludeAnalyzer.__init__(self,
                                              client_root_keeper,
                                              stat_reset_triggers)
    self._InitializeAllCachesMemoizing()

  def ClearStatCaches(self):
    """Reset stat caches and the node cache, which depends on stat caches."""
    # First, clear caches as the IncludeAnalyzer class prescribes it.
    include_analyzer.IncludeAnalyzer.ClearStatCaches(self)
    # Then, clear own caches.
    self._InitializeAllCachesMemoizing()

  def _PrintableFilePath(self, fp):
    return (isinstance(fp, int) and self.includepath_map.String(fp)
            or isinstance(fp, tuple) and
            (self.directory_map.string[fp[0]],
             self.includepath_map.string[fp[1]]))


  def RunAlgorithm(self, filepath_resolved_pair, filepath_real_idx):
    """See RunAlgorithm of class IncludeAnalyzer in include_analyzer."""
    incl_config = (self.currdir_idx, self.quote_dirs, self.angle_dirs, self.parse_file.raw_strings)
    try:
      nodes_for_incl_config = self.master_cache[incl_config]
    except KeyError:
      nodes_for_incl_config = self.master_cache[incl_config] = {}

    # Process symbols defined on command line.
    for d_opt in self.d_opts:
      if len(d_opt) == 1:
        lhs, rhs = d_opt[0], "1"
      elif len(d_opt) == 2:
        [lhs, rhs] = d_opt
        parse_file.InsertMacroDefInTable(lhs, rhs, self.symbol_table,
                                         self.support_master.InvalidateRecords)
      else:
        # Assume this is a syntax error of some sort.
        pass

    # Construct or find the node for filepath_resolved.
    node = self.FindNode(nodes_for_incl_config,
                         filepath_resolved_pair,
                         RESOLVED,
                         None,
                         filepath_real_idx)
    # Find the nodes reachable from node and represent as an include closure.
    include_closure = {}
    self._CalculateIncludeClosure(node, include_closure)
    return include_closure

  def FindNode(self,
               nodes_for_incl_config,
               fp,
               resolution_mode,
               file_dir_idx=None,
               fp_real_idx=None):
    """Find a previously constructed node or create a new node.

    Arguments:
      nodes_for_incl_config: a dictionary (see class documentation).
      fp: a filepath index or, if resolution_mode == RESOLVED, a filepath pair
      resolution_mode: an integer in RESOLUTION_MODES
      file_dir_idx: consider the file F that has the line '#include "fp"'
        which is causing us to call FindNode on fp.  file_dir_idx is the
        index of dirname(F).  (This argument affects the semantics of
        resolution for resolution_mode == QUOTE.)
      fp_real_idx: the realpath index of resolved filepath
        (Useful for resolution_mode == RESOLVED only.)
    Returns:
      a node or None
    Raises:
      NotCoveredError

    This is function is long, too long. But function calls are
    expensive in Python. TODO(klarlund): refactor.
    """
    # Convenient abbreviations for cache access.
    dir_map = self.directory_map
    includepath_map =  self.includepath_map
    resolve = self.build_stat_cache.Resolve
    # Now a little dynamic type verification.  Remember that "A implies B" is
    # exactly the same as "not A or B", at least in some primitive formal
    # systems.
    assert isinstance(nodes_for_incl_config, dict)
    assert (not self.IsFilepathPair(fp)
            or resolution_mode == RESOLVED)
    assert (not fp
            or (self.IsFilepathPair(fp)
                or (resolution_mode != RESOLVED
                    and self.IsIncludepathIndex(fp))))
    assert resolution_mode in RESOLUTION_MODES
    assert not resolution_mode == QUOTE or file_dir_idx
    assert not file_dir_idx or resolution_mode == QUOTE
    assert not fp_real_idx or resolution_mode == RESOLVED

    if __debug__:
      Debug(DEBUG_TRACE,
            "FindNode: fp: %s, mode: %s\n  file_dir: %s,\n  fp_real: %s" %
            (self._PrintableFilePath(fp),
             RESOLUTION_MODES_STR[resolution_mode],
             not file_dir_idx and " " or dir_map.string[file_dir_idx],
             not fp_real_idx and " "
             or self.realpath_map.string[fp_real_idx]))
      statistics.find_node_counter += 1

    if fp == None: return

    # We must remember the resolution_mode when we key our function call. And
    # for resolution_mode == QUOTE it is important to also remember the
    # file_dir_idx, because the filepath is resolved against file_dir.
    key = (fp, resolution_mode, file_dir_idx)
    if key in nodes_for_incl_config:
      # Is the support record valid?
      if nodes_for_incl_config[key][self.SUPPORT_RECORD].valid:
        statistics.master_hit_counter += 1
        return nodes_for_incl_config[key]
      else:
        # Invalid support record. The meaning of some computed includes may have
        # changed.
        node = nodes_for_incl_config[key]
        currdir_idx = self.currdir_idx
        quote_dirs = self.quote_dirs
        angle_dirs = self.angle_dirs
        # Retrieve filepath information. That is still OK. Disregard children,
        # because they will be rebuilt. Reuse support_record. Don't switch
        # support_record.valid to True before running through all the caching
        # code below -- we don't want to reuse an earlier result.
        [fp_real_idx, fp_resolved_pair, _, support_record] = node
        Debug(DEBUG_TRACE,
              "Invalid record for translation unit: %s, file: %s",
              self.translation_unit, self._PrintableFilePath(fp))

    else:
      # This is a new file -- for this include configuration at least.
      support_record = SupportRecord(self.support_master)
      currdir_idx = self.currdir_idx
      quote_dirs = self.quote_dirs
      angle_dirs = self.angle_dirs

      if resolution_mode == QUOTE:
        (fp_resolved_pair, fp_real_idx) = (
          resolve(fp, currdir_idx, file_dir_idx, quote_dirs))
      elif resolution_mode == ANGLE:
         (fp_resolved_pair, fp_real_idx) = (
           resolve(fp, currdir_idx, None, angle_dirs))
      elif resolution_mode == NEXT:
        # The node we return is just a dummy whose children are all the
        # possible resolvants.
        fp_resolved_pair = None
        fp_real_idx = None
      else:
        assert resolution_mode == RESOLVED
        assert fp_real_idx  # this is the realpath corresponding to fp
        assert self.IsFilepathPair(fp)
        fp_resolved_pair = fp  # we are given the resolvant

    if fp_resolved_pair:
       # The resolution succeeded. Before recursing, make sure to
       # mirror the path.  Guard the call of MirrorPath with a cache
       # check; many files will have been visited before (for other
       # include directories).
       (d_, fp_) = fp_resolved_pair
       if (fp_resolved_pair, currdir_idx) not in self.mirrored:
         self.mirrored.add((fp_resolved_pair, currdir_idx))
         self.mirror_path.DoPath(
            os.path.join(dir_map.string[currdir_idx],
                         dir_map.string[d_],
                         includepath_map.string[fp_]),
            currdir_idx,
            self.client_root_keeper.client_root)

    # We have fp_resolved_pair if and only if we have fp_real_idx
    assert not fp_resolved_pair or fp_real_idx
    assert not fp_real_idx or fp_resolved_pair
    # Now construct the node, even before we know the children; this
    # early construction/late filling-in of children allows us to stop
    # a recursion early, when key is in nodes_for_incl_config. A cyclic
    # structure may arise in this way.
    children = []
    node = (fp_real_idx, fp_resolved_pair, children,
            support_record)
    nodes_for_incl_config[key] =  node

    if not fp_resolved_pair:
      support_record.valid = True
      if resolution_mode == NEXT:
        # Create children of this dummy node. Try against all
        # directories in quote_dirs; that list includes the
        # angle_dirs.  Recurse for each success.
        for d in quote_dirs:
          (fp_resolved_pair_, fp_real_idx_) = (
            resolve(fp, currdir_idx, None, (d,)))
          if fp_resolved_pair_ != None:
            node_ = self.FindNode(nodes_for_incl_config,
                                  fp_resolved_pair_,
                                  RESOLVED,
                                  None, # file_dir_idx
                                  fp_real_idx_)
            children.append(node_)
            support_record.DependsOn(node_[self.SUPPORT_RECORD])
        return node
      else:
        # For non-NEXT resolution modes
        return node

    # Now, we've got the resolution: (search directory, include path).
    assert (fp and fp_real_idx and fp_resolved_pair)
    (searchdir_idx, includepath_idx) = fp_resolved_pair

    # We need the realpath index of the current file directory. That's because
    # we are going to ask whether we have really visited this file, despite the
    # failure above to recognize it using a possibly relative name.  Here,
    # 'really' means 'with respect to realpath'.  Please see the class
    # documentation for why we need to calculate the realpath index of file
    # directory as part of the investigation of whether we have 'really'
    # encountered the file before.
    try:
      (fp_dirname_idx, fp_dirname_real_idx)  = (
          self.dirname_cache.cache[(currdir_idx,
                                    searchdir_idx,
                                    includepath_idx)])
    except KeyError:
      (fp_dirname_idx, fp_dirname_real_idx) = (
          self.dirname_cache.Lookup(currdir_idx,
                                    searchdir_idx,
                                    includepath_idx))

    if resolution_mode != RESOLVED:
      # See whether we know about filepath post-resolution.
      cached = nodes_for_incl_config.get((fp_real_idx, fp_dirname_real_idx))
      if cached and cached[self.SUPPORT_RECORD].valid:
        statistics.master_hit_counter += 1
        # Redo former decision about node: we use the one that is
        # already there.
        node = cached
        nodes_for_incl_config[key] = node
        return node
      # Couldn't find node under real name. We'll remember the node, but have to
      # continue processing it.
      nodes_for_incl_config[(fp_real_idx, fp_dirname_real_idx)] = node

    # All chances of hitting the node cache are now exhausted!
    statistics.master_miss_counter += 1
    # If we're revisiting because the support record was invalid, then it is
    # time to set it.
    support_record.valid = True

    # Try to get the cached result of parsing file.
    try:
      (quote_includes, angle_includes, expr_includes, next_includes) = (
        self.file_cache[fp_real_idx, self.parse_file.raw_strings])
    except KeyError:
      # Parse the file.
      self.file_cache[fp_real_idx, self.parse_file.raw_strings] = self.parse_file.Parse(
         self.realpath_map.string[fp_real_idx],
         self.symbol_table)
      (quote_includes, angle_includes, expr_includes, next_includes) = (
        self.file_cache[fp_real_idx, self.parse_file.raw_strings])


    # Do the includes of the form #include "foo.h".
    for quote_filepath in quote_includes:
      node_ = self.FindNode(nodes_for_incl_config, quote_filepath, QUOTE,
                            fp_dirname_idx)
      if node_:
        children.append(node_)
        support_record.DependsOn(node_[self.SUPPORT_RECORD])
    # Do the includes of the form #include <foo.h>.
    for angle_filepath in angle_includes:
      node_ = self.FindNode(nodes_for_incl_config, angle_filepath, ANGLE)
      if node_:
        children.append(node_)
        support_record.DependsOn(node_[self.SUPPORT_RECORD])
    if __debug__:
      if expr_includes: # Computed includes are interesting
        Debug(DEBUG_DATA, "FindNode, expr_includes: file: %s: '%s'",
              (isinstance(fp, int) and includepath_map.String(fp)
               or (isinstance(fp, tuple) and
                   (dir_map.string[fp[0]],
                    includepath_map.string[fp[1]]))),
              expr_includes)

    # Do the includes of the form #include expr, the computed includes.
    for expr in expr_includes:
      # Use multi-valued semantics to gather set of possible filepaths that the
      # C/C++ string expr may evaluate to under preprocessing semantics, given
      # the current symbol table. The symbols are all those of possible
      # expansions.
      (files, symbols) = (
        macro_eval.ResolveExpr(includepath_map.Index,
                               resolve,
                               expr,
                               self.currdir_idx, fp_dirname_idx,
                               self.quote_dirs, self.angle_dirs,
                               self.symbol_table,
                               self.support_master.MatchingSymbols))
      for (fp_resolved_pair_, fp_real_idx_) in files:
        node_ = self.FindNode(nodes_for_incl_config,
                              fp_resolved_pair_,
                              RESOLVED, None, fp_real_idx_)
        if node_:
          children.append(node_)
          support_record.DependsOn(node_[self.SUPPORT_RECORD])
      #  Now the resolution of includes of the file of the present node depends
      #  on symbols.
      support_record.UpdateSet(symbols)

    # Do includes of the form #include_next "foo.h" or # #include_next <foo.h>.
    for include_next_filepath in next_includes:
      node_ = self.FindNode(nodes_for_incl_config, include_next_filepath, NEXT)
      if node_:
        children.append(node_)
        support_record.DependsOn(node_[self.SUPPORT_RECORD])
    return node

  def _CalculateIncludeClosure(self, node, include_closure):
    """Explore the subgraph reachable from node and gather real paths.

    Arguments:
      node: the node of the translation unit, the initial source file
            (see class documentation for a description of this tuple).
      include_closure: a map (see IncludeAnalyzer.RunAlgorithm
                       documentation for a description of this map).
    Modifies:
      include_closure.  We modify in place to avoid copying this big struct.
    """
    assert not include_closure  # should start out empty
    visited = set([])
    dir_map_string = self.directory_map.string
    if not node: return
    stack = ([node])          # TODO(csilvers): consider using a deque
    statistics.len_calculated_closure_nonsys = 0
    statistics.len_calculated_closure = 0
    while stack:
      node = stack.pop(-1)
      id_ = id(node)
      if id_ in visited:
        continue
      visited.add(id_)
      # We optimized away:
      #
      #   (fp_real_idx, fp_resolved_pair, children) = node
      #
      # so that the common case (that fp_real_idx is known to compiler)
      # is dispatched away with quickly:
      if node[0]:      # fp_real_idx
        statistics.len_calculated_closure += 1
        # System headers belong to the client too. Send their snapshots using
        # the same immutable input cache as application headers.
        if node[0] not in include_closure:
          include_closure[node[0]] = []
        searchdir_idx = node[1][0]
        if (searchdir_idx and dir_map_string[searchdir_idx] and
            dir_map_string[searchdir_idx][0] == '/'):
          include_closure[node[0]].append(node[1])
      # Even when a node does not describe a filepath, it may still
      # have children that do if it is a dummy node.
      # TODO(csilvers): see if it speeds things up to append node[2],
      #                 so stack is a list of lists.
      stack.extend(node[2])
    statistics.len_calculated_closure_nonsys = sum(
        not self.systemdir_prefix_cache.StartsWithSystemdir(index, self.realpath_map)
        for index in include_closure)
