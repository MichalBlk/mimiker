#include <sys/klog.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>

void taskqueue_init(taskqueue_t *tq) {
  STAILQ_INIT(&tq->tq_list);
  mtx_init(&tq->tq_mutex, LK_RECURSIVE);
  token_init(&tq->tq_token, LK_TYPE_BLOCK, 0);
}

void taskqueue_destroy(taskqueue_t *tq) {
  assert(STAILQ_EMPTY(&tq->tq_list));
  mtx_destroy(&tq->tq_mutex);
  token_destroy(&tq->tq_token);
}

void taskqueue_add(taskqueue_t *tq, task_t *task) {
  SCOPED_MTX_LOCK(&tq->tq_mutex);
  STAILQ_INSERT_TAIL(&tq->tq_list, task, t_link);
  token_give_one(&tq->tq_token);
}

void taskqueue_run(taskqueue_t *tq) {
  int ntasks = token_take_all(&tq->tq_token);

  SCOPED_MTX_LOCK(&tq->tq_mutex);

  for (int i = 0; i < ntasks; i++) {
    task_t *task = STAILQ_FIRST(&tq->tq_list);
    task->t_func(task->t_arg);
    STAILQ_REMOVE_HEAD(&tq->tq_list, t_link);
  }
}
