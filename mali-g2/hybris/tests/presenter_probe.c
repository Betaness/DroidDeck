/*
 * A presenter that does what the Vortek server's zero_copy.c only does in rare cases, against the
 * compositor's vortek_present.c (tests/compositor_test.sh): announces a swapchain whose id the
 * compositor's chain keys cannot hold (it must be refused under that id), then connects again from the
 * same process (the old connection must be closed, as when a presenter reconnects). Prints
 * "PROBE-RESULT ok|fail".
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vortek_present.h"

static int connect_to(const char* path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect");
        exit(1);
    }
    return fd;
}

/* The next message within 2 s: 0, 1 when the connection was closed, -1 on a timeout. */
static int next(int fd, struct vp_msg* m) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 2000) <= 0) return -1;
    ssize_t r = recv(fd, m, sizeof(*m), MSG_WAITALL);
    return r == (ssize_t)sizeof(*m) ? 0 : r == 0 ? 1 : -1;
}

int main(void) {
    const char* path = getenv("VORTEK_PRESENT_SOCKET");
    if (!path) return 1;
    int ok = 1;
    int a = connect_to(path);
    const uint32_t big = 0x1000001;
    struct vp_msg m = {.type = VP_MSG_CHAIN_CREATE, .chain = big, .image_count = 0, .width = 64, .height = 64};
    send(a, &m, sizeof(m), MSG_NOSIGNAL);
    struct vp_msg r;
    int got = next(a, &r);
    while (got == 0 && r.type == VP_MSG_MODE) got = next(a, &r);
    if (got != 0 || r.type != VP_MSG_REFUSED || r.chain != big) {
        printf("PROBE: swapchain %#x not refused under its own id (got %d, type %u chain %#x)\n", big, got, r.type, r.chain);
        ok = 0;
    }
    int b = connect_to(path);
    do got = next(a, &r); while (got == 0);
    if (got != 1) {
        printf("PROBE: the first connection stayed open after the process connected again\n");
        ok = 0;
    }
    m = (struct vp_msg){.type = VP_MSG_HIDE, .chain = 5};
    send(b, &m, sizeof(m), MSG_NOSIGNAL);
    usleep(100 * 1000);
    close(a);
    close(b);
    printf("PROBE-RESULT %s\n", ok ? "ok" : "fail");
    return ok ? 0 : 1;
}
