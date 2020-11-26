/*
 * The kernel resource manager.
 *
 * Based on FreeBSD `subr_rman.c` file.
 *
 * This code is responsible for keeping track
 * of hardware resources which are apportioned out to various drivers.
 * It does not actually assign those resources, and it is not expected
 * that end-device drivers will call into this code directly.  Rather,
 * the code which implements the buses that those devices are attached to,
 * and the code which manages CPU resources, will call this code, and the
 * end-device drivers will make upcalls to that code to actually perform
 * the allocation.
 */

#include <sys/mimiker.h>
#include <sys/rman.h>
#include <sys/malloc.h>

#define IS_RESERVED(r) ((r)->r_flags & RF_RESERVED)

static KMALLOC_DEFINE(M_RES, "resources & regions");

void rman_init(rman_t *rm, const char *name) {
  rm->rm_name = name;
  mtx_init(&rm->rm_lock, 0);
  TAILQ_INIT(&rm->rm_resources);
}

static resource_t *rman_alloc_resource(rman_t *rm, rman_addr_t start,
                                       rman_addr_t end, res_flags_t flags,
                                       kmem_flags_t kflags) {
  resource_t *r = kmalloc(M_RES, sizeof(resource_t), kflags | M_ZERO);
  r->r_rman = rm;
  r->r_start = start;
  r->r_end = end;
  r->r_flags = flags;
  return r;
}

void rman_manage_region(rman_t *rm, rman_addr_t start, size_t size) {
  assert((start + size - 1) >= start); /* check for overflow */

  resource_t *r = rman_alloc_resource(rm, start, start + size - 1, RF_NONE, 0);

  WITH_MTX_LOCK (&rm->rm_lock) {
    resource_t *cur;

    /* Skip entries before us. */
    TAILQ_FOREACH (cur, &rm->rm_resources, r_link) {
      /* Note that we need this aditional check
       * due to possible overflow in the following case. */
      if (cur->r_end == RMAN_ADDR_MAX)
        break;
      if (cur->r_end + 1 >= r->r_start)
        break;
    }

    /* If we ran off the end of the list, insert at the tail. */
    if (!cur) {
      TAILQ_INSERT_TAIL(&rm->rm_resources, r, r_link);
      return;
    }

    /* Check for any overlap with the current region. */
    if (r->r_start <= cur->r_end && r->r_end >= cur->r_start)
      panic("overlapping regions");

    resource_t *next = TAILQ_NEXT(cur, r_link);
    if (next) {
      /* Check for any overlap with the next region. */
      if (r->r_start <= next->r_end && r->r_end >= next->r_start)
        panic("overlapping regions");

      /* See if the new region can be merged with the next region.
       * If not, clear the pointer. */
      if (r->r_end + 1 != next->r_start || IS_RESERVED(next))
        next = NULL;
    }

    /* See if we can merge with the current region. */
    if (cur->r_end != RMAN_ADDR_MAX && /* watch out for overflow */
        cur->r_end + 1 == r->r_start && !IS_RESERVED(cur)) {
      /* Can we merge all 3 regions? */
      if (next) {
        cur->r_end = next->r_end;
        TAILQ_REMOVE(&rm->rm_resources, next, r_link);
        kfree(M_RES, r);
        kfree(M_RES, next);
      } else {
        cur->r_end = r->r_end;
        kfree(M_RES, r);
      }
    } else if (next) {
      /* We can merge with just the next region. */
      next->r_start = r->r_start;
      kfree(M_RES, r);
    } else if (cur->r_end < r->r_start) {
      TAILQ_INSERT_AFTER(&rm->rm_resources, cur, r, r_link);
    } else {
      TAILQ_INSERT_BEFORE(cur, r, r_link);
    }
  }
}

void rman_init_from_resource(rman_t *rm, const char *name, resource_t *r) {
  rman_init(rm, name);
  rman_manage_region(rm, r->r_start, rman_get_size(r));
}

void rman_fini(rman_t *rm) {
  WITH_MTX_LOCK (&rm->rm_lock) {
    while (!TAILQ_EMPTY(&rm->rm_resources)) {
      resource_t *r = TAILQ_FIRST(&rm->rm_resources);
      if (IS_RESERVED(r))
        panic("resource is busy");
      TAILQ_REMOVE(&rm->rm_resources, r, r_link);
      kfree(M_RES, r);
    }
  }
  /* TODO: destroy the `rm_lock` after implementing `mtx_destroy`. */
}

static resource_t *rman_split(resource_t *r, rman_addr_t start, rman_addr_t end,
                              res_flags_t flags) {
  rman_t *rm = r->r_rman;
  assert(mtx_owned(&rm->rm_lock));
  /*
   * If r->r_start < start and r->r_end > end,
   * then we need to split the region into three pieces
   * (the middle one will get returned to the user).
   * Otherwise, we are allocating at either the beginning
   * or the end of s, so we only need to split it in two.
   * The first case requires two allocations.
   * The second requires but one.
   */
  resource_t *rv = rman_alloc_resource(rm, start, end, flags, M_NOWAIT);

  if (r->r_start < start && r->r_end > end) {
    resource_t *gap =
      rman_alloc_resource(rm, end + 1, r->r_end, r->r_flags, M_NOWAIT);
    r->r_end = start - 1;
    TAILQ_INSERT_AFTER(&rm->rm_resources, r, rv, r_link);
    TAILQ_INSERT_AFTER(&rm->rm_resources, rv, gap, r_link);
  } else if (r->r_start == start) {
    r->r_start = end + 1;
    TAILQ_INSERT_BEFORE(r, rv, r_link);
  } else {
    r->r_end = start - 1;
    TAILQ_INSERT_AFTER(&rm->rm_resources, r, rv, r_link);
  }

  return rv;
}

resource_t *rman_reserve_resource(rman_t *rm, rman_addr_t start,
                                  rman_addr_t end, size_t count,
                                  size_t alignment, res_flags_t flags) {
  /* Watch out for overflow. */
  assert((start + count - 1) >= start);
  assert(count);

  alignment = max(alignment, 1UL);
  assert(powerof2(alignment));

  /* Discard sensless requests. */
  if (start + count - 1 > end)
    return NULL;

  flags = (flags | RF_RESERVED) & ~RF_ACTIVE;

  resource_t *rv = NULL;

  WITH_MTX_LOCK (&rm->rm_lock) {
    resource_t *r;

    TAILQ_FOREACH (r, &rm->rm_resources, r_link) {
      /* Skip lower regions. */
      if (r->r_end < start + count - 1)
        continue;

      /* Skip reserved regions. */
      if (IS_RESERVED(r))
        continue;

      /* Stop if we've gone too far. */
      if (r->r_start > end - count + 1)
        break;

      /* Stop if roundup causes overflow. */
      if (r->r_start > RMAN_ADDR_MAX - alignment + 1)
        break;

      rman_addr_t new_start = roundup(max(r->r_start, start), alignment);
      rman_addr_t new_end = new_start + count - 1;

      /* Check for overflow. */
      if ((new_start + count - 1) < new_start)
        break;

      /* See if it fits. */
      if (new_end > r->r_end)
        continue;

      /* Isn't it too far? */
      if (new_end > end)
        break;

      /* Can we use the whole region? */
      if (r->r_end - r->r_start + 1 == count) {
        rv = r;
        rv->r_flags = flags;
      } else {
        rv = rman_split(r, new_start, new_end, flags);
      }
      break;
    }
  }
  return rv;
}

void rman_activate_resource(resource_t *r) {
  WITH_MTX_LOCK (&r->r_rman->rm_lock) { r->r_flags |= RF_ACTIVE; }
}

void rman_deactivate_resource(resource_t *r) {
  WITH_MTX_LOCK (&r->r_rman->rm_lock) { r->r_flags &= ~RF_ACTIVE; }
}

void rman_release_resource(resource_t *r) {
  rman_t *rm = r->r_rman;

  WITH_MTX_LOCK (&rm->rm_lock) {
    if (r->r_flags & RF_ACTIVE)
      panic("releasing an active resource");
    /*
     * Look at the adjacent resources in the list and see if our
     * resource can be merged with any of them. If either of the
     * resources is reserved or is not exactly adjacent then they
     * cannot be merged with our resource.
     */
    resource_t *prev = TAILQ_PREV(r, res_list, r_link);
    if (prev && (IS_RESERVED(prev) || prev->r_end + 1 != r->r_start))
      prev = NULL;
    resource_t *next = TAILQ_NEXT(r, r_link);
    if (next && (IS_RESERVED(next) || r->r_end + 1 != next->r_start))
      next = NULL;

    if (prev && next) {
      /* Merge all three regions. */
      prev->r_end = next->r_end;
      TAILQ_REMOVE(&rm->rm_resources, next, r_link);
      kfree(M_RES, next);
    } else if (prev) {
      /* Merge previous region with ours. */
      prev->r_end = r->r_end;
    } else if (next) {
      /* Merge next region with ours. */
      next->r_start = r->r_start;
    } else {
      /*
       * At this point, we know there is nothing we can
       * potentially merge with, because on each side,
       * there is either nothing there or what is there
       * is still reserved. In that case, we don't want
       * to remove the resource from the list, we simply
       * want to change it to an unreserved region and
       * return without freeing anything.
       */
      r->r_flags &= ~RF_RESERVED;
      mtx_unlock(&rm->rm_lock);
      return;
    }
    TAILQ_REMOVE(&rm->rm_resources, r, r_link);
    kfree(M_RES, r);
  }
}
