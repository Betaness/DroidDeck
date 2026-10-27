/* [mali-g2] Transfers larger than a ring (vortek-0013, winlator-0021) between the two ring
 * implementations, as the connection uses them: the server's ring_buffer.c creates two 256 KB
 * rings in shared memory, the client's (a separate process) attaches to them. Messages of 8 bytes to 3 MB,
 * each a 4-byte size and its bytes, go from the server to the client (replies, such as
 * vkGetPipelineCacheData of an application's cache) and then from the client to the server
 * (requests; RingBuffer_write2 like the client's vt_send). Every byte is checked.
 * Prints RING-PASS or RING-FAIL.
 *
 * Build (from mali-g2/vortek, after fetch.sh):
 *   S=work/winlator-app/app/src/main/cpp/winlator
 *   cc -O2 -I$S/include -o out/ring_test_server tests/ring_test.c $S/src/ring_buffer.c -lpthread
 *   cc -O2 -DRING_CLIENT -Iwork/vortek/include -o out/ring_test_client tests/ring_test.c work/vortek/src/ring_buffer.c -lpthread
 *   out/ring_test_server out/ring_test_client */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ring_buffer.h"

#define RING_SIZE 262144

static const uint32_t sizes[] = {8, 1000, RING_SIZE / 2, RING_SIZE - 4, RING_SIZE, RING_SIZE + 1, 1u << 20, (3u << 20) + 13, 8};
#define COUNT (sizeof(sizes) / sizeof(sizes[0]))

static uint8_t pattern(uint32_t message, uint32_t i) {
    return (uint8_t)(i * 31u + message * 7u + (i >> 12));
}

static bool sendMessage(RingBuffer* ring, uint32_t message, uint8_t* buffer) {
    uint32_t size = sizes[message];
    for (uint32_t i = 0; i < size; i++) buffer[i] = pattern(message, i);
#ifdef RING_CLIENT
    return RingBuffer_write2(ring, &size, sizeof(size), buffer, size);
#else
    return RingBuffer_write(ring, &size, sizeof(size)) && RingBuffer_write(ring, buffer, size);
#endif
}

static bool receiveMessage(RingBuffer* ring, uint32_t message, uint8_t* buffer, const char* side) {
    uint32_t size = 0;
    if (!RingBuffer_read(ring, &size, sizeof(size)) || size != sizes[message]) {
        printf("%s: message %u: size %u, want %u\n", side, message, size, sizes[message]);
        return false;
    }
    if (!RingBuffer_read(ring, buffer, size)) {
        printf("%s: message %u: reading %u bytes failed\n", side, message, size);
        return false;
    }
    for (uint32_t i = 0; i < size; i++) {
        if (buffer[i] != pattern(message, i)) {
            printf("%s: message %u (%u bytes): byte %u is %u, want %u\n", side, message, size, i, buffer[i], pattern(message, i));
            return false;
        }
    }
    return true;
}

static bool exchange(RingBuffer* ring, bool sending, const char* side) {
    uint8_t* buffer = malloc((4u << 20));
    bool ok = buffer != NULL;
    for (uint32_t m = 0; ok && m < COUNT; m++) ok = sending ? sendMessage(ring, m, buffer) : receiveMessage(ring, m, buffer, side);
    free(buffer);
    return ok;
}

#ifndef RING_CLIENT
static RingBuffer* newRing(int* fd) {
    *fd = memfd_create("ring_test", 0);
    if (*fd < 0 || ftruncate(*fd, RingBuffer_getSHMemSize(RING_SIZE)) != 0) return NULL;
    return RingBuffer_create(*fd, RING_SIZE);
}
#endif

int main(int argc, char** argv) {
    alarm(120);
#ifdef RING_CLIENT
    if (argc != 3) return 2;
    RingBuffer* replies = RingBuffer_attach(atoi(argv[1]), RING_SIZE);
    RingBuffer* requests = RingBuffer_attach(atoi(argv[2]), RING_SIZE);
    if (!replies || !requests) return 3;
    bool ok = exchange(replies, false, "client (server to client)") && exchange(requests, true, "client");
    return ok ? 0 : 1;
#else
    if (argc != 2) {
        fprintf(stderr, "usage: %s <ring_test_client>\n", argv[0]);
        return 2;
    }
    int replyFd, requestFd;
    RingBuffer* replies = newRing(&replyFd);
    RingBuffer* requests = newRing(&requestFd);
    if (!replies || !requests) return 3;
    pid_t child = fork();
    if (child == 0) {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%d", replyFd);
        snprintf(b, sizeof(b), "%d", requestFd);
        execl(argv[1], argv[1], a, b, (char*)NULL);
        _exit(4);
    }
    bool ok = exchange(replies, true, "server") && exchange(requests, false, "server (client to server)");
    int status = 0;
    waitpid(child, &status, 0);
    ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    uint64_t total = 0;
    for (uint32_t m = 0; m < COUNT; m++) total += sizes[m];
    printf("%s: %zu messages each way (%llu bytes) through %u-byte rings, child status 0x%x\n", ok ? "RING-PASS" : "RING-FAIL", COUNT,
           (unsigned long long)total, RING_SIZE, status);
    return ok ? 0 : 1;
#endif
}
