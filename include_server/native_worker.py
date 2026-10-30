"""Resident libraries in the caller's namespace; one isolated process per job."""
import array
import os
import selectors
import signal
import socket
import struct
import sys
import traceback

from include_server import distcc_pump_c_extensions as protocol


def serve(descriptor, frontend):
    control = socket.socket(fileno=descriptor)
    wake_read, wake_write = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
    signal.signal(signal.SIGCHLD, lambda *_: None)
    signal.set_wakeup_fd(wake_write)
    selector = selectors.DefaultSelector()
    selector.register(control, selectors.EVENT_READ)
    selector.register(wake_read, selectors.EVENT_READ)
    jobs = {}
    control.sendall(b'R')
    try:
        while True:
            for event, _ in selector.select():
                if event.fileobj == wake_read:
                    os.read(wake_read, 4096)
                elif event.fileobj == control:
                    data, ancillary, flags, _ = control.recvmsg(1, socket.CMSG_SPACE(5 * array.array('i').itemsize),
                                                               socket.MSG_CMSG_CLOEXEC)
                    fds = []
                    for level, kind, values in ancillary:
                        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                            fds.extend(array.array('i', values))
                    try:
                        if not data:
                            return 0
                        if data not in (b'C', b'P') or flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or len(fds) != 5:
                            raise ValueError('invalid local compiler request')
                        cwd, compiler = protocol.RArgv(fds[0])
                        argv = protocol.RArgv(fds[0])
                        argv[0] = compiler
                        reply = socket.socket(fileno=fds[1])
                        fds[1] = -1
                        pid = os.fork()
                        if not pid:
                            signal.set_wakeup_fd(-1)
                            signal.signal(signal.SIGCHLD, signal.SIG_DFL)
                            control.close(); selector.close()
                            os.close(wake_read); os.close(wake_write)
                            reply.close()
                            for peer in jobs.values(): peer.close()
                            status = 1
                            try:
                                os.setpgid(0, 0)
                                for dest, source in enumerate(fds[2:]): os.dup2(source, dest)
                                for fd in fds:
                                    if fd >= 0 and (data != b'P' or fd != fds[0]): os.close(fd)
                                os.chdir(cwd)
                                if data == b'P':
                                    prepared = frontend.prepare(argv)
                                    if prepared is not None:
                                        # The input has been fully consumed. Reuse
                                        # its memfd for the captured command.
                                        os.ftruncate(fds[0], 0)
                                        os.lseek(fds[0], 0, os.SEEK_SET)
                                        for values in prepared: protocol.XArgv(fds[0], values)
                                        status = 0
                                    os.close(fds[0])
                                else:
                                    status = frontend.run(argv)
                            except BaseException:
                                traceback.print_exc()
                            sys.stdout.flush(); sys.stderr.flush()
                            os._exit(status if status >= 0 else 128-status)
                        try: os.setpgid(pid, pid)
                        except ProcessLookupError: pass
                        jobs[pid] = reply
                        selector.register(reply, selectors.EVENT_READ, pid)
                    finally:
                        for fd in fds:
                            if fd >= 0: os.close(fd)
                else:
                    # Client closure cancels the entire invocation's group.
                    pid = event.data
                    try: os.killpg(pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    selector.unregister(event.fileobj)
            for pid, peer in list(jobs.items()):
                done, status, usage = os.wait4(pid, os.WNOHANG)
                if done:
                    try: selector.unregister(peer)
                    except KeyError: pass
                    try: peer.sendall(struct.pack('=2Q', status, round((usage.ru_utime + usage.ru_stime) * 1e9)))
                    except BrokenPipeError: pass
                    peer.close()
                    del jobs[pid]
    finally:
        for pid, peer in jobs.items():
            try: os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError: pass
            os.waitpid(pid, 0); peer.close()
        control.close(); selector.close()
        signal.set_wakeup_fd(-1)
        os.close(wake_read); os.close(wake_write)
