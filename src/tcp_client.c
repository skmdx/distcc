/* Direct socket borrower: no broker payload relay and no per-file IPC.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include "distcc.h"
#include "hosts.h"
#include "exitcode.h"
#include "trace.h"
#include "tcp_job.h"
#include "frontend_fs.h"

struct input_stream { int fd, immutable; unsigned index; uint64_t offset, plain; };
static int local_snapshot(int source, uint64_t size)
{
    if (size > DCC_TCP_BYTES) return -1;
    int seals = fcntl(source, F_GET_SEALS);
    int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if (seals >= 0 && (seals & required) == required) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", source);
        return open(path, O_RDONLY | O_CLOEXEC);
    }
    int fd = dcc_job_memfd("distcc-local-input"), readonly = -1;
    off_t offset = 0;
    char path[64];
    if (fd < 0) return -1;
    /* Reopening /proc/self/fd in a different UID still checks inode mode.
     * An anonymous, sealed snapshot grants access only through the passed
     * descriptor without exposing or chmod'ing the client's named file. */
    while ((uint64_t)offset < size) {
        size_t amount = size - offset > DCC_TCP_CHUNK ? DCC_TCP_CHUNK : size - offset;
        ssize_t n = sendfile(fd, source, &offset, amount);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
    }
    if (fchmod(fd, 0444) || fcntl(fd, F_ADD_SEALS,
            F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) goto done;
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    readonly = open(path, O_RDONLY | O_CLOEXEC);
done:
    close(fd);
    return readonly;
}
static unsigned char *next_chunk(struct input_stream *stream, const struct dcc_job_input *in,
                                 size_t *size, off_t *file_offset, size_t *file_left)
{
    *file_left = 0;
    uint64_t remaining = in->plain_size - stream->plain;
    size_t amount = remaining < DCC_TCP_CHUNK ? remaining : DCC_TCP_CHUNK;
    if (in->encoding == DCC_JOB_RAW) {
        unsigned char *p = dcc_tcp_file_chunk(DCC_TCP_INPUT, stream->index, stream->fd,
            stream->offset, amount, amount == remaining, stream->immutable, size);
        if (!p) return NULL;
        if (*size == 16) { *file_offset = stream->offset; *file_left = amount; }
        stream->offset += amount; stream->plain += amount;
        return p;
    }
    /* Pump files already contain independent LZ4 blocks. Reframe, don't
     * decompress and recompress, and require their terminal marker. */
    unsigned char header[8];
    if (pread(stream->fd, header, 8, stream->offset) != 8) return NULL;
    uint32_t plain = dcc_tcp_get32(header), encoded = dcc_tcp_get32(header + 4);
    if ((!plain && remaining) || plain > DCC_TCP_CHUNK || plain > remaining ||
        (encoded && encoded >= plain)) return NULL;
    size_t payload = encoded ? encoded : plain;
    unsigned char *p = malloc(16);
    if (!p) return NULL;
    *file_offset = stream->offset + 8;
    *file_left = payload;
    stream->offset += 8 + payload; stream->plain += plain;
    int final = stream->plain == in->plain_size;
    if (final) {
        if (plain) {
            if (pread(stream->fd, header, 8, stream->offset) != 8 ||
                dcc_tcp_get64(header)) { free(p); return NULL; }
            stream->offset += 8;
        }
        if (stream->offset != in->wire_size) { free(p); return NULL; }
    }
    dcc_tcp_put32(p, DCC_TCP_INPUT | (encoded ? DCC_TCP_COMPRESSED : 0) | (final ? DCC_TCP_FINAL : 0));
    dcc_tcp_put32(p + 4, payload + 8);
    dcc_tcp_put32(p + 8, stream->index); dcc_tcp_put32(p + 12, plain);
    *size = 16; /* The immutable snapshot supplies the following file extent. */
    return p;
}
/* Metadata is already in memory. A READ joins its prefix and open file into
 * the same existing chunk stream, taking a private snapshot of mutable bytes
 * before compression/send. This does not introduce an extra frame or wait. */
static unsigned char *next_fs_chunk(const struct dcc_frontend_reply *reply,
                                    uint64_t *offset, unsigned char *buffer, size_t *size)
{
    uint64_t total = reply->size + reply->file_size;
    size_t amount = total - *offset < DCC_TCP_CHUNK ? total - *offset : DCC_TCP_CHUNK;
    size_t prefix = *offset < reply->size ? reply->size - *offset : 0;
    if (prefix > amount) prefix = amount;
    const void *data;
    if (prefix == amount) data = reply->data + *offset;
    else {
        if (prefix) memcpy(buffer, reply->data + *offset, prefix);
        size_t used = prefix;
        while (used < amount) {
            ssize_t n = pread(reply->fd, buffer + used, amount - used, *offset + used - reply->size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return NULL; /* A truncated source must fail the job. */
            used += n;
        }
        data = buffer;
    }
    unsigned char *p = malloc(amount + 16);
    if (!p) return NULL;
    *size = dcc_tcp_encode(DCC_TCP_FS_REPLY, UINT_MAX, data, amount, *offset + amount == total, p);
    if (*size == 16) { memcpy(p + 16, data, amount); *size += amount; }
    *offset += amount;
    return p;
}
int dcc_tcp_exchange(struct dcc_hostdef *host, struct dcc_job *job, char **files,
                      const char *cpp, const char *output, const char *deps, struct dcc_job_result *result)
{
    struct dcc_tcp_frame incoming = {0};
    struct input_stream stream = {.fd = -1};
    unsigned *requests = calloc(job->count ? job->count : 1, sizeof(*requests));
    unsigned char *requested = calloc(job->count ? job->count : 1, 1);
    unsigned char *buffer = malloc(DCC_TCP_CHUNK), *outgoing = NULL;
    uint64_t received[DCC_JOB_RESULT_COUNT] = {0}, id;
    unsigned finals = 0, read_request = 0, count = 0;
    size_t length = 0, sent = 0;
    size_t file_left = 0;
    off_t file_offset = 0;
    int ret = EXIT_IO_ERROR, have_result = 0;
    struct sockaddr_storage address;
    socklen_t address_size = sizeof(address);
    int local = !getsockname(host->broker_fd, (struct sockaddr *)&address, &address_size) &&
        address.ss_family == AF_UNIX;
    int local_input = local && job->kind == DCC_JOB_COMPILE && job->count == 1 &&
        job->inputs[0].encoding == DCC_JOB_RAW && job->inputs[0].fd >= 0;
    int send_input_fd = local_input;
    int shared_input = -1;
    struct dcc_frontend_reply filesystem = {.fd = -1};
    uint64_t fs_offset = 0, fs_requests = 0, fs_bytes = 0;
    time_t active = time(NULL);
    dcc_job_result_init(result);
    if (!host->broker_managed || host->broker_fd < 0 || !requests || !requested || !buffer ||
        getrandom(&id, sizeof(id), 0) != sizeof(id) || !id) goto done;
    if (local_input && (shared_input = local_snapshot(job->inputs[0].fd, job->inputs[0].plain_size)) < 0)
        goto done;
    size_t metadata_size = 0;
    unsigned char *manifest = dcc_job_encode_buffer(job, &metadata_size);
    if (!manifest) goto done;
    length = metadata_size + 16;
    outgoing = malloc(length);
    if (outgoing) {
        dcc_tcp_put32(outgoing, DCC_TCP_BEGIN);
        dcc_tcp_put32(outgoing + 4, metadata_size + 8);
        dcc_tcp_put64(outgoing + 8, id);
        memcpy(outgoing + 16, manifest, metadata_size);
    }
    free(manifest);
    if (!outgoing) goto done;
    for (unsigned i = 0; i < job->count; ++i)
        if (!local_input && dcc_job_file(&job->inputs[i]) && !job->inputs[i].cached) {
            requested[i] = 1; requests[count++] = i;
        }
    for (;;) {
        if (!outgoing && filesystem.data) {
            outgoing = next_fs_chunk(&filesystem, &fs_offset, buffer, &length);
            if (!outgoing) goto done;
            sent = 0;
        }
        if (!outgoing && (stream.fd >= 0 || read_request < count)) {
            if (stream.fd < 0) {
                stream.index = requests[read_request++]; stream.offset = stream.plain = 0;
                int retained = job->inputs[stream.index].fd;
                stream.fd = retained >= 0 ? fcntl(retained, F_DUPFD_CLOEXEC, 0) :
                    open(files ? files[stream.index] : cpp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (stream.fd < 0) goto done;
                int seals = fcntl(stream.fd, F_GET_SEALS);
                int immutable = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
                stream.immutable = seals >= 0 && (seals & immutable) == immutable;
            }
            outgoing = next_chunk(&stream, &job->inputs[stream.index],
                                  &length, &file_offset, &file_left);
            if (!outgoing) goto done;
            sent = 0;
        }
        int timeout = dcc_get_io_timeout() - (time(NULL) - active);
        if (timeout <= 0) { ret = EXIT_TIMEOUT; goto done; }
        struct pollfd p = {host->broker_fd, POLLIN | (outgoing ? POLLOUT : 0), 0};
        int ready = poll(&p, 1, timeout * 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) goto done;
        for (unsigned turn = 0; turn < 4 && (p.revents & (POLLIN | POLLHUP | POLLERR)); ++turn) {
            size_t before = incoming.used;
            int complete = dcc_tcp_frame_read(host->broker_fd, &incoming, DCC_TCP_CHUNK + 8U);
            if (before != incoming.used) active = time(NULL);
            if (complete < 0) goto done;
            if (!complete) break;
            if (complete) {
                uint32_t type = incoming.type & ~DCC_TCP_FLAGS;
                if (incoming.fd_plus_one && (!local || type != DCC_TCP_RESULT_FD)) goto done;
                if (type == DCC_TCP_PROGRESS && !have_result && incoming.size == 8 &&
                    incoming.type == DCC_TCP_PROGRESS && dcc_tcp_get64(incoming.data) == id) {
                    /* Worker execution can exceed the transport idle timeout. */
                } else if (type == DCC_TCP_NEED && !have_result && incoming.size && !(incoming.size % 4)) {
                    for (size_t i = 0; i < incoming.size; i += 4) {
                        unsigned index = dcc_tcp_get32(incoming.data + i);
                        if (index >= job->count || requested[index] ||
                            !dcc_job_file(&job->inputs[index]) || count == job->count) goto done;
                        requested[index] = 1; requests[count++] = index;
                    }
                } else if (type == DCC_TCP_FS_QUERY && incoming.type == DCC_TCP_FS_QUERY &&
                           job->kind == DCC_JOB_FRONTEND && !have_result &&
                           !outgoing && stream.fd < 0 && !filesystem.data && read_request == count) {
                    if (dcc_frontend_fs_reply(job, incoming.data, incoming.size, &filesystem)) goto done;
                    ++fs_requests; fs_bytes += filesystem.size + filesystem.file_size;
                    fs_offset = 0;
                } else if (type == DCC_TCP_RESULT && !have_result && incoming.size == DCC_TCP_RESULT_SIZE) {
                    have_result = 1;
                    result->status = dcc_tcp_get32(incoming.data);
                    result->mode = dcc_tcp_get32(incoming.data + 4);
                    if (result->status > 65535 || result->mode > 0777) goto done;
                    result->prepare_ns = dcc_tcp_get64(incoming.data + 40);
                    result->queue_ns = dcc_tcp_get64(incoming.data + 48);
                    result->execute_ns = dcc_tcp_get64(incoming.data + 56);
                    result->cpu_ns = dcc_tcp_get64(incoming.data + 64);
                    if (result->prepare_ns > 86400ULL * 1000000000 ||
                        result->queue_ns > 86400ULL * 1000000000 ||
                        result->execute_ns > 86400ULL * 1000000000) goto done;
                    uint64_t total = 0;
                    for (unsigned i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
                        uint64_t size = dcc_tcp_get64(incoming.data + 8 + i * 8);
                        if (size > DCC_TCP_BYTES - total) goto done;
                        total += size;
                        result->size[i] = size;
                        if (!size) finals |= 1U << i;
                        const char *path = i == DCC_JOB_OBJECT ? output : i == DCC_JOB_DEPS ? deps : NULL;
                        if (local && size) continue; /* The result frame supplies its own FD. */
                        if (!local && !result->status && path && strcmp(path, "-") && strcmp(path, "/dev/null")) {
                            if (asprintf(&result->temporary[i], "%s.distcc-XXXXXX", path) < 0) {
                                result->temporary[i] = NULL; goto done;
                            }
                            result->fd[i] = mkstemp(result->temporary[i]);
                        } else result->fd[i] = dcc_job_memfd("distcc-tcp-result");
                        if (result->fd[i] < 0) goto done;
                    }
                } else if (type == DCC_TCP_RESULT_FD && local && have_result && incoming.size == 4 &&
                           incoming.fd_plus_one) {
                    unsigned index = dcc_tcp_get32(incoming.data);
                    int fd = incoming.fd_plus_one - 1;
                    struct stat st;
                    if (index >= DCC_JOB_RESULT_COUNT || (finals & (1U << index)) || received[index] ||
                        fstat(fd, &st) || !S_ISREG(st.st_mode) ||
                        (uint64_t)st.st_size != result->size[index] ||
                        (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY) goto done;
                    if (result->fd[index] >= 0) close(result->fd[index]);
                    result->fd[index] = fd;
                    incoming.fd_plus_one = 0;
                    received[index] = result->size[index];
                    finals |= 1U << index;
                } else if (type == DCC_TCP_OUTPUT && have_result && !local) {
                    unsigned index;
                    size_t size;
                    const void *data;
                    if (dcc_tcp_unpack(&incoming, &index, buffer, &data, &size) ||
                        index >= DCC_JOB_RESULT_COUNT || (finals & (1U << index)) ||
                        size > result->size[index] - received[index] ||
                        dcc_writex(result->fd[index], data, size)) goto done;
                    received[index] += size;
                    if (incoming.type & DCC_TCP_FINAL) {
                        if (received[index] != result->size[index]) goto done;
                        finals |= 1U << index;
                    }
                } else if (type == DCC_TCP_RETRY && !have_result && incoming.size == 8 &&
                           dcc_tcp_get64(incoming.data) == id &&
                           !outgoing && stream.fd < 0 && !filesystem.data && read_request == count) {
                    ret = EXIT_BUSY; goto done;
                } else if (type == DCC_TCP_COMPLETE && have_result && incoming.size == 8 &&
                           dcc_tcp_get64(incoming.data) == id && finals == (1U << DCC_JOB_RESULT_COUNT) - 1 &&
                           !outgoing && stream.fd < 0 && !filesystem.data && read_request == count) {
                    ret = 0; goto done;
                } else goto done;
                dcc_tcp_frame_clear(&incoming);
            }
        }
        for (unsigned turn = 0; turn < 4 && (p.revents & POLLOUT) && outgoing; ++turn) {
            ssize_t n;
            if (send_input_fd) {
                struct iovec iov = {outgoing + sent, length - sent};
                union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control = {0};
                struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                    .msg_control = control.data, .msg_controllen = sizeof(control)};
                struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
                c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(c), &shared_input, sizeof(int));
                n = sendmsg(host->broker_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (n > 0) send_input_fd = 0; /* Rights accompany the first byte, even on a short write. */
            } else if (sent < length)
                n = send(host->broker_fd, outgoing + sent, length - sent,
                         MSG_NOSIGNAL | MSG_DONTWAIT | (file_left ? MSG_MORE : 0));
            else {
                n = sendfile(host->broker_fd, stream.fd, &file_offset, file_left);
                if (!n) goto done; /* Never accept a truncated source extent. */
                if (n > 0) file_left -= n;
            }
            if (n < 0 && errno != EAGAIN && errno != EINTR) goto done;
            if (n < 0) break;
            if (!n) goto done;
            if (n > 0) {
                active = time(NULL);
                if (sent < length) sent += n;
                if (sent == length && !file_left) {
                    if (dcc_tcp_get32(outgoing) & DCC_TCP_FINAL) {
                        unsigned type = dcc_tcp_get32(outgoing) & ~DCC_TCP_FLAGS;
                        if (type == DCC_TCP_FS_REPLY) dcc_frontend_fs_clear(&filesystem);
                        else if (type == DCC_TCP_INPUT) { close(stream.fd); stream.fd = -1; }
                    }
                    free(outgoing); outgoing = NULL; sent = 0;
                }
            }
        }
    }
done:
    if (fs_requests) rs_trace("frontend filesystem: %llu requests, %llu bytes",
        (unsigned long long)fs_requests, (unsigned long long)fs_bytes);
    if (ret)
        rs_trace("TCP exchange failed: inputs %u/%u, result %d, received frame %x (%zu bytes), pending write %zu/%zu",
                 read_request, count, have_result, incoming.type, incoming.used, sent, length);
    if (shared_input >= 0) close(shared_input);
    if (!ret) {
        unsigned cached = 0, transferred = 0;
        for (unsigned i = 0; i < job->count; ++i) {
            cached += job->inputs[i].cached;
            transferred += job->inputs[i].cached && requested[i];
        }
        if (cached) rs_trace("input cache: %u of %u entries transferred", transferred, job->count);
    }
    if (stream.fd >= 0) close(stream.fd);
    dcc_frontend_fs_clear(&filesystem);
    free(requests); free(requested); free(buffer); free(outgoing);
    dcc_tcp_frame_clear(&incoming);
    if (ret) dcc_job_result_free(result);
    return ret;
}
