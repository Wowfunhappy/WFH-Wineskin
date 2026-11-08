/*
 * __ulock_wait and __ulock_wake (10.12), process-private compare-and-wait on a 32-bit value: a waiter
 * sleeps while *addr holds the value it passed, until woken or until its timeout (microseconds, 0 for
 * none) expires. Waiters hash to buckets; a waiter checks the value under its bucket's lock and a waker
 * wakes the bucket under the same lock, so no wake is lost. Like the kernel's, a wait may return early
 * without a wake, and callers recheck the value.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>

#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL        0x00000100
#define ULF_NO_ERRNO        0x01000000
#define ULOCK_BUCKETS       64

static struct ulock_bucket
{
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} buckets[ULOCK_BUCKETS];
static pthread_once_t buckets_once = PTHREAD_ONCE_INIT;

static void init_buckets(void)
{
    int i;
    for (i = 0; i < ULOCK_BUCKETS; i++)
    {
        pthread_mutex_init(&buckets[i].mutex, NULL);
        pthread_cond_init(&buckets[i].cond, NULL);
    }
}

static struct ulock_bucket *bucket_for(void *addr)
{
    pthread_once(&buckets_once, init_buckets);
    return &buckets[((uintptr_t)addr >> 2) % ULOCK_BUCKETS];
}

static int ulock_error(uint32_t operation, int err)
{
    if (operation & ULF_NO_ERRNO) return -err;
    errno = err;
    return -1;
}

int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout)
{
    struct ulock_bucket *b;
    int ret = 0;

    if ((operation & 0xff) != UL_COMPARE_AND_WAIT || !addr) return ulock_error(operation, EINVAL);
    b = bucket_for(addr);
    pthread_mutex_lock(&b->mutex);
    if (__atomic_load_n((volatile uint32_t *)addr, __ATOMIC_SEQ_CST) == (uint32_t)value)
    {
        if (timeout)
        {
            struct timeval now;
            struct timespec deadline;
            gettimeofday(&now, NULL);
            deadline.tv_sec = now.tv_sec + timeout / 1000000;
            deadline.tv_nsec = (now.tv_usec + timeout % 1000000) * 1000;
            if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
            ret = pthread_cond_timedwait(&b->cond, &b->mutex, &deadline);
        }
        else ret = pthread_cond_wait(&b->cond, &b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
    if (ret == ETIMEDOUT) return ulock_error(operation, ETIMEDOUT);
    return 0;
}

int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value)
{
    struct ulock_bucket *b;

    if ((operation & 0xff) != UL_COMPARE_AND_WAIT || !addr) return ulock_error(operation, EINVAL);
    b = bucket_for(addr);
    /* The bucket may hold waiters on other addresses, so every waiter wakes and rechecks. */
    pthread_mutex_lock(&b->mutex);
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}
