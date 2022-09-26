#ifndef _SYS_TOKEN_H_
#define _SYS_TOKEN_H_

#include <sys/lock.h>
#include <sys/condvar.h>

typedef struct {
  lock_t lock;
  condvar_t cv;
  unsigned nwaiters;
  int count;
  bool abort;
} token_t;

void token_init(token_t *token, lk_attr_t la, int count);

int token_take(token_t *token, int count);

#define token_take_one(t) ((void)token_take((t), 1))
#define token_take_all(t) token_take((t), 0)

int token_try_take(token_t *token, int count, int *countp);

#define token_try_take_one(t) token_try_take((t), 1, NULL)
#define token_try_take_all(t, cp) token_try_take((t), 1, (cp))

int token_take_timed(token_t *token, int count, systime_t timeout, int *countp);

#define token_take_one_timed(t, to) token_take_timed((t), 1, (to), NULL)
#define token_take_all_timed(t, to, cp) token_take_timed((t), 0, (to), (cp))

#define token_take_intr(t, c, cp) token_take_timed((t), (c), 0, (cp))

#define token_take_one_intr(t) token_take_intr((t), 1, NULL)
#define token_take_all_intr(t, cp) token_take_intr((t), 0, (cp))

void token_give(token_t *token, int count);

#define token_give_one(t) token_give(t, 1)

void token_abort(token_t *token);

void token_destroy(token_t *token);

#endif /* !_SYS_TOKEN_H_ */
