#!/usr/bin/env python3

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

"""Conservative approximation of include dependencies for C/C++."""

__author__ = "Nils Klarlund"

# TODO (klarlund) Implement abort mechanism: regularly check whether
# ppid is 0; if so, then abort.

# Python imports
import gc
import fcntl
import getopt
import glob
import os
import re
import resource
import shutil
import signal
import socketserver
import sys
import struct
import tempfile
import time
import traceback

# Include server imports
import basics
import distcc_pump_c_extensions
import scan_statistics as statistics

# The default size passed to listen by a streaming socket server of
# socketserver is only 5. Make it 4096, which is the default net.core.somaxconn
# setting as of Linux 5.4. This enables requests to the include server to be
# buffered better before connections begin being refused. See also listen(2).
REQUEST_QUEUE_SIZE = 4096

Debug = basics.Debug
DEBUG_TRACE = basics.DEBUG_TRACE
DEBUG_WARNING = basics.DEBUG_WARNING
# Exceptions.
SignalSIGTERM = basics.SignalSIGTERM
NotCoveredError = basics.NotCoveredError


# USAGE

def Usage():
  print("""Usage:

include_server --port INCLUDE_SERVER_PORT [OPTIONS]

where INCLUDE_SERVER_PORT is a socket name. Fork the include server
for incremental include analysis. The include server answers queries
from the distcc client about which files to include in a C/C++
compilation. This command itself terminates as soon as the include
server has been spawned.

OPTIONS:

 -dPAT, --debug_pattern=PAT  Bit vector for turning on warnings and debugging
                               1 = warnings
                               2 = trace some functions
                             other powers of two: see basics.py.

 -e, --email                 Send email to discc-pump developers when include
                             server gets in trouble.

 --email_bound NUMBER        Maximal number of emails to send (in addition to
                             a final email). Default: 3.

 --no-email                  Do not send email.

 --path_observation_re=RE    Issue warning message whenever a filename is
                             resolved to a realpath that is matched by RE,
                             which is a regular expression in Python syntax.
                             This is useful for finding out where files included
                             actually come from. Use RE="" to find them all.
                             Note: warnings must be enabled with at least -d1.

 --pid_file FILEPATH         The pid of the include server is written to file
                             FILEPATH.

 --workers NUMBER           Persistent analyzers. Pump derives this from the
                             CPU affinity and cgroup CPU quotas.

 -s, --statistics            Print information to stdout about include analysis.

 --stat_reset_triggers=LIST  Flush stat caches when the timestamp of any
                             filepath in LIST changes or the filepath comes in
                             or out of existence.  LIST is a colon separated
                             string of filepaths, possibly containing simple
                             globs (as allowed by Python's glob module). Print
                             a warning whenever such a change happens (if
                             warnings are enabled). This option allows limited
                             exceptions to distcc_pump's normal assumption that
                             source files are not modified during the build.

 -t, --time                  Print elapsed, user, and system time to stderr.

 --no_force_dirs             Do not force the creation of all directories used
                             in an include path.  May improve performance for
                             some cases, but will break builds which use
                             include structures like "<foo/../file.h>" without
                             including other files in foo/.

 -v, --verify                Verify that files in CPP closure are contained in
                             closure calculated by include processor.

 -w, --write_include_closure Write a .d_approx file which lists all the
                             included files calculated by the include server;
                             with -v, additionally write the included files
                             as calculated by CPP to a .d_exact file.

""")


# UTILITIES

def _PrintStackTrace(fd):
  """Print stacktrace to file object."""
  print('------- Include server stack trace -----------', file=fd)
  # Limit is 1000 entries.
  traceback.print_exc(1000, fd)
  print('----------------------------------------------', file=fd)


class _EmailSender(object):
  """For sending emails. We limit their number to avoid email storms."""

  def __init__(self):
    self.number_sent = 0

  def TryToSend(self, fd, force=False, never=False):
    """Send the contents of file to predefined blame address.
    Arguments:
      fd: open file descriptor, will remain open
      force: send even if bound has been reached
    """
    if not basics.opt_send_email: return
    if self.number_sent >= basics.opt_email_bound and not force: return
    if never: return
    self.number_sent += 1
    # For efficiency, we postpone reading needed libraries for emailing until
    # now.
    import smtplib
    import getpass
    import socket
    try:
      user_addr = "%s@%s" %  (getpass.getuser(), socket.gethostname())
      fd.seek(0)
      msg = "Subject: %s\nTo: %s\nFrom: %s\n\n%s\n%s" % (
        basics.EMAIL_SUBJECT,
        basics.DCC_EMAILLOG_WHOM_TO_BLAME,
        user_addr,
        "Automated email number %d in include server session.\n" %
        self.number_sent,
        fd.read())
      s = smtplib.SMTP()
      s.connect()
      s.sendmail(user_addr, [basics.DCC_EMAILLOG_WHOM_TO_BLAME], msg)
      Debug(DEBUG_WARNING, "Include server sent email to %s",
            basics.DCC_EMAILLOG_WHOM_TO_BLAME)
      s.close()
    except:
      Debug(DEBUG_WARNING, basics.CANT_SEND_MESSAGE)
      traceback.print_exc()

  def MaybeSendEmail(self, fd, force=False, never=False):
    """Print warning and maybe send email; the contents is from file object.

    Arguments:
      fd: a file object that will be closed.
      force: send the mail even if number of emails sent exceed
        basics.opt_email_bound
    """
    fd.seek(0, 0)
    Debug(DEBUG_WARNING, "%s", fd.read())
    self.TryToSend(fd, force, never)
    fd.close()


NEWLINE_RE = re.compile(r"\n", re.MULTILINE)
BACKSLASH_NEWLINE_RE = re.compile(r"\\\n", re.MULTILINE)


def ExactDependencies(cmd, realpath_map, translation_unit):
  """The dependencies as calculated by CPP, the C Preprocessor.
  Arguments:
    cmd:  the compilation command, a string
    realpath_map: map from filesystem paths (no symlink) to idx
    translation_unit: string
  Returns:
    the set of realpath indices of the include dependencies.
  Raises:
    NotCoveredError
  """

  # Safely get a couple of temporary files.
  (fd_o, name_o) = tempfile.mkstemp("distcc-pump")
  (fd_d, name_d) = tempfile.mkstemp("distcc-pump")

  def _delete_temp_files():
    os.close(fd_d)
    os.close(fd_o)
    os.unlink(name_o)
    os.unlink(name_d)

  # Remove -o option and call with -E, -M, and -MF flags.
  preprocessing_command = (
    (re.sub(r"\s-o[ ]?(\w|[./+-])+", " ", cmd) # delete -o option
     + " -o %(name_o)s"                        # add it back, but to temp file,
     + " -E"                                   # macro processing only
     + " -M -MF %(name_d)s") %                  # output .d file
    {'name_o':name_o, 'name_d':name_d})

  ret = os.system(preprocessing_command)
  if ret:
    _delete_temp_files()
    raise NotCoveredError("Could not execute '%s'" %
                          preprocessing_command,
                          translation_unit)
  # Using the primitive fd_d file descriptor for reading is cumbersome, so open
  # normally as well.
  fd_d_ = open(name_d, "r", encoding="latin-1")
  # Massage the contents of fd_d_
  dotd = re.sub("^.*:", # remove Makefile target
                "",
                NEWLINE_RE.sub(
                  "", # remove newlines
                  BACKSLASH_NEWLINE_RE.sub("", # remove backslashes
                                   fd_d_.read())))
  fd_d_.close()
  _delete_temp_files()
  # The sets of dependencies is a set the of realpath indices of the
  # absolute filenames corresponding to files in the dotd file.
  deps = set([ rp_idx
               for filepath in dotd.split()
               for rp_idx in [ realpath_map.Index(os.path.join(os.getcwd(),
                                                               filepath)) ]
              ])
  statistics.len_exact_closure = len(deps)
  return deps


def WriteDependencies(deps, result_file, realpath_map):
  """Write the list of deps to result_file.
  Arguments:
    deps: a list of realpath indices
    result_file: a filepath
    realpath_map: map from filesystem paths (no symlink) to idx
  """
  try:
    fd = open(result_file, "w")
    fd.write("\n".join([realpath_map.string[d] for d in deps]))
    fd.write("\n")
    fd.close()
  except (IOError, OSError) as  why:
    raise NotCoveredError("Could not write to '%s': %s" % (result_file, why))


def VerifyExactDependencies(include_closure,
                            exact_no_system_header_dependency_set,
                            realpath_map,
                            translation_unit):
  """Compare computed and real include closures, ignoring system
  header files (such as those in /usr/include).
  Arguments:
    include_closure: a dictionary whose keys are realpath indices
    exact_no_system_header_dependency_set: set of realpath indices
    realpath_map: map from filesystem paths (no symlink) to idx
    translation_unit: string
  Raises:
    NotCoveredError
"""
  diff = exact_no_system_header_dependency_set - set(include_closure)
  statistics.len_surplus_nonsys = (
    len(set(include_closure) - exact_no_system_header_dependency_set))

  if diff != set([]):
    # Pick one bad dependency.
    bad_dep = diff.pop()
    raise NotCoveredError(
      ("Calculated include closure does not contain: '%s'.\n"
       + "There %s %d such missing %s.")
      % (realpath_map.string[bad_dep],
         len(diff) == 0 and "is" or "are",
         len(diff) + 1,
         len(diff) == 0 and "dependency" or "dependencies"),
      translation_unit)


# A SOCKET SERVER

class Queuingsocketserver(socketserver.UnixStreamServer):
  """A socket server whose request queue have size REQUEST_QUEUE_SIZE."""
  request_queue_size = REQUEST_QUEUE_SIZE

  def handle_error(self, request, client_address):
    """Re-raise current exception; overrides socketserver.handle_error.
    """
    raise


# HANDLER FOR SOCKETSERVER

def RecordMemory(path):
  """Publish a monotonic pool-wide resident plus native frontend high-water mark."""
  peak = 1024 * (resource.getrusage(resource.RUSAGE_SELF).ru_maxrss +
                 resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
  with open(path, 'r+b', buffering=0) as output:
    fcntl.lockf(output, fcntl.LOCK_EX)
    previous, = struct.unpack('=Q', output.read(8))
    if peak > previous:
      output.seek(0)
      output.write(struct.pack('=Q', peak))


def DistccIncludeHandlerGenerator(include_analyzer, memory_path=None):
  """Wrap a socketserver based on the include_analyzer object inside a new
  type that is a class named IncludeHandler."""

  # TODO(klarlund): Can we do this without dynamic type generation?

  class IncludeHandler(socketserver.StreamRequestHandler):
    """Define a handle() method that invokes the include closure algorithm ."""

    def handle(self):
      """Using distcc protocol, read command and return include closure.

      Do the following:
       - Read from the socket, using the RPC protocol of distcc:
          - the current directory, and
          - the compilation command, already broken down into an argv vector.
       - Parse the command to find options like -I, -iquote,...
       - Invoke the include server's closure algorithm to yield a set of files
         and set of symbolic links --- both sets of files under client_root,
         which duplicates the part of the file system that CPP will need.
       - Transmit the file and link names on the socket using the RPC protocol.
      """
      statistics.StartTiming()
      currdir = distcc_pump_c_extensions.RCwd(self.rfile.fileno())
      cmd, compression = distcc_pump_c_extensions.RArgv(self.rfile.fileno(), True)

      try:
        files_and_links = include_analyzer.DoCompilationCommand(
            cmd, currdir, include_analyzer.client_root_keeper, compression)

      except NotCoveredError as inst:
        # Warn user. The 'Preprocessing locally' message is meant to
        # assure the user that the build process is otherwise intact.
        fd = tempfile.TemporaryFile(mode='w+')
        print(("Preprocessing locally. Include server not covering: %s for "
          + "translation unit '%s'") % (
            (inst.args and inst.args[0] or "unknown reason",
             include_analyzer.translation_unit)), file=fd, end=' ')
        # We don't include a stack trace here.
        include_analyzer.email_sender.MaybeSendEmail(fd,
                                                     never=not inst.send_email)
        # The empty argv list denotes failure. Communicate this
        # information back to the distcc client, so that it can fall
        # back to preprocessing on the client.
        distcc_pump_c_extensions.XArgv(self.wfile.fileno(), [])
        # A failed scan may have partially populated the input generation.
        # Never publish that state for the next translation unit.
        include_analyzer.ClearStatCaches()

      except SignalSIGTERM:
        # Normally, we will get this exception when the include server is no
        # longer needed. But we also handle it here, during the servicing of a
        # request. See basics.RaiseSignalSIGTERM.
        Debug(DEBUG_TRACE, "SIGTERM received while handling request.")
        raise
      except KeyboardInterrupt:
        # Propagate to the last-chance exception handler in Main.
        raise
      except SystemExit as inst:
        # When handler tries to exit (by invoking sys.exit, which in turn raises
        # SystemExit), something is really wrong. Terminate the include
        # server. But, print out an informative message first.
        fd = tempfile.TemporaryFile(mode='w+')
        print(("Preprocessing locally. Include server fatal error: '%s' for "
           + "translation unit '%s'") % (
          (inst.args, include_analyzer.translation_unit)), file=fd, end=' ')
        _PrintStackTrace(fd)
        include_analyzer.email_sender.MaybeSendEmail(fd, force=True)
        distcc_pump_c_extensions.XArgv(self.wfile.fileno(), [])
        sys.exit("Now terminating include server.")
      # All other exceptions are trapped here.
      except Exception as inst:
        # Internal error. Better be safe than sorry: terminate include
        # server. But show error to user on stderr. We hope this message will be
        # reported.
        fd = tempfile.TemporaryFile(mode='w+')
        print(("Preprocessing locally. Include server internal error: '%s: %s'"
           + " for translation unit '%s'") % (
          (inst.__class__, inst.args, include_analyzer.translation_unit)), file=fd)
        _PrintStackTrace(fd)
        # # Force this email through (if basics.opt_send_email is True), because
        # # this is the last one and this is an important case to report.
        include_analyzer.email_sender.MaybeSendEmail(fd, force=True)
        distcc_pump_c_extensions.XArgv(self.wfile.fileno(), [])
        raise SignalSIGTERM  # to be caught in Main with no further stack trace
      else:
        # No exception raised, include closure can be trusted.
        if memory_path is not None:
          RecordMemory(memory_path)
        distcc_pump_c_extensions.XArgv(self.wfile.fileno(), files_and_links)
      # Print out observed paths.
      if basics.opt_path_observation_re:
         include_analyzer.build_stat_cache.WarnAboutPathObservations(
             include_analyzer.translation_unit)
      # Finally, stop the clock and report statistics if needed.
      statistics.EndTiming()
      if basics.opt_statistics:
        statistics.PrintStatistics(include_analyzer)

  return IncludeHandler


def _ParseCommandLineOptions():
  """Parse arguments and options for the include server command.

  Returns:
    (include_server_port, pid_file, workers), where include_server_port
    is a string and pid_file is a string or None
  Modifies:
    option variables in module basics
  """
  try:
    opts, args = getopt.getopt(sys.argv[1:],
			       "d:estvw",
			       ["port=", "workers=",
                                "pid_file=",
                                "debug_pattern=",
                                "email",
                                "no-email",
                                "email_bound=",
                                "path_observation_re=",
                                "stat_reset_triggers=",
                                "statistics",
                                "time",
                                "no_force_dirs",
                                "verify",
                                "write_include_closure"])
  except getopt.GetoptError:
    # Print help information and exit.
    Usage()
    sys.exit(1)
  pid_file = None
  workers = 1
  include_server_port = None
  for opt, arg in opts:
    try:
      if opt in ("-d", "--debug_pattern"):
        basics.opt_debug_pattern = int(arg)
      if opt in ("--port", ):
        include_server_port = arg
      if opt in ("--pid_file",):
        pid_file = arg
      if opt == "--workers":
        workers = int(arg)
        if workers < 1:
          raise ValueError('workers must be positive')
      if opt in ("-e", "--email"):
        basics.opt_send_email = True
      if opt in ("--no-email",):
        basics.opt_send_email = False
      if opt in ("--email_bound",):
        basics.opt_email_bound = int(arg)
      if opt in ("--path_observation_re",):
        basics.opt_path_observation_re = re.compile(arg)
      if opt in ("--stat_reset_triggers",):
        basics.opt_stat_reset_triggers = (
          dict([ (glob_expr,
                  dict ([ (path, basics.Stamp(path))
                          for path in glob.glob(glob_expr) ]))
                 for glob_expr in arg.split(':') ]))
      if opt in ("--no_force_dirs",):
        basics.opt_no_force_dirs = True
      if opt in ("-s", "--statistics"):
        basics.opt_statistics = True
      if opt in ("-t", "--time"):
        basics.opt_print_times = True
      if opt in ("-v", "--verify"):
        basics.opt_verify = True
      if opt in ("-w", "--write_include_closure"):
        basics.opt_write_include_closure = True
    except ValueError:
      Usage()
      sys.exit(1)
  # We must have a port!
  if not include_server_port:
    print("INCLUDE_SERVER_PORT not provided. Aborting.", file=sys.stderr)
    print("-------------------------------------------", "\n", file=sys.stderr)
    Usage()
    sys.exit(1)
  return (include_server_port, pid_file, workers)


def _PrintTimes(times_at_start, times_at_fork, times_child):
  """Print elapsed, user, system, and user + system times."""
  # The os.times format stores user time in positions 0 and 2 (for parent and
  # children, resp.) Similarly, system time is stored in positions 1 and
  # 3. Elapsed time is in position 4. Elapsed time is measured relative to some
  # epoch whereas user and system time are 0 at the time of process creation.
  total_u = (times_at_fork[0] + times_at_fork[2]
             + times_child[0] + times_child[2])
  total_s = (times_at_fork[1] + times_at_fork[3]
             + times_child[1] + times_child[3])
  total_cpu = total_u + total_s
  total_e = times_child[4] - times_at_start[4]
  print("Include server timing. ", sys.stderr)
  print("Elapsed: %3.1fs User: %3.1fs System: %3.1fs User + System: %3.1fs" %
    (total_e, total_u, total_s, total_cpu), file=sys.stderr)


class AnalyzerPool:
  """Persistent, process-isolated analyzers sharing one listening socket.

  Each worker has its own cwd, compiler contexts and input generations.
  All published roots belong to the supervisor, so replacing a failed worker
  cannot invalidate inputs a distcc client is still transmitting.
  """

  def __init__(self, port):
    # Import before forking: immutable Python/native-library pages stay shared,
    # while each child creates its own analyzer and mutable compiler contexts.
    import compiler_analyzer
    self.analyzer_type = compiler_analyzer.CompilerAnalyzer
    self.workers = set()
    self.keeper = basics.ClientRootKeeper()
    self.keeper.CleanOutOthers()
    self.server = Queuingsocketserver(port, socketserver.BaseRequestHandler)
    self.memory_path = port + '.memory'
    try:
      with open(self.memory_path, 'xb') as output:
        output.write(struct.pack('=Q', 0))
    except BaseException:
      self.server.server_close()
      os.unlink(port)
      raise
    # Several workers can wake for one connection. Losing accept must not block.
    self.server.socket.setblocking(False)

  def start_worker(self, startup_fd=None):
    read_fd, write_fd = os.pipe()
    # Publish child ownership before a shutdown signal can unwind this method.
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
    try:
      pid = os.fork()
    except BaseException:
      os.close(read_fd)
      os.close(write_fd)
      signal.pthread_sigmask(signal.SIG_SETMASK, mask)
      raise
    if pid == 0:
      os.close(read_fd)
      if startup_fd is not None:
        os.close(startup_fd)
      status = 1
      try:
        os.setpgid(0, 0)
        signal.pthread_sigmask(signal.SIG_SETMASK, mask)
        analyzer = self.analyzer_type(
            self.keeper, basics.opt_stat_reset_triggers)
        analyzer.email_sender = _EmailSender()
        self.server.RequestHandlerClass = DistccIncludeHandlerGenerator(analyzer, self.memory_path)
        gc.set_threshold(basics.GC_THRESHOLD)
        os.write(write_fd, b'\n')
        os.close(write_fd)
        write_fd = None
        self.server.serve_forever()
      except (SignalSIGTERM, KeyboardInterrupt):
        status = 0
      except BaseException:
        traceback.print_exc()
      finally:
        if write_fd is not None:
          os.close(write_fd)
        self.server.server_close()
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(status)
    self.workers.add(pid)
    os.close(write_fd)
    try:
      signal.pthread_sigmask(signal.SIG_SETMASK, mask)
      if os.read(read_fd, 1) != b'\n':
        raise RuntimeError('include analyzer failed to start')
    finally:
      os.close(read_fd)

  def serve(self, count, ready_fd):
    try:
      for _ in range(count):
        self.start_worker(ready_fd)
      os.write(ready_fd, b'\n')
    finally:
      os.close(ready_fd)
    while True:
      # A killed scanner may leave its GCC subprocess behind. Reclaim that
      # group before reaping its leader, while its PID cannot be reused.
      exited = os.waitid(os.P_ALL, 0, os.WEXITED | os.WNOWAIT)
      assert exited is not None  # Blocking wait; WNOHANG is deliberately absent.
      pid = exited.si_pid
      self.signal_worker(pid, signal.SIGKILL)
      _, status = os.waitpid(pid, 0)
      self.workers.remove(pid)
      print('Include server: replacing analyzer (status %d)' % status,
            file=sys.stderr, flush=True)
      self.start_worker()

  @staticmethod
  def signal_worker(pid, sig):
    try:
      os.killpg(pid, sig)
    except ProcessLookupError:
      pass

  def close(self):
    self.server.server_close()
    for pid in self.workers:
      self.signal_worker(pid, signal.SIGTERM)
    # Native library calls can delay Python signal handling. Bound shutdown,
    # then reap every worker before removing the shared published-input roots.
    deadline = time.monotonic() + 2
    while self.workers and time.monotonic() < deadline:
      for pid in tuple(self.workers):
        if os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT):
          self.signal_worker(pid, signal.SIGKILL)
          os.waitpid(pid, 0)
          self.workers.remove(pid)
      if self.workers:
        time.sleep(.01)
    for pid in self.workers:
      self.signal_worker(pid, signal.SIGKILL)
      os.waitpid(pid, 0)
    self.workers.clear()
    self.keeper.CleanOutClientRoots()
    os.unlink(self.memory_path)
    os.unlink(str(self.server.server_address))


def Main():
  """Parse command line, fork, and start stream request handler."""
  # Remember the time spent in the parent.
  times_at_start = os.times()
  include_server_port, pid_file, workers = _ParseCommandLineOptions()
  read_fd, ready_fd = os.pipe()
  # Now spawn child so that parent can exit immediately after writing
  # the process id of child to the pid file.
  times_at_fork = os.times()
  pid = os.fork()
  if pid != 0:
    os.close(ready_fd)
    if pid_file:
      pid_file_fd = open(pid_file, "w")
      print(pid, file=pid_file_fd)
      pid_file_fd.close()
    try:
      if os.read(read_fd, 1) != b'\n':
        os.waitpid(pid, 0)
        sys.exit('Include server: analyzer pool failed to start')
    finally:
      os.close(read_fd)
  else:
    os.close(read_fd)
    pool = None
    try:
      pool = AnalyzerPool(include_server_port)
      pool.serve(workers, ready_fd)
    except (KeyboardInterrupt, SignalSIGTERM):
      Debug(DEBUG_TRACE, 'Include server shutting down.')
    finally:
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      signal.signal(signal.SIGINT, signal.SIG_IGN)
      if pool is not None:
        pool.close()
      if basics.opt_print_times:
        _PrintTimes(times_at_start, times_at_fork, os.times())


if __name__ == "__main__":
  # Treat SIGTERM (the default of kill) as Ctrl-C.
  signal.signal(signal.SIGTERM, basics.RaiseSignalSIGTERM)
  Main()
