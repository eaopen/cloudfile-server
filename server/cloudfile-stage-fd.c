#define _GNU_SOURCE
#include <glib.h>
#include <jansson.h>
#include "cloudfile-stage-fd.h"

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int stage_packet (const char *packet, size_t length, int fd, char **metadata)
{
    const char domain[] = "CLOUDFILE-STAGE-V1\n";
    if (length <= sizeof(domain) - 1 || memcmp(packet, domain, sizeof(domain) - 1)) return -1;
    json_t *root = json_loadb (packet + sizeof(domain) - 1, length - sizeof(domain) + 1,
                              JSON_REJECT_DUPLICATES, NULL);
    if (!json_is_object(root) || json_object_size(root) != 4) {
        if (root) json_decref(root);
        return -1;
    }
    json_t *stage = json_object_get(root, "stage_id"), *store = json_object_get(root, "store_id");
    json_t *size = json_object_get(root, "length"), *digest = json_object_get(root, "sha256");
    const char *stage_id = json_string_value(stage), *store_id = json_string_value(store);
    const char *bytes = json_string_value(size), *sha = json_string_value(digest);
    int result = -1;
    struct stat info;
    if (!stage_id || !store_id || json_string_length(stage) != 36 || json_string_length(store) != 36 ||
        !g_uuid_string_is_valid(stage_id) || !g_uuid_string_is_valid(store_id) ||
        strspn(stage_id, "0123456789abcdef-") != 36 || strspn(store_id, "0123456789abcdef-") != 36 ||
        !bytes || !json_string_length(size) || json_string_length(size) > 19 ||
        json_string_length(size) != strlen(bytes) || strspn(bytes, "0123456789") != strlen(bytes) ||
        (bytes[0] == '0' && bytes[1]) || !sha || json_string_length(digest) != 64 ||
        strspn(sha, "0123456789abcdef") != 64) goto out;
    errno = 0;
    guint64 expected = g_ascii_strtoull(bytes, NULL, 10);
    gboolean overflow = errno == ERANGE;
    int flags = fcntl(fd, F_GETFL);
    if (overflow || expected > G_MAXINT64 || flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
        fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_nlink != 0 ||
        (info.st_mode & 0777) != 0400 || info.st_size < 0 || (guint64)info.st_size != expected) goto out;
    /* Actual content digest is independently remeasured by the native indexer. */
    *metadata = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS);
    result = *metadata ? 0 : -1;
out:
    json_decref(root);
    return result;
}
#endif

int cf_stage_receive_fd (int connection, guint32 expected_peer_uid, int *stage_fd, char **metadata)
{
    if (!stage_fd || !metadata) return -1;
    *stage_fd = -1;
    *metadata = NULL;
#ifdef __linux__
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    int type = 0;
    socklen_t type_size = sizeof(type);
    struct sockaddr_storage address;
    socklen_t address_size = sizeof(address);
    if (getsockopt(connection, SOL_SOCKET, SO_TYPE, &type, &type_size) < 0 || type != SOCK_SEQPACKET ||
        getpeername(connection, (struct sockaddr *)&address, &address_size) < 0 || address.ss_family != AF_UNIX ||
        getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) < 0 ||
        peer_size != sizeof(peer) || peer.pid <= 0 || peer.uid != expected_peer_uid) return -1;
    char packet[1024];
    union { struct cmsghdr aligned; char bytes[CMSG_SPACE(sizeof(int) * 16)]; } ancillary;
    struct iovec buffer = { packet, sizeof(packet) };
    struct msghdr message = {0};
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.bytes;
    message.msg_controllen = sizeof(ancillary.bytes);
    gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    ssize_t received;
    while (TRUE) {
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) return -1;
        struct pollfd waiting = {connection, POLLIN, 0};
        int ready = poll(&waiting, 1, (int)((remaining + 999) / 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(waiting.revents & POLLIN)) return -1;
        message.msg_controllen = sizeof(ancillary.bytes);
        message.msg_flags = 0;
        received = recvmsg(connection, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (received < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        break;
    }
    if (received < 0) return -1;
    int owned[16], count = 0;
    gboolean valid = received > 0 && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS || item->cmsg_len < CMSG_LEN(0)) {
            valid = FALSE; continue;
        }
        size_t bytes = item->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) valid = FALSE;
        for (size_t index = 0; index < bytes / sizeof(int); ++index) {
            int fd;
            memcpy(&fd, (char *)CMSG_DATA(item) + index * sizeof(int), sizeof(fd));
            if (count < 16) owned[count++] = fd;
            else { close(fd); valid = FALSE; }
        }
    }
    if (count != 1 || !valid || stage_packet(packet, (size_t)received, owned[0], metadata) < 0) {
        for (int index = 0; index < count; ++index) close(owned[index]);
        return -1;
    }
    *stage_fd = owned[0];
    return 0;
#else
    (void)connection; (void)expected_peer_uid;
    return -1;
#endif
}
