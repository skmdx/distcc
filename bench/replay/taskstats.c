/* Exit-time Linux task accounting for the saved-workload observer.
 * libnl owns netlink framing; the installed UAPI owns the taskstats layout.
 * This adapter exports aggregates, never scheduler transitions. */
#include <errno.h>
#include <inttypes.h>
#include <linux/taskstats.h>
#include <netlink/attr.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <netlink/socket.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *output;
static void check(int result, const char *operation)
{
    if (result < 0) {
        fprintf(stderr, "%s: %s\n", operation, nl_geterror(result));
        exit(1);
    }
}

static void string(const char *s, size_t size)
{
    fputc('"', output);
    for (size_t i = 0; i < size && s[i]; ++i) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') fprintf(output, "\\%c", c);
        else if (c < 32) fprintf(output, "\\u%04x", c);
        else fputc(c, output);
    }
    fputc('"', output);
}

static int receive(struct nl_msg *message, void *unused)
{
    struct nlattr *outer, *inner;
    int left, remaining;
    struct genlmsghdr *header = nlmsg_data(nlmsg_hdr(message));
    (void)unused;
    nla_for_each_attr(outer, genlmsg_attrdata(header, 0), genlmsg_attrlen(header, 0), left) {
        if (nla_type(outer) != TASKSTATS_TYPE_AGGR_PID) continue;
        nla_for_each_nested(inner, outer, remaining) {
            if (nla_type(inner) != TASKSTATS_TYPE_STATS) continue;
            struct taskstats s = {0};
            /* Taskstats is append-only. Require the fields actually consumed,
             * not an exact kernel/header version match. */
            if (nla_len(inner) < (int)(offsetof(struct taskstats, ac_btime64) + sizeof(s.ac_btime64))) {
                fprintf(stderr, "taskstats response lacks required accounting fields\n");
                exit(1);
            }
            size_t size = nla_len(inner);
            memcpy(&s, nla_data(inner), size < sizeof(s) ? size : sizeof(s));
            struct timespec now;
            if (clock_gettime(CLOCK_REALTIME, &now)) { perror("clock_gettime"); exit(1); }
            fprintf(output, "{\"pid\":%u,\"ppid\":%u,\"comm\":", s.ac_pid, s.ac_ppid);
            string(s.ac_comm, sizeof(s.ac_comm));
            fprintf(output,
                ",\"version\":%u,\"birth_seconds\":%" PRIu64
                ",\"elapsed_us\":%" PRIu64 ",\"cpu_ns\":%" PRIu64
                ",\"scheduled_ns\":%" PRIu64 ",\"runnable_ns\":%" PRIu64
                ",\"dispatches\":%" PRIu64 ",\"voluntary_switches\":%" PRIu64
                ",\"involuntary_switches\":%" PRIu64
                ",\"read_bytes\":%" PRIu64 ",\"write_bytes\":%" PRIu64
                ",\"received_ns\":%" PRIu64 "}\n",
                s.version, (uint64_t)s.ac_btime64, (uint64_t)s.ac_etime,
                (uint64_t)s.cpu_run_real_total, (uint64_t)s.cpu_run_virtual_total,
                (uint64_t)s.cpu_delay_total, (uint64_t)s.cpu_count,
                (uint64_t)s.nvcsw, (uint64_t)s.nivcsw,
                (uint64_t)s.read_bytes, (uint64_t)s.write_bytes,
                (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec);
            if (ferror(output)) { perror("taskstats output"); exit(1); }
        }
    }
    return NL_OK;
}

static void subscribe(struct nl_sock *socket, int family, const char *mask, int attribute)
{
    struct nl_msg *message = nlmsg_alloc();
    if (!message) { perror("nlmsg_alloc"); exit(1); }
    genlmsg_put(message, NL_AUTO_PORT, NL_AUTO_SEQ, family, 0, NLM_F_REQUEST | NLM_F_ACK,
                TASKSTATS_CMD_GET, TASKSTATS_GENL_VERSION);
    check(nla_put_string(message, attribute, mask), "taskstats CPU mask");
    check(nl_send_auto(socket, message), "taskstats subscribe");
    nlmsg_free(message);
    check(nl_wait_for_ack(socket), "taskstats acknowledgement");
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: taskstats OUTPUT ONLINE_CPUS\n"); return 2; }
    output = fopen(argv[1], "w");
    if (!output) { perror(argv[1]); return 1; }
    struct nl_sock *socket = nl_socket_alloc();
    if (!socket) return 1;
    check(genl_connect(socket), "generic netlink");
    int family = genl_ctrl_resolve(socket, TASKSTATS_GENL_NAME);
    check(family, "TASKSTATS family");
    nl_socket_disable_seq_check(socket);
    check(nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM, receive, NULL), "taskstats callback");
    subscribe(socket, family, argv[2], TASKSTATS_CMD_ATTR_REGISTER_CPUMASK);
    puts("ready"); fflush(stdout);
    struct pollfd descriptors[] = {{nl_socket_get_fd(socket), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    for (;;) {
        if (poll(descriptors, 2, -1) < 0) { perror("poll"); return 1; }
        if (descriptors[0].revents) check(nl_recvmsgs_default(socket), "taskstats receive (including overflow)");
        if (descriptors[1].revents) break;
    }
    subscribe(socket, family, argv[2], TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK);
    nl_socket_free(socket);
    return fclose(output) ? 1 : 0;
}
