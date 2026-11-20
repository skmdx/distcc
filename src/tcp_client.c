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
/* TCP has no ancillary descriptors. Read ahead into one maximum wire chunk;
 * frames borrow its payload until consumed. Local IPC retains exact recvmsg
 * boundaries so SCM_RIGHTS stays attached to its own frame. */
struct tcp_input { unsigned char *wire; size_t begin, end; };
static int read_remote(int fd, struct tcp_input *input, struct dcc_tcp_frame *frame)
{
    for (;;) {
        size_t available = input->end - input->begin;
        if (available >= 8 && frame->used < 8) {
            memcpy(frame->header, input->wire + input->begin, 8);
            if (dcc_tcp_frame_header(frame, DCC_TCP_CHUNK + 8U)) return -1;
        }
        /* Bytes already examined do not constitute internal readiness when
         * the rest of the frame has yet to arrive. */
        frame->used = available;
        if (available >= 8 && available >= frame->size + 8) {
            frame->data = input->wire + input->begin + 8;
            return 1;
        }
        if (input->end == DCC_TCP_CHUNK + 16U) {
            memmove(input->wire, input->wire + input->begin, available);
            input->begin = 0; input->end = available;
        }
        ssize_t n = recv(fd, input->wire + input->end,
                         DCC_TCP_CHUNK + 16U - input->end, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return -1;
        input->end += n;
    }
}
static void consume_remote(struct tcp_input *input, struct dcc_tcp_frame *frame)
{
    input->begin += frame->size + 8;
    if (input->begin == input->end) input->begin = input->end = 0;
    *frame = (struct dcc_tcp_frame){0};
}
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
                                 unsigned char *p, unsigned char *snapshot,
                                 size_t *size, off_t *file_offset, size_t *file_left)
{
    *file_left = 0;
    uint64_t remaining = in->plain_size - stream->plain;
    size_t amount = remaining < DCC_TCP_CHUNK ? remaining : DCC_TCP_CHUNK;
    if (in->encoding == DCC_JOB_RAW) {
        p = dcc_tcp_file_chunk(DCC_TCP_INPUT, stream->index, stream->fd,
            stream->offset, amount, amount == remaining, stream->immutable, p, snapshot, size);
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
    *file_offset = stream->offset + 8;
    *file_left = payload;
    stream->offset += 8 + payload; stream->plain += plain;
    int final = stream->plain == in->plain_size;
    if (final) {
        if (plain) {
            if (pread(stream->fd, header, 8, stream->offset) != 8 ||
                dcc_tcp_get64(header)) return NULL;
            stream->offset += 8;
        }
        if (stream->offset != in->wire_size) return NULL;
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
static int next_fs_chunk(const struct dcc_frontend_reply *reply, uint64_t *offset,
                         unsigned char *buffer, unsigned char *p, size_t *size)
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
            if (n <= 0) return -1; /* A truncated source must fail the job. */
            used += n;
        }
        data = buffer;
    }
    *size = dcc_tcp_encode(DCC_TCP_FS_REPLY, UINT_MAX, data, amount, *offset + amount == total, p);
    if (*size == 16) { memcpy(p + 16, data, amount); *size += amount; }
    *offset += amount;
    return 0;
}
static int stage_outputs(struct dcc_job *job, struct dcc_job_result *r,
                         const unsigned char *data, size_t length)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < DCC_JOB_RESULT_COUNT; ++i) total += r->size[i];
    while (length > 12) {
        uint64_t size = dcc_tcp_get64(data);
        unsigned mode = dcc_tcp_get32(data + 8);
        const char *name = (const char *)data + 12;
        size_t n = strnlen(name, length - 12);
        if (!n || n > NAME_MAX || n == length - 12 || strchr(name, '/') ||
            !strcmp(name, ".") || !strcmp(name, "..") || size > DCC_TCP_BYTES - total || mode > 0777)
            return -1;
        for (unsigned i = 0; i < r->named_count; ++i)
            if (!strcmp(r->named[i].name, name)) return -1;
        void *next = realloc(r->named, (r->named_count + 1) * sizeof(*r->named));
        if (!next) return -1;
        r->named = next;
        struct dcc_named_output *f = &r->named[r->named_count++];
        *f = (struct dcc_named_output){.fd = -1, .mode = mode, .size = size, .name = strdup(name)};
        if (!f->name) return -1;
        char **p = job->destinations;
        while (p && *p && strcmp(*p, name)) p += 2;
        if (job->linkdir && !strncmp(name, ".distcc-link-", 13)) {
            if (asprintf(&f->destination,"%s/%s",job->linkdir,name)<0) return -1;
        } else if (p && *p) f->destination = strdup(p[1]);
        else if (!job->outdir) return -1;
        else if (asprintf(&f->destination, "%s/%s", job->outdir, name) < 0) return -1;
        if (!f->destination) return -1;
        const char *base = strrchr(f->destination, '/');
        char *temporary;
        if (!base || asprintf(&temporary, "%.*s/.distcc-rust-XXXXXX", (int)(base-f->destination),
                              f->destination) < 0) return -1;
        f->fd = mkostemp(temporary, O_CLOEXEC);
        if (f->fd < 0) { free(temporary); return -1; }
        f->temporary = temporary;
        total += size; length -= 13 + n; data += 13 + n;
    }
    return length ? -1 : 0;
}
static uint64_t transaction_id(void)
{
    static pid_t owner;
    static uint64_t counter;
    pid_t pid = getpid();
    if (owner != pid) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        counter = ((uint64_t)pid << 32) ^ ((uint64_t)now.tv_sec * 1000000000 + now.tv_nsec);
        owner = pid;
    }
    /* IDs correlate frames on one leased connection; they are not secrets.
     * Reseed after fork and retain the sequence across retries. */
    if (!++counter) ++counter;
    return counter;
}
int dcc_tcp_exchange(struct dcc_hostdef *host, struct dcc_job *job, char **files,
                      const char *cpp, const char *output, const char *deps, struct dcc_job_result *result)
{
    struct dcc_tcp_frame incoming = {0};
    struct input_stream stream = {.fd = -1};
    unsigned stack_req[16];
    unsigned char stack_reqd[16] = {0};
    unsigned *requests = job->count <= 16 ? stack_req : calloc(job->count, sizeof(*requests));
    unsigned char *requested = job->count <= 16 ? stack_reqd : calloc(job->count, 1);
    /* The command-line client runs exchanges sequentially, including retries.
     * Retain only storage; all job state and descriptors are reset below. */
    static unsigned char p_buf[DCC_TCP_CHUNK + 16U], snap_buf[DCC_TCP_CHUNK + 16U];
    static unsigned char *large_manifest;
    static size_t manifest_capacity;
    unsigned char *outgoing = p_buf;
    uint64_t received[DCC_JOB_RESULT_COUNT] = {0}, id = transaction_id();
    unsigned finals = 0, read_request = 0, count = 0;
    size_t length = 0, sent = 0;
    size_t file_left = 0;
    off_t file_offset = 0;
    int ret = EXIT_IO_ERROR, have_result = 0;
    unsigned named_index = 0;
    int names_received = 0;
    struct dcc_job_result early;
    dcc_job_result_init(&early);
    char *notice = NULL;
    uint64_t early_bytes = 0;
    uint64_t named_bytes = 0;
    struct sockaddr_storage address;
    socklen_t address_size = sizeof(address);
    int local = !getsockname(host->broker_fd, (struct sockaddr *)&address, &address_size) &&
        address.ss_family == AF_UNIX;
    static unsigned char buffer[2 * DCC_TCP_CHUNK + 16U];
    struct tcp_input input = {.wire = local ? NULL : buffer + DCC_TCP_CHUNK};
    int local_input = local && job->kind == DCC_JOB_COMPILE && job->count == 1 &&
        job->inputs[0].encoding == DCC_JOB_RAW && job->inputs[0].fd >= 0;
    int send_input_fd = local_input;
    int shared_input = -1;
    static struct dcc_frontend_reply filesystem = {.fd = -1};
    uint64_t fs_offset = 0, fs_requests = 0, fs_bytes = 0;
    unsigned needs = 0, reads = 0, read_batches = 0;
    time_t active = time(NULL);
    dcc_job_result_init(result);
    if (!host->broker_managed || host->broker_fd < 0 || !requests || !requested) goto done;
    if (local_input && (shared_input = local_snapshot(job->inputs[0].fd, job->inputs[0].plain_size)) < 0)
        goto done;
    size_t metadata_size = 0;
    unsigned char *manifest = dcc_job_encode_buffer(job, &metadata_size);
    if (!manifest) goto done;
    length = metadata_size + 16;
    if (length > sizeof(p_buf)) {
        if (manifest_capacity < length) {
            void *next = realloc(large_manifest, length);
            if (!next) { free(manifest); goto done; }
            large_manifest = next;
            manifest_capacity = length;
        }
        outgoing = large_manifest;
    }
    dcc_tcp_put32(outgoing, DCC_TCP_BEGIN);
    dcc_tcp_put32(outgoing + 4, metadata_size + 8);
    dcc_tcp_put64(outgoing + 8, id);
    memcpy(outgoing + 16, manifest, metadata_size);
    free(manifest);
    for (unsigned i = 0; i < job->count; ++i)
        if (!local_input && dcc_job_file(&job->inputs[i]) && job->inputs[i].cached != DCC_CACHE_LOOKUP) {
            requested[i] = 1; requests[count++] = i;
        }
    for (;;) {
        /* A bounded send turn also prepares the next chunk. Poll only after
         * this turn, rather than after every immediately writable chunk. */
        for (unsigned turn = 0; turn < 4; ++turn) {
            if (!length && filesystem.size) {
                outgoing = p_buf;
                if (next_fs_chunk(&filesystem, &fs_offset, buffer, outgoing, &length)) goto done;
                sent = 0;
            }
            if (!length && (stream.fd >= 0 || read_request < count)) {
                if (stream.fd < 0) {
                    stream.index = requests[read_request++]; stream.offset = stream.plain = 0;
                    int retained = job->inputs[stream.index].fd;
                    stream.fd = retained >= 0 ? fcntl(retained, F_DUPFD_CLOEXEC, 0) :
                        open(files ? files[stream.index] : cpp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                    if (stream.fd < 0) goto done;
                    int seals = fcntl(stream.fd, F_GET_SEALS);
                    int immutable = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
                    /* Rust holds this generation and verifies its descriptor and
                     * pathname again before publishing any output. */
                    stream.immutable = job->kind == DCC_JOB_RUST || (seals >= 0 && (seals & immutable) == immutable);
                }
                outgoing = next_chunk(&stream, &job->inputs[stream.index],
                                      p_buf, snap_buf, &length, &file_offset, &file_left);
                if (!outgoing) goto done;
                sent = 0;
            }
            if (!length) break;
            /* Prepared replies can be sent immediately. Register POLLOUT only
             * after a nonblocking write leaves bytes pending. */
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
                        if (type == DCC_TCP_FS_REPLY) dcc_frontend_fs_reset(&filesystem);
                        else if (type == DCC_TCP_INPUT) { close(stream.fd); stream.fd = -1; }
                    }
                    length = 0; sent = 0;
                }
            }
        }
        int timeout = dcc_get_io_timeout() - (time(NULL) - active);
        if (timeout <= 0) { ret = EXIT_TIMEOUT; goto done; }
        int pending = !length && (filesystem.size || stream.fd >= 0 || read_request < count);
        struct pollfd p = {host->broker_fd, POLLIN | (length ? POLLOUT : 0), 0};
        int buffered = input.end - input.begin > incoming.used;
        int ready;
        if (buffered) { p.revents = POLLIN; ready = 1; }
        else ready = poll(&p, 1, pending ? 0 : timeout * 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (!ready && pending) continue; /* Prepare the next chunk without waiting for I/O. */
        if (ready <= 0) goto done;
        for (unsigned turn = 0; turn < 4 && (p.revents & (POLLIN | POLLHUP | POLLERR)); ++turn) {
            size_t before = incoming.used;
            int complete = local ? dcc_tcp_frame_read(host->broker_fd, &incoming, DCC_TCP_CHUNK + 8U) :
                                   read_remote(host->broker_fd, &input, &incoming);
            if (before != incoming.used) active = time(NULL);
            if (complete < 0) goto done;
            if (!complete) break;
            if (complete) {
                uint32_t type = incoming.type & ~DCC_TCP_FLAGS;
                if (incoming.fd_plus_one && (!local || type != DCC_TCP_RESULT_FD)) goto done;
                if (type == DCC_TCP_PROGRESS && !have_result && incoming.size == 8 &&
                    incoming.type == DCC_TCP_PROGRESS && dcc_tcp_get64(incoming.data) == id) {
                    /* Worker execution can exceed the transport idle timeout. */
                } else if (type == DCC_TCP_METADATA_BEGIN && !have_result && !notice &&
                           job->publish_metadata && incoming.size > 14) {
                    size_t name = strnlen((char *)incoming.data + 12, incoming.size-12);
                    size_t prefix = 13 + name;
                    if (prefix >= incoming.size || memchr(incoming.data+prefix, 0, incoming.size-prefix) ||
                        stage_outputs(job, &early, incoming.data, prefix) || early.named_count != 1 ||
                        !(notice = strndup((char *)incoming.data+prefix, incoming.size-prefix))) goto done;
                } else if (type == DCC_TCP_METADATA_DATA && !have_result && notice && !job->metadata_published) {
                    struct dcc_named_output *f = early.named;
                    if (incoming.size > f->size-early_bytes || dcc_writex(f->fd, incoming.data, incoming.size)) goto done;
                    early_bytes += incoming.size;
                    if (early_bytes == f->size) {
                        if (job->publish_metadata(job->metadata_context, f, notice)) goto done;
                        job->metadata_published = 1;
                    }
                } else if (type == DCC_TCP_NEED && !have_result && incoming.size &&
                           incoming.size == dcc_tcp_bitmap_size(job->count)) {
                    ++needs;
                    for (unsigned index = 0; index < incoming.size * 8; ++index) {
                        if (!(incoming.data[index / 8] & (1U << (index % 8)))) continue;
                        if (index >= job->count || requested[index] || !job->inputs[index].cached ||
                            !dcc_job_file(&job->inputs[index])) goto done;
                        requested[index] = 1; requests[count++] = index;
                    }
                } else if (type == DCC_TCP_FS_QUERY && incoming.type == DCC_TCP_FS_QUERY &&
                           dcc_job_frontend(job) && !have_result &&
                           !length && stream.fd < 0 && !filesystem.size && read_request == count) {
                    if (dcc_frontend_fs_reply(job, incoming.data, incoming.size, &filesystem)) goto done;
                    unsigned op = dcc_tcp_get32(incoming.data);
                    reads += op == DCC_FS_READ;
                    read_batches += op == DCC_FS_BATCH && incoming.size >= 12 &&
                                    dcc_tcp_get32(incoming.data + 8) == DCC_FS_READ;
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
                } else if (type == DCC_TCP_OUTPUT_NAMES && (job->outdir || job->llvm_outputs) && have_result &&
                           !result->status && !names_received) {
                    if (stage_outputs(job, result, incoming.data, incoming.size)) goto done;
                    names_received = 1;
                    while (named_index < result->named_count && !result->named[named_index].size) ++named_index;
                } else if (type == DCC_TCP_OUTPUT && have_result) {
                    unsigned index;
                    size_t size;
                    const void *data;
                    if (dcc_tcp_unpack(&incoming, &index, buffer, &data, &size)) goto done;
                    if (index >= DCC_JOB_RESULT_COUNT) {
                        if (index - DCC_JOB_RESULT_COUNT != named_index || named_index >= result->named_count)
                            goto done;
                        struct dcc_named_output *f = &result->named[named_index];
                        if (!size || size > f->size - named_bytes || dcc_writex(f->fd, data, size)) goto done;
                        named_bytes += size;
                        if ((named_bytes == f->size) != !!(incoming.type & DCC_TCP_FINAL)) goto done;
                        if (named_bytes == f->size) {
                            named_bytes = 0;
                            do { ++named_index; } while (named_index < result->named_count && !result->named[named_index].size);
                        }
                    } else {
                        if (local || (finals & (1U << index)) ||
                            size > result->size[index] - received[index] ||
                            dcc_writex(result->fd[index], data, size)) goto done;
                        received[index] += size;
                        if (incoming.type & DCC_TCP_FINAL) {
                            if (received[index] != result->size[index]) goto done;
                            finals |= 1U << index;
                        }
                    }
                } else if (type == DCC_TCP_RETRY && !have_result && incoming.size == 8 &&
                           dcc_tcp_get64(incoming.data) == id &&
                           !length && stream.fd < 0 && !filesystem.size && read_request == count) {
                    ret = EXIT_BUSY; goto done;
                } else if (type == DCC_TCP_COMPLETE && have_result && incoming.size == sizeof(struct dcc_tcp_complete) &&
                           (!notice || job->metadata_published) &&
                           (!(job->outdir || job->llvm_outputs) || result->status || names_received) &&
                           named_index == result->named_count &&
                           dcc_tcp_get64(incoming.data) == id && finals == (1U << DCC_JOB_RESULT_COUNT) - 1 &&
                           !length && stream.fd < 0 && !filesystem.size && read_request == count) {
                    /* No next job exists until this connection is returned
                     * to the pool. Unsolicited trailing bytes are not a result. */
                    if (!local && input.end - input.begin != incoming.size + 8) goto done;
                    memcpy(&result->load, incoming.data + 8, sizeof(result->load));
                    ret = 0; goto done;
                } else goto done;
                if (local) dcc_tcp_frame_next(&incoming);
                else consume_remote(&input, &incoming);
                if (type == DCC_TCP_FS_QUERY) break; /* The compiler awaits this reply. */
            }
        }
    }
done:
    /* A Rust input stream or result belongs to an accepted transaction.
     * Only a connection lost during acquisition or BEGIN is retryable. */
    if (ret == EXIT_IO_ERROR && (job->kind == DCC_JOB_RUST || job->kind == DCC_JOB_RUST_CGU) &&
        (read_request || needs || have_result || notice))
        ret = EXIT_DISTCC_FAILED;
    free(notice);
    dcc_job_result_free(&early);
    if (fs_requests) rs_trace("frontend filesystem: %llu requests, %llu bytes, %u reads, %u read batches",
        (unsigned long long)fs_requests, (unsigned long long)fs_bytes, reads, read_batches);
    if (ret)
        rs_trace("TCP exchange failed: inputs %u/%u, result %d, received frame %x (%zu bytes), pending write %zu/%zu",
                 read_request, count, have_result, incoming.type, incoming.used, sent, length);
    if (shared_input >= 0) close(shared_input);
    if (!ret) {
        rs_trace("BEGIN %zu bytes, NEED %u, main pushed %d", metadata_size + 8, needs,
                 job->main_input && job->inputs[job->main_input - 1].cached == DCC_CACHE_PUSH);
        unsigned cached = 0, transferred = 0;
        for (unsigned i = 0; i < job->count; ++i) {
            cached += !!job->inputs[i].cached;
            transferred += job->inputs[i].cached && requested[i];
        }
        if (cached) rs_trace("input cache: %u of %u entries transferred", transferred, job->count);
    }
    if (stream.fd >= 0) close(stream.fd);
    dcc_frontend_fs_reset(&filesystem);
    if (requests != stack_req) free(requests);
    if (requested != stack_reqd) free(requested);
    if (local) dcc_tcp_frame_clear(&incoming);
    if (ret) dcc_job_result_free(result);
    return ret;
}
