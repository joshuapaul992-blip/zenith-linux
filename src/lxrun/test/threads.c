/*
 * threads.c -- pthreads test for lxrun Step 2 (clone + futex + TLS).
 *
 * Spawns 4 threads that each bump a shared counter under a mutex, joins them,
 * and checks the total. Exercises clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS),
 * futex (mutex + join), per-thread TLS, and thread exit / CHILD_CLEARTID.
 *
 * Build against musl (dynamic) and place in the rootfs, e.g. on Alpine:
 *     gcc -O2 -o /threads threads.c
 * or with a musl cross toolchain:
 *     musl-gcc -O2 -o threads threads.c
 * Run:
 *     lxrun -v --root C:\rootfs C:\rootfs\threads
 * Expect:
 *     counter = 400000 (expected 400000)
 * and exit code 0.
 */

#include <pthread.h>
#include <stdio.h>

static long counter = 0;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&m);
        counter++;
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

int main(void)
{
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    printf("counter = %ld (expected 400000)\n", counter);
    return counter == 400000 ? 0 : 1;
}
