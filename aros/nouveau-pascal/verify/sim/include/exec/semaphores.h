#pragma once
/* Simulator override: back AROS semaphores (recursive, like AROS) with pthreads */
#include <pthread.h>
#include <exec/lists.h>
struct SignalSemaphore { pthread_mutex_t m; };
