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
#

__author__ = "Nils Klarlund"

import os
import os.path
import tempfile
import unittest
import socket
import array
import errno
from unittest import mock

import basics

class BasicsTest(unittest.TestCase):

  def setUp(self):
    environment = mock.patch.dict(os.environ)
    environment.start()
    self.addCleanup(environment.stop)
    basics.opt_debug_pattern = 1

  def tearDown(self):
    pass

  def test_ClientRootKeeper(self):
    os.environ['DISTCC_CLIENT_TMP'] = 'to/be'
    self.assertRaises(SystemExit, basics.ClientRootKeeper)
    for path, padding in (('/to', 1), ('/to/be', 0), ('/to/be/or', 0)):
      os.environ['DISTCC_CLIENT_TMP'] = path
      self.assertEqual(basics.ClientRootKeeper().number_missing_levels, padding)
    with tempfile.TemporaryDirectory() as directory:
      os.environ['DISTCC_CLIENT_TMP'] = directory
      keeper = basics.ClientRootKeeper()
      keeper.ClientRootMakedir(1)
      self.assertTrue(os.path.isdir(keeper.client_root))
      self.assertTrue(keeper.client_root.startswith(directory + '/'))
      keeper.CleanOutClientRoots()
      self.assertFalse(keeper.Glob(str(os.getpid())))

  def test_ClientRootKeeper_Deletions(self):
    """Test whether directories emerge and go away appropriately."""

    # Test with a one-level value of DISTCC_CLIENT_TMP.
    os.environ['DISTCC_CLIENT_TMP'] = tempfile.gettempdir()
    client_root_keeper = basics.ClientRootKeeper()
    client_root_keeper.ClientRootMakedir(117)
    self.assertTrue(os.path.isdir(client_root_keeper._client_root_before_padding))
    self.assertTrue(os.path.isdir(client_root_keeper.client_root))
    self.assertTrue(client_root_keeper.client_root.startswith(tempfile.gettempdir()))
    client_root_keeper.ClientRootMakedir(118)
    client_root_keeper.CleanOutClientRoots()
    # Directories must be gone now!
    self.assertTrue(not os.path.isdir(
        client_root_keeper._client_root_before_padding))
    # Test with a two-level value of DISTCC_CLIENT_TMP.
    try:
      os.environ['DISTCC_CLIENT_TMP'] = tempfile.mkdtemp('basics_test',
                                                         dir=tempfile.gettempdir())
      client_root_keeper = basics.ClientRootKeeper()
      client_root_keeper.ClientRootMakedir(117)
      self.assertTrue(os.path.isdir(
          client_root_keeper._client_root_before_padding))
      self.assertTrue(os.path.isdir(client_root_keeper.client_root))
      client_root_keeper.ClientRootMakedir(118)
      client_root_keeper.CleanOutClientRoots()
      self.assertTrue(os.path.isdir,
                   client_root_keeper._client_root_before_padding)
    finally:
      os.rmdir(os.environ['DISTCC_CLIENT_TMP'])

  def test_retired_generations_follow_client_descriptors(self):
    with tempfile.TemporaryDirectory() as directory:
      os.environ['DISTCC_CLIENT_TMP'] = directory
      keeper = basics.ClientRootKeeper()
      collector = basics.ClientRootKeeper()
      keeper.ClientRootMakedir(1)
      old = keeper.client_root
      with open(old + '/snapshot', 'w') as output:
        output.write('immutable input')
      sender, receiver = socket.socketpair()
      try:
        sender.sendmsg([b'\0'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                array.array('i', [keeper.lease]))])
        _, rights, _, _ = receiver.recvmsg(1, socket.CMSG_SPACE(4))
        lease = array.array('i', rights[0][2])[0]
      finally:
        sender.close()
        receiver.close()
      try:
        for generation in range(2, 22):
          keeper.ClientRootMakedir(generation)
          collector.CollectClientRoots()
          self.assertEqual(len(keeper.Glob(str(os.getpid()))), 2)
          with open(old + '/snapshot') as source:
            self.assertEqual(source.read(), 'immutable input')
      finally:
        os.close(lease)
      collector.CollectClientRoots()
      self.assertFalse(os.path.exists(old))
      self.assertTrue(os.path.isdir(keeper.client_root))
      keeper.CleanOutClientRoots()
      self.assertEqual(os.listdir(directory), [])

  def test_storage_failure_releases_retired_generation(self):
    with tempfile.TemporaryDirectory() as directory:
      os.environ['DISTCC_CLIENT_TMP'] = directory
      keeper = basics.ClientRootKeeper()
      keeper.ClientRootMakedir(1)
      old = keeper.client_root
      with mock.patch.object(tempfile, 'mkdtemp', side_effect=OSError(errno.ENOSPC, 'full')):
        with self.assertRaises(SystemExit):
          keeper.ClientRootMakedir(2)
      self.assertFalse(os.path.exists(old))
      self.assertIsNone(keeper.lease)
      keeper.ClientRootMakedir(3)
      keeper.CleanOutClientRoots()
      self.assertEqual(os.listdir(directory), [])

unittest.main()
