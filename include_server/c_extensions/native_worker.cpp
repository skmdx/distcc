/* Descriptor dispatch and child ownership for resident local compilers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_worker.h"
#include "../../src/clang_worker.h"
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Support/raw_ostream.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <map>

namespace {
// Received descriptors can occupy a closed standard stream. Move those out of
// the way before installing stdio and the sole request FD inherited by Clang.
bool child_descriptors(int *fds) {
    for (unsigned i : {0U, 2U, 3U, 4U}) {
        if (fds[i] < 3) {
            fds[i] = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
            if (fds[i] < 0) return false;
        }
    }
    for (unsigned i = 0; i != 3; ++i)
        if (dup2(fds[i+2], i) < 0) return false;
    return dup2(fds[0], 3) >= 0 && !close_range(4, ~0U, 0);
}
}

int dcc_native_worker(int control, int (*execute)(int, bool)) {
    int events = -1, signals = -1;
    struct job { int peer; pid_t group; };
    std::map<pid_t, job> jobs;
    sigset_t blocked, previous_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous_mask)) { close(control); return 1; }
    struct sigaction action{}, previous_action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    bool installed = !sigaction(SIGCHLD, &action, &previous_action);
    auto cleanup = llvm::scope_exit([&] {
        for (auto [pid, run] : jobs) { kill(-run.group, SIGKILL); close(run.peer); }
        for (auto [pid, run] : jobs)
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        close(control);
        if (signals >= 0) close(signals);
        if (events >= 0) close(events);
        if (installed) sigaction(SIGCHLD, &previous_action, nullptr);
        sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    });
    if (!installed || (events = epoll_create1(EPOLL_CLOEXEC)) < 0 ||
        (signals = signalfd(-1, &blocked, SFD_CLOEXEC | SFD_NONBLOCK)) < 0) return 1;
    auto watch = [&](int fd, uint64_t id) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.u64 = id;
        return !epoll_ctl(events, EPOLL_CTL_ADD, fd, &event);
    };
    if (!watch(control, 0) || !watch(signals, 1) ||
        send(control, "R", 1, MSG_NOSIGNAL) != 1) return 1;
    for (;;) {
        epoll_event ready[32];
        int count = epoll_wait(events, ready, 32, -1);
        if (count < 0) { if (errno == EINTR) continue; return 1; }
        for (int i = 0; i != count; ++i) {
            uint64_t id = ready[i].data.u64;
            if (id == 1) {
                signalfd_siginfo signal;
                while (read(signals, &signal, sizeof(signal)) > 0)
                    if (signal.ssi_signo != SIGCHLD) return 128+signal.ssi_signo;
            } else if (id == 0) {
                dcc_clang_request request;
                int fds[5], received = 0;
                alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(fds))];
                iovec bytes{&request, sizeof(request)};
                msghdr message{};
                message.msg_iov = &bytes; message.msg_iovlen = 1;
                message.msg_control = ancillary; message.msg_controllen = sizeof(ancillary);
                ssize_t length;
                do { length = recvmsg(control, &message, MSG_CMSG_CLOEXEC); } while (length < 0 && errno == EINTR);
                if (length < 0) return 1;
                for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
                    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
                    unsigned amount = (header->cmsg_len-CMSG_LEN(0))/sizeof(int);
                    auto *descriptors = reinterpret_cast<int *>(CMSG_DATA(header));
                    for (unsigned n = 0; n != amount; ++n) {
                        if (received < 5) fds[received++] = descriptors[n];
                        else { close(descriptors[n]); message.msg_flags |= MSG_CTRUNC; }
                    }
                }
                auto close_received = llvm::scope_exit([&] {
                    for (int n = 0; n != received; ++n) if (fds[n] >= 0) close(fds[n]);
                });
                if (!length) return 0;
                if (length != sizeof(request) || message.msg_flags & (MSG_TRUNC | MSG_CTRUNC) || received != 5 ||
                    (request.operation != 'C' && request.operation != 'P') || request.group > INT_MAX) return 1;
                llvm::outs().flush(); llvm::errs().flush();
                pid_t parent = getpid(), pid = fork();
                if (pid < 0) return 1;
                if (!pid) {
                    // No Python API or at-fork interpreter work occurs in the
                    // compiler child. Its lifetime ends through _exit only.
                    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent ||
                        setpgid(0, request.group) || sigprocmask(SIG_SETMASK, &previous_mask, nullptr) ||
                        sigaction(SIGINT, &action, nullptr) ||
                        !child_descriptors(fds)) _exit(1);
                    int status = execute(3, request.operation == 'P');
                    llvm::outs().flush(); llvm::errs().flush();
                    _exit(status >= 0 ? status : 128-status);
                }
                pid_t group = request.group ? pid_t(request.group) : pid;
                jobs.emplace(pid, job{fds[1], group});
                fds[1] = -1;
                // Cancellation can remove the execution worker's group while
                // this request is queued. Contain that failure to its child;
                // it must not terminate the other users of this frontend.
                if (setpgid(pid, group) && errno != ESRCH) kill(pid, SIGKILL);
                if (!watch(jobs.at(pid).peer, uint64_t(pid)+2)) return 1;
            } else {
                auto found = jobs.find(id-2);
                if (found != jobs.end()) {
                    kill(-found->second.group, SIGKILL);
                    epoll_ctl(events, EPOLL_CTL_DEL, found->second.peer, nullptr);
                }
            }
        }
        // SIGCHLD coalesces; reap all completed invocations after each event
        // batch. Work follows completions, rather than probing every live PID.
        while (!jobs.empty()) {
            int status;
            rusage usage{};
            pid_t pid = wait4(-1, &status, WNOHANG, &usage);
            if (pid < 0) { if (errno == EINTR) continue; return 1; }
            if (!pid) break;
            auto found = jobs.find(pid);
            if (found == jobs.end()) return 1;
            uint64_t cpu = uint64_t(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000000 +
                uint64_t(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)*1000;
            dcc_clang_result reply{uint64_t(status), uint64_t(usage.ru_maxrss)*1024, cpu};
            int peer = found->second.peer;
            epoll_ctl(events, EPOLL_CTL_DEL, peer, nullptr);
            send(peer, &reply, sizeof(reply), MSG_NOSIGNAL);
            close(peer);
            jobs.erase(found);
        }
    }
}
