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
 * We make one simplifying assumption: two clients
 * sharing the same resource must use the same range of indices. That
 * is to say, sharing of overlapping-but-not-identical resources is not
 * permitted.
 */

#include <sys/mimiker.h>
#include <sys/rman.h>
#include <sys/malloc.h>

#define RMAN_ALIGN(addr, amask) (((addr) + (amask)) & ~(amask))

typedef TAILQ_HEAD(, resource) res_list_t;

struct share_list {
  LIST_HEAD(, resource) sl_list; /* resources representing shared object */
  void *sl_virtual;              /* virtual address of the mapped object */
};

struct rman_region {
  rman_addr_t rg_start;             /* first physical address */
  rman_addr_t rg_end;               /* last physical address */
  res_list_t rg_resources;          /* all managed resources */
  TAILQ_ENTRY(rman_region) rg_link; /* link on resource manager list */
};

static KMALLOC_DEFINE(M_RES, "resources & regions");

void rman_init(rman_t *rm, const char *name) {
  rm->rm_name = name;
  mtx_init(&rm->rm_lock, 0);
  TAILQ_INIT(&rm->rm_regions);
}

void rman_manage_region(rman_t *rm, rman_addr_t start, rman_addr_t end) {
  rman_region_t *rg, *s, *t;

  rg = kmalloc(M_RES, sizeof(rman_region_t), M_ZERO);
  assert(rg);
  rg->rg_start = start;
  rg->rg_end = end;
  TAILQ_INIT(&rg->rg_resources);

  WITH_MTX_LOCK (&rm->rm_lock) {
    /* Skip regions before us. */
    TAILQ_FOREACH (s, &rm->rm_regions, rg_link) {
      if (s->rg_end == RMAN_ADDR_MAX)
        break;
      if (s->rg_end + 1 >= rg->rg_start)
        break;
    }

    /* If we ran off the end of the list, insert at the tail. */
    if (s == NULL) {
      TAILQ_INSERT_TAIL(&rm->rm_regions, rg, rg_link);
    } else {
      /* Check for any overlap with the current region. */
      if (rg->rg_start <= s->rg_end && rg->rg_end >= s->rg_start)
        panic("overlapping regions");

      /* Check for any overlap with the next region. */
      t = TAILQ_NEXT(s, rg_link);
      if (t) {
        if (rg->rg_start <= t->rg_end && rg->rg_end >= t->rg_start)
          panic("overlaping regions");

        /*
         * See if this region can be merged with the next region.
         * If not, clear the pointer.
         */
        if (rg->rg_end + 1 != t->rg_start)
          t = NULL;
      }

      /* See if we can merge with the current region. */
      if (rg->rg_start == s->rg_end + 1) {
        /* Can we merge all 3 regions? */
        if (t) {
          s->rg_end = t->rg_end;
          TAILQ_CONCAT(&s->rg_resources, &t->rg_resources, r_link);
          TAILQ_REMOVE(&rm->rm_regions, t, rg_link);
          kfree(M_RES, t);
          kfree(M_RES, rg);
        } else {
          s->rg_end = rg->rg_end;
          kfree(M_RES, rg);
        }
      } else if (t) {
        /* We can merge with just the next region. */
        t->rg_start = rg->rg_start;
        kfree(M_RES, rg);
      } else if (s->rg_end < rg->rg_start) {
        TAILQ_INSERT_AFTER(&rm->rm_regions, s, rg, rg_link);
      } else {
        TAILQ_INSERT_BEFORE(s, rg, rg_link);
      }
    }
  }
}

void rman_init_from_resource(rman_t *rm, const char *name, resource_t *r) {
  rman_init(rm, name);
  rman_manage_region(rm, r->r_start, r->r_end);
}

void rman_fini(rman_t *rm) {
  WITH_MTX_LOCK (&rm->rm_lock) {
    rman_region_t *rg;

    while (!TAILQ_EMPTY(&rm->rm_regions)) {
      rg = TAILQ_FIRST(&rm->rm_regions);
      TAILQ_REMOVE(&rm->rm_regions, rg, rg_link);
      if (!TAILQ_EMPTY(&rg->rg_resources))
        panic("non-empty region");
      kfree(M_RES, rg);
    }
  }
  /* TODO: destroy the `rm_lock` after implementing `mtx_desotry`. */
}

#define SHARE_TYPE(f) ((f) & (RF_SHAREABLE | RF_PREFETCHABLE))

static bool rman_region_reserve_resource(rman_region_t *rg, rman_addr_t start,
                                         rman_addr_t end, size_t count,
                                         resource_t *r) {
  resource_t *s, *t;
  rman_addr_t rstart, rend, amask;
  res_flags_t flags = r->r_flags;

  assert(mtx_owned(&r->r_rman->rm_lock));

  amask = (1ull << RF_ALIGNMENT(flags)) - 1;
  start = RMAN_ALIGN(max(rg->rg_start, start), amask);
  end = min(rg->rg_end, end);
  if (end - count + 1 < start)
    return false;

  /*
   * First try to find an acceptable totally-unshared region.
   */

  /* Does the resource fit before the first resource in the region? */
  s = TAILQ_FIRST(&rg->rg_resources);
  if (!s || (s && s->r_start - count >= start)) {
    r->r_rg = rg;
    r->r_start = start;
    r->r_end = start + count - 1;
    TAILQ_INSERT_HEAD(&rg->rg_resources, r, r_link);
    return true;
  }

  /* Look up first element after which we can place our new resoucre. */
  TAILQ_FOREACH_SAFE (s, &rg->rg_resources, r_link, t) {
    rman_addr_t gap_start = s->r_end + 1;
    rman_addr_t gap_end = t ? t->r_start - 1 : rg->rg_end;

    /* We surely can't fit the resource here (and `gap_start` overflowed). */
    if (s->r_end == RMAN_ADDR_MAX)
      break;
    /* Skip lower gaps. */
    if (gap_end - count + 1 < start)
      continue;
    /* Stop if we've gone too far. */
    if (gap_start > end - count + 1)
      break;
    /* Too large start address for required alignment. */
    if (gap_start > RMAN_ADDR_MAX - amask)
      break;

    rstart = RMAN_ALIGN(max(start, gap_start), amask);
    /* Check for overflow. */
    if ((rstart + count - 1) < rstart)
      break;
    rend = min(gap_end, max(rstart + count - 1, end));
    /* Check if the gap is large enough. */
    if (rstart <= rend && rend - rstart >= count - 1) {
      r->r_rg = rg;
      r->r_start = rstart;
      r->r_end = rstart + count - 1;
      TAILQ_INSERT_AFTER(&rg->rg_resources, s, r, r_link);
      return true;
    }
  }

  /*
   * Now find an acceptable shared region, if the client's requirements
   * allow sharing. By our implementation restriction, a candidate
   * region must match exactly by both size and sharing type in order
   * to be considered compatible with the client's request.
   */
  if (!(flags & RF_SHAREABLE))
    return false;

  TAILQ_FOREACH (s, &rg->rg_resources, r_link) {
    if (SHARE_TYPE(s->r_flags) == SHARE_TYPE(flags) && s->r_start >= start &&
        s->r_end - s->r_start == count - 1 && (s->r_start & amask) == 0) {
      r->r_rg = rg;
      assert(s->r_rg == rg);
      r->r_start = s->r_start;
      r->r_end = s->r_end;
      /* Check if share list is empty. */
      if (s->r_sharelist == NULL) {
        /* We need a new share list. */
        s->r_sharelist = kmalloc(M_RES, sizeof(share_list_t), M_ZERO);
        assert(s->r_sharelist);
        LIST_INIT(&s->r_sharelist->sl_list);
        LIST_INSERT_HEAD(&s->r_sharelist->sl_list, s, r_sharelink);
        s->r_flags |= RF_FIRSTSHARE;
      }
      r->r_sharelist = s->r_sharelist;
      LIST_INSERT_HEAD(&s->r_sharelist->sl_list, r, r_sharelink);
      return true;
    }
  }

  /* We couldn't find anything. */
  return false;
}

resource_t *rman_reserve_resource(rman_t *rm, rman_addr_t start,
                                  rman_addr_t end, size_t count,
                                  res_flags_t flags) {
  rman_addr_t amask;
  amask = (1ull << RF_ALIGNMENT(flags)) - 1;
  assert(start <= RMAN_ADDR_MAX - amask); /* alignment causes overflow */
  assert(!(flags & RF_FIRSTSHARE));
  assert(count);
  assert((start + count - 1) >= start); /* overflow */
  assert(start + count - 1 <= end);

  resource_t *r = kmalloc(M_RES, sizeof(resource_t), M_ZERO);
  assert(r);
  r->r_rman = rm;
  r->r_flags = flags & ~RF_ACTIVE;

  WITH_MTX_LOCK (&rm->rm_lock) {
    rman_region_t *rg;
    bool reserved = false;

    TAILQ_FOREACH (rg, &rm->rm_regions, rg_link) {
      /* Skip lower regions. */
      if (rg->rg_end - count + 1 < start)
        continue;
      /* Stop if we've gone too far. */
      if (rg->rg_start > end - count + 1)
        break;
      /* An inner alignment logic will cause an
       * overflow if the start of a region is too large. */
      if (rg->rg_start > RMAN_ADDR_MAX - amask)
        break;
      /* Try to place the resource in the current region. */
      if ((reserved = rman_region_reserve_resource(rg, start, end, count, r)))
        break;
    }
    if (reserved) {
      assert(r->r_start >= start);
      assert(r->r_end <= end);
      assert(r->r_end - r->r_start == count - 1);
      assert(r->r_rg);
    } else {
      kfree(M_RES, r);
      r = NULL;
    }
  }

  return r;
}

void rman_activate_resource(resource_t *r) {
  rman_t *rm = r->r_rman;
  WITH_MTX_LOCK (&rm->rm_lock) { r->r_flags |= RF_ACTIVE; }
}

void rman_deactivate_resource(resource_t *r) {
  rman_t *rm = r->r_rman;
  WITH_MTX_LOCK (&rm->rm_lock) { r->r_flags &= ~RF_ACTIVE; }
}

void rman_release_resource(resource_t *r) {
  rman_t *rm = r->r_rman;
  rman_region_t *rg = r->r_rg;

  WITH_MTX_LOCK (&rm->rm_lock) {
    if (r->r_flags & RF_ACTIVE)
      r->r_flags &= ~RF_ACTIVE;

    /* Check for a share list first. */
    if (r->r_sharelist) {
      resource_t *s;
      /*
       * If a sharing list exists, then we know there are at
       * least two sharers.
       */
      LIST_REMOVE(r, r_sharelink);
      s = LIST_FIRST(&r->r_sharelist->sl_list);
      if (r->r_flags & RF_FIRSTSHARE) {
        /* Transfer ownership of the list. */
        s->r_flags |= RF_FIRSTSHARE;
        TAILQ_INSERT_BEFORE(r, s, r_link);
        TAILQ_REMOVE(&rg->rg_resources, r, r_link);
      }
      /*
       * Make sure that the sharing list goes away completely
       * if the resource is no longer being shared at all.
       */
      if (LIST_NEXT(s, r_sharelink) == NULL) {
        assert(s->r_flags & RF_FIRSTSHARE);
        kfree(M_RES, s->r_sharelist);
        s->r_sharelist = NULL;
        s->r_flags &= ~RF_FIRSTSHARE;
      }
    } else {
      TAILQ_REMOVE(&rg->rg_resources, r, r_link);
    }
    kfree(M_RES, r);
  }
}

res_flags_t rman_make_alignment_flags(uint32_t size) {
  uint32_t cl = size;
  /*
   * Find the highest bit set, and add one if more than one bit is set.
   * We're effectively computing the ceil(log2(size)) here.
   */
  for (int i = 1; i <= 16; i <<= 1)
    cl |= cl >> i;
  cl &= ~(cl >> 1);
  if (~cl & size)
    cl++;
  /* The following will ensure alignment to 2^ceil(log2(size)). */
  return (res_flags_t)RF_ALIGNMENT_LOG2(cl);
}

void *rman_get_virtual(resource_t *r) {
  void *vaddr = NULL;
  rman_t *rm = r->r_rman;

  WITH_MTX_LOCK (&rm->rm_lock) {
    if (r->r_sharelist)
      vaddr = r->r_sharelist->sl_virtual;
  }
  return vaddr;
}

void rman_set_virtual(resource_t *r, void *vaddr) {
  rman_t *rm = r->r_rman;

  WITH_MTX_LOCK (&rm->rm_lock) {
    if (r->r_sharelist)
      r->r_sharelist->sl_virtual = vaddr;
  }
}
