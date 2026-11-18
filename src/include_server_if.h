/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/* Author: Manos Renieris */

/* Fixed-width network order, in the same order as the leased paths.
 * An all-ones wire size identifies links and directory placeholders. */
struct dcc_snapshot_metadata { unsigned char digest[8], wire[8], plain[8]; };
int dcc_talk_to_include_server(char **argv, char ***files, enum dcc_compress compr,
                               int *snapshot, struct dcc_snapshot_metadata **metadata);
int dcc_get_original_fname(const char *fname, char **original_fname);
int dcc_approximate_includes(struct dcc_hostdef *host, char **argv);
