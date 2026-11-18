/*
 * Drives talk_base::PhysicalSocketServer inside the real steamclient.so and
 * reproduces both FD_SETSIZE aborts on demand.
 *
 * Offsets match linux64/steamclient.so from the PZ 42.22.2 dedicated server
 * (md5 86c1277186c24c4e08ef32ec4eee6b42).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define PSS_CTOR 0x237f900
#define PSS_WAIT 0x237f1c0

enum { SLOT_CREATE_ASYNC_SOCKET = 3, SLOT_WAIT = 4, SLOT_WAKE_UP = 5 };

static void *pss;
static sem_t inside_socket, wait_finished;
static int hold_next_socket;

static void **vtable(void)
{
    return *(void ***)pss;
}

static void *create_async_socket(int type)
{
    return ((void *(*)(void *, int))vtable()[SLOT_CREATE_ASYNC_SOCKET])(pss, type);
}

static void wake_and_wait(void)
{
    ((void (*)(void *))vtable()[SLOT_WAKE_UP])(pss);
    ((int (*)(void *, int, int))vtable()[SLOT_WAIT])(pss, 100, 1);
}

/* Interposes libc so the race test can park CreateAsyncSocket inside socket(). */
int socket(int domain, int type, int protocol)
{
    static int (*real)(int, int, int);

    if (!real)
        real = dlsym(RTLD_NEXT, "socket");
    if (hold_next_socket) {
        hold_next_socket = 0;
        sem_post(&inside_socket);
        sem_wait(&wait_finished);
    }
    return real(domain, type, protocol);
}

static void *waiter(void *unused)
{
    (void)unused;
    sem_wait(&inside_socket);
    wake_and_wait();
    sem_post(&wait_finished);
    return NULL;
}

static void fill_fds_below(int limit)
{
    struct rlimit rl = { 4096, 4096 };
    int devnull = open("/dev/null", O_RDONLY);

    setrlimit(RLIMIT_NOFILE, &rl);
    while (dup(devnull) < limit - 1)
        ;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "high-fd") && strcmp(argv[2], "race"))) {
        fprintf(stderr, "usage: %s <steamclient.so> high-fd|race\n", argv[0]);
        return 2;
    }

    void *lib = dlopen(argv[1], RTLD_NOW);
    struct link_map *map;

    if (!lib || dlinfo(lib, RTLD_DI_LINKMAP, &map)) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }

    char *base = (char *)map->l_addr;

    pss = calloc(1, 0x200);
    ((void (*)(void *))(base + PSS_CTOR))(pss);
    if (vtable()[SLOT_WAIT] != base + PSS_WAIT) {
        fprintf(stderr, "steamclient.so build does not match the offsets\n");
        return 2;
    }

    if (!strcmp(argv[2], "high-fd")) {
        fill_fds_below(1024);
        create_async_socket(SOCK_DGRAM);
        wake_and_wait();
    } else {
        pthread_t thread;

        sem_init(&inside_socket, 0, 0);
        sem_init(&wait_finished, 0, 0);
        pthread_create(&thread, NULL, waiter, NULL);
        hold_next_socket = 1;
        create_async_socket(SOCK_DGRAM);
        pthread_join(thread, NULL);
        wake_and_wait();
    }

    puts("survived");
    return 0;
}
