/*
 * VirtIO keyboard driver.
 */
#define KL_LOG KL_DEV
#include <sys/bus.h>
#include <sys/devclass.h>
#include <sys/input-event-codes.h>
#include <sys/klog.h>
#include <sys/kmem.h>
#include <sys/pmap.h>
#include <sys/sched.h>
#include <sys/sleepq.h>
#include <dev/evdev.h>
#include <dev/virtio.h>
#include <dev/virtio_input.h>

typedef struct viokbd_state {
  virtqueue_t *ev_vq;
  virtio_input_event_t *ev;
  bool *ev_v;
  evdev_dev_t *evdev;
  thread_t *thread;
  paddr_t ev_pa;
  uint16_t ev_vcnt;
} viokbd_state_t;

/*
 * Virtqueue interrupt handling.
 */

static inline int viokbd_desc2idx(viokbd_state_t *viokbd, vring_desc_t *d) {
  return (d->addr - viokbd->ev_pa) / sizeof(virtio_input_event_t);
}

static void viokbd_intr(void *state, vring_desc_t *d, size_t len) {
  device_t *dev = state;
  viokbd_state_t *viokbd = dev->state;

  if (len != sizeof(virtio_input_event_t))
    return;

  int idx = viokbd_desc2idx(viokbd, d);
  viokbd->ev_v[idx] = true;
  viokbd->ev_vcnt++;

  sleepq_signal(&viokbd->ev_vcnt);
}

/*
 * VirtIO keyboard evdev thread.
 */

static void viokbd_thread(void *arg) {
  device_t *dev = arg;
  viokbd_state_t *viokbd = dev->state;

  int num = viokbd->ev_vq->num;
  bool *v = kmalloc(M_DEV, num, 0);
  assert(v);

  for (;;) {
    WITH_INTR_DISABLED {
      while (!viokbd->ev_vcnt)
        sleepq_wait(&viokbd->ev_vcnt, NULL, NULL);

      memcpy(v, viokbd->ev_v, num);
      memset(viokbd->ev_v, 0, num);
      viokbd->ev_vcnt = 0;
    }

    for (int i = 0; i < num; i++) {
      if (!v[i])
        continue;

      virtio_input_event_t *ev = &viokbd->ev[i];
      evdev_push_event(viokbd->evdev, ev->type, ev->code, ev->value);
      evdev_sync(viokbd->evdev);

      vring_desc_t *d = &viokbd->ev_vq->desc[i];
      virtio_vq_schedule(viokbd->ev_vq, d);
    }

    virtio_vq_kick(dev, viokbd->ev_vq->id);
  }
}

/*
 * Driver interface.
 */

static int viokbd_probe(device_t *dev) {
  virtio_device_t *viodev = virtio_device_of(dev);
  assert(viodev);
  /* XXX: for now, we assume that the keyboard
   * is the only VirtIO input device connected to the system. */
  return viodev->device_id == VIRTIO_DEVICE_ID_INPUT;
}

static int viokbd_attach(device_t *dev) {
  viokbd_state_t *viokbd = dev->state;
  int err = 0;

  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_DRIVER);

  if ((err = virtio_select_features(dev, VIRTIO_F_VERSION_1)))
    goto bad;

  if ((err = virtio_vq_init(dev, 0, "eventq", &viokbd->ev_vq)))
    goto bad;

  /* Initialize receiving. */
  int num = viokbd->ev_vq->num;
  size_t one_ev_sz = sizeof(virtio_input_event_t);
  size_t ev_sz = num * one_ev_sz;
  size_t ev_rsz = max((size_t)1 << (log2(ev_sz) + 1), PAGESIZE);

  viokbd->ev = (void *)kmem_alloc_contig(&viokbd->ev_pa, ev_rsz, PMAP_NOCACHE);
  assert(viokbd->ev);

  viokbd->ev_v = kmalloc(M_DEV, viokbd->ev_vq->num, M_ZERO);
  assert(viokbd->ev_v);
  viokbd->ev_vcnt = 0;

  vring_desc_t *d = viokbd->ev_vq->desc;
  paddr_t pa = viokbd->ev_pa;
  for (int i = 0; i < num; i++, d++, pa += one_ev_sz) {
    d->addr = pa;
    d->len = one_ev_sz;
    d->flags = VRING_DESC_F_WRITE;

    virtio_vq_schedule(viokbd->ev_vq, d);
  }

  virtio_vq_setup_intr(dev, viokbd->ev_vq, viokbd_intr, dev);

  /* Init evdev. */
  virtio_device_t *viodev = virtio_device_of(dev);
  viokbd->evdev = evdev_alloc();
  evdev_set_name(viokbd->evdev, "VirtIO keyboard");
  evdev_set_id(viokbd->evdev, BUS_VIRTUAL, viodev->vendor_id, viodev->device_id, 2);

  evdev_support_event(viokbd->evdev, EV_SYN);
  evdev_support_event(viokbd->evdev, EV_KEY);
  evdev_support_all_hidkbd_keys(viokbd->evdev);

  evdev_register(viokbd->evdev);

  /* Create a thead for handling evdev events. */
  viokbd->thread = thread_create("VirtIO keyboard", viokbd_thread, dev,
                                 prio_ithread(PRIO_ITHRD_QTY - 1));
  sched_add(viokbd->thread);

  /* At this point the device is ready to go. */
  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK);
  virtio_vq_kick(dev, viokbd->ev_vq->id);

  return 0;

bad:
  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_FAILED);
  return err;
}

static driver_t viokbd_driver = {
  .desc = "VirtIO keyboard",
  .size = sizeof(viokbd_state_t),
  .pass = SECOND_PASS,
  .probe = viokbd_probe,
  .attach = viokbd_attach,
};

DEVCLASS_ENTRY(virtio, viokbd_driver);
