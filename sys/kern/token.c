#include <sys/klog.h>
#include <sys/mutex.h>
#include <sys/pool.h>
#include <sys/spinlock.h>
#include <sys/token.h>

static POOL_DEFINE(P_SPIN, "spinlock", sizeof(spin_t));
static POOL_DEFINE(P_MTX, "mutex", sizeof(mtx_t));

void token_init(token_t *token, lk_attr_t la, int count) {
  assert((la & ~LK_TYPE_MASK) == 0);

  if (la == LK_TYPE_SPIN) {
    token->lock = (lock_t)(spin_t *)pool_alloc(P_SPIN, M_WAITOK);
    spin_init(token->lock.spin, 0);
  } else {
    token->lock = (lock_t)(mtx_t *)pool_alloc(P_MTX, M_WAITOK);
    mtx_init(token->lock.mtx, 0);
  }

  cv_init(&token->cv, "tokens available");
  token->nwaiters = 0;
  token->count = count;
  token->abort = false;
}

int token_take(token_t *token, int count) {
  int required = count ? count : 1;
  int rv = 0;

  lk_acquire(token->lock, __caller(0));
  assert(!token->abort);

  while (token->count < required) {
    token->nwaiters++;
    cv_wait(&token->cv, token->lock);
    token->nwaiters--;
    assert(!token->abort);
  }

  assert(token->count);

  if (count) {
    rv = count;
    token->count -= count;
  } else {
    rv = token->count;
    token->count = 0;
  }

  lk_release(token->lock);
  return rv;
}

int token_try_take(token_t *token, int count, int *countp) {
  int required = count ? count : 1;
  int value = 0;
  int rv = 0;

  lk_acquire(token->lock, __caller(0));

  if (token->abort || token->count < required) {
    rv = 1;
    goto end;
  }

  if (count) {
    value = count;
    token->count -= count;
  } else {
    value = token->count;
    token->count = 0;
  }

end:
  if (countp)
    *countp = value;

  lk_release(token->lock);
  return rv;
}

int token_take_timed(token_t *token, int count, systime_t timeout,
                     int *countp) {
  int required = count ? count : 1;
  int value = 0;
  int error = 0;

  lk_acquire(token->lock, __caller(0));

  while (!error && !token->abort && token->count < required) {
    token->nwaiters++;
    error = cv_wait_timed(&token->cv, token->lock, timeout);
    token->nwaiters--;
  }
  if (error || token->abort)
    goto end;

  assert(token->count);

  if (count) {
    value = count;
    token->count -= count;
  } else {
    value = token->count;
    token->count = 0;
  }

end:
  if (countp)
    *countp = value;

  lk_release(token->lock);
  return error;
}

void token_give(token_t *token, int count) {
  lk_acquire(token->lock, __caller(0));
  assert(!token->abort);

  token->count += count;
  if (token->count > 0 && token->nwaiters)
    cv_broadcast(&token->cv);

  lk_release(token->lock);
}

void token_abort(token_t *token) {
  lk_acquire(token->lock, __caller(0));
  assert(!token->abort);

  token->abort = true;
  if (token->nwaiters)
    cv_broadcast(&token->cv);

  lk_release(token->lock);
}

void token_destroy(token_t *token) {
  lock_t lock = token->lock;

  if (lk_spin_p(lock))
    spin_destroy(lock.spin);
  else
    mtx_destroy(lock.mtx);

  cv_destroy(&token->cv);
}
