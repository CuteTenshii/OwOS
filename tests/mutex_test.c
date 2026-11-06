/* Host test for the futex-backed mutex (user/lib/mutex.c) over a pthread futex. */
#define OWOS_SUITE_TEST
#include "mutex.h"
#include "test.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>

static pthread_mutex_t fx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fxc = PTHREAD_COND_INITIALIZER;

static unsigned now_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

bool futex_wait(unsigned *addr, unsigned expected, unsigned ms)
{
  bool timed_out = false;
  pthread_mutex_lock(&fx);
  if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) == expected) {
    if (ms) {
      struct timespec t;
      clock_gettime(CLOCK_REALTIME, &t);
      t.tv_sec += ms / 1000;
      t.tv_nsec += (long)(ms % 1000) * 1000000;
      if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
      }
      timed_out = pthread_cond_timedwait(&fxc, &fx, &t) != 0;
    } else {
      pthread_cond_wait(&fxc, &fx);
    }
  }
  pthread_mutex_unlock(&fx);
  return timed_out;
}

int futex_wake(unsigned *addr, int n)
{
  (void)addr;
  (void)n;
  pthread_mutex_lock(&fx);
  pthread_cond_broadcast(&fxc);
  pthread_mutex_unlock(&fx);
  return 0;
}

#define THREADS 8
#define ITERS 20000

static mutex_t m = MUTEX_INIT;
static long counter;

static void *worker(void *arg)
{
  (void)arg;
  for (int i = 0; i < ITERS; i++) {
    mutex_lock(&m);
    long v = counter;
    counter = v + 1;
    mutex_unlock(&m);
  }
  return 0;
}

int main(void)
{
  test("lock and unlock exclude each other");
  pthread_t th[THREADS];
  for (int i = 0; i < THREADS; i++)
    pthread_create(&th[i], 0, worker, 0);
  for (int i = 0; i < THREADS; i++)
    pthread_join(th[i], 0);
  expect(counter == (long)THREADS * ITERS);
  expect(m.state == 0);

  test("trylock fails while held and succeeds when free");
  expect(mutex_trylock(&m));
  expect(!mutex_trylock(&m));
  mutex_unlock(&m);
  expect(mutex_trylock(&m));
  mutex_unlock(&m);

  test("timedlock times out on a held mutex and succeeds on a free one");
  expect(mutex_timedlock(&m, 10));
  unsigned t0 = now_ms();
  expect(!mutex_timedlock(&m, 30));
  expect(now_ms() - t0 >= 5);
  mutex_unlock(&m);
  expect(mutex_timedlock(&m, 10));
  mutex_unlock(&m);

  test("reset releases a stuck mutex");
  mutex_lock(&m);
  mutex_reset(&m);
  expect(mutex_trylock(&m));
  mutex_unlock(&m);
  return 0;
}
