/*
 * VirtIO bus driver.
 */
#define KL_LOG KL_DEV
#include <sys/bus.h>
#include <sys/devclass.h>
#include <sys/errno.h>
#include <sys/fdt.h>
#include <sys/klog.h>
#include <sys/kmem.h>
#include <sys/malloc.h>
#include <sys/pmap.h>
#include <dev/simplebus.h>
#include <dev/virtio.h>
#include <dev/virtio_mmio.h>

typedef struct vio_state {
} vio_state_t;

#define in(regs, addr) bus_read_4(regs, (addr))
#define out(regs, addr, val) bus_write_4(regs, (addr), (val))

static const char *device_id_info[] = {
  [VIRTIO_DEVICE_ID_NETWORK] = "network",
  [VIRTIO_DEVICE_ID_BLOCK] = "block",
  [VIRTIO_DEVICE_ID_CONSOLE] = "console",
  [VIRTIO_DEVICE_ID_ENTROPY] = "entropy",
  [VIRTIO_DEVICE_ID_BALLOON] = "balloon",
  [VIRTIO_DEVICE_ID_IOMEM] = "IO memory",
  [VIRTIO_DEVICE_ID_RPMSG] = "rpmsg",
  [VIRTIO_DEVICE_ID_SCSI] = "SCSI",
  [VIRTIO_DEVICE_ID_9P] = "9P",
  [VIRTIO_DEVICE_ID_GPU] = "GPU",
  [VIRTIO_DEVICE_ID_INPUT] = "input",
};

DEVCLASS_CREATE(virtio);

/*
 * VirtIO device handling functions.
 */

static void vio_dev_print_ids(virtio_device_t *viodev) {
  const char *devids = NULL;
  if (viodev->device_id <= VIRTIO_DEVICE_ID_MAX)
    devids = device_id_info[viodev->device_id];

  const char *venids = NULL;
  if (viodev->vendor_id == VIRTIO_VENDOR_ID_QEMU)
    venids = "QEMU";

  klog("VirtIO device (%p):", viodev);
  if (devids)
    klog("\tDevice ID: %s", devids);
  else
    klog("\tDevice ID: %#x", viodev->device_id);

  if (venids)
    klog("\tVendor ID: %s", venids);
  else
    klog("\tVendor ID: %#x", viodev->vendor_id);
}

static void vio_dev_alloc(device_t *dev, uint32_t device_id, uint32_t vendor_id, resource_t *regs) {
  virtio_device_t *viodev = kmalloc(M_DEV, sizeof(virtio_device_t), M_ZERO);
  assert(viodev);
  LIST_INIT(&viodev->vqs);
  viodev->regs = regs;
  viodev->device_id = device_id;
  viodev->vendor_id = vendor_id;
  dev->instance = viodev;
}

/*
 * VirtIO interface functions.
 *
 * TODO: the following is an implementation of the VirtIO interface for
 * the MMIO transport option. This code should be moved to a separate module.
 * Furthermore, we should also provide an implementation for the PCI
 * transport option.
 */

static uint64_t vio_read_features(device_t *dev) {
  virtio_device_t *viodev = virtio_device_of(dev);

  out(viodev->regs, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
  uint64_t features = in(viodev->regs, VIRTIO_MMIO_DEVICE_FEATURES);

  out(viodev->regs, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
  return features | ((uint64_t)in(viodev->regs, VIRTIO_MMIO_DEVICE_FEATURES) << 32);
}

static void vio_write_features(device_t *dev, uint64_t val) {
  virtio_device_t *viodev = virtio_device_of(dev);

  out(viodev->regs, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
  out(viodev->regs, VIRTIO_MMIO_DRIVER_FEATURES, (int32_t)val);

  out(viodev->regs, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
  out(viodev->regs, VIRTIO_MMIO_DRIVER_FEATURES, (int32_t)(val >> 32));

  viodev->features = val;
}

static uint32_t vio_read_status(device_t *dev) {
  virtio_device_t *viodev = virtio_device_of(dev);
  return in(viodev->regs, VIRTIO_MMIO_STATUS);
}

static void vio_set_status(device_t *dev, uint32_t val) {
  virtio_device_t *viodev = virtio_device_of(dev);
  uint32_t sts = in(viodev->regs, VIRTIO_MMIO_STATUS);
  out(viodev->regs, VIRTIO_MMIO_STATUS, sts | val);
}

static void vio_ack_intr(device_t *dev) {
  virtio_device_t *viodev = virtio_device_of(dev);
  out(viodev->regs, VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_CONFIG_ISR_QUEUE_INTERRUPT);
}

static uint16_t vio_vq_num(device_t *dev, uint32_t id) {
  virtio_device_t *viodev = virtio_device_of(dev);

  out(viodev->regs, VIRTIO_MMIO_QUEUE_SEL, id);
  uint16_t max_num = in(viodev->regs, VIRTIO_MMIO_QUEUE_NUM_MAX);

  /* Let's select the biggest power of 2 that is not bigger than max_num. */
  uint16_t num = 1 << (31 - __builtin_clz(max_num));
  out(viodev->regs, VIRTIO_MMIO_QUEUE_NUM, num);

  return num;
}

static void vio_vq_setup(device_t *dev, uint32_t id, uint64_t desc_pa, uint64_t avail_pa, uint64_t used_pa) {
  virtio_device_t *viodev = virtio_device_of(dev);

  out(viodev->regs, VIRTIO_MMIO_QUEUE_SEL, id);

  out(viodev->regs, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_pa);
  out(viodev->regs, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_pa >> 32));

  out(viodev->regs, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_pa);
  out(viodev->regs, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_pa >> 32));

  out(viodev->regs, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used_pa);
  out(viodev->regs, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_pa >> 32));
}

static void vio_vq_enable(device_t *dev, uint32_t id) {
  virtio_device_t *viodev = virtio_device_of(dev);
  out(viodev->regs, VIRTIO_MMIO_QUEUE_SEL, id);
  out(viodev->regs, VIRTIO_MMIO_QUEUE_READY, 1);
  assert(in(viodev->regs, VIRTIO_MMIO_QUEUE_READY) == 1);
}

static void vio_vq_kick(device_t *dev, uint32_t id) {
  virtio_device_t *viodev = virtio_device_of(dev);
  out(viodev->regs, VIRTIO_MMIO_QUEUE_NOTIFY, id);
}

/*
 * VirtIO standard functions.
 */

int virtio_select_features(device_t *dev, uint64_t val) {
  assert(val & VIRTIO_F_VERSION_1);

  uint64_t features = virtio_read_features(dev);

  if (!(features & VIRTIO_F_VERSION_1)) {
    klog("VirtIO legacy device detected");
    return ENXIO;
  }

  virtio_write_features(dev, features & val);
  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK);

  if (!(virtio_read_status(dev) & VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK)) {
    klog("VirtIO feature selection has failed");
    return ENXIO;
  }

  return 0;
}

/*
 * Virtqueue handling functions.
 */

static inline size_t vio_roundup(size_t sz) {
  size_t val = powerof2(sz) ? sz : (size_t)1 << (log2(sz) + 1);
  return max(val, (size_t)PAGESIZE);
}

static inline uint16_t vio_mod(virtqueue_t *vq, uint16_t idx) {
  return idx & (vq->num - 1);
}

static inline uint16_t vio_nxt(virtqueue_t *vq, uint16_t idx) {
  return idx == vq->num - 1 ? 0 : idx + 1;
}

static inline uint16_t vio_desc2id(virtqueue_t *vq, vring_desc_t *d) {
  return d - vq->desc;
}

static virtqueue_t *vio_vq_alloc(void) {
  virtqueue_t *vq = kmalloc(M_DEV, sizeof(virtqueue_t), M_ZERO);
  assert(vq);
  mtx_init(&vq->lock, MTX_SPIN);
  return vq;
}

static ih_filter_t vio_intr;

int virtio_vq_init(device_t *dev, uint32_t id, const char *name, virtqueue_t **vqp) {
  vring_desc_t *desc = NULL;
  vring_avail_t *avail = NULL;
  vring_used_t *used = NULL;
  uint16_t num = virtio_vq_num(dev, id);

  /* Allocate memory for the descriptor vring. */
  size_t desc_sz = vio_roundup(num * sizeof(vring_desc_t));
  paddr_t desc_pa;
  if (!(desc = (void *)kmem_alloc_contig(&desc_pa, desc_sz, PMAP_NOCACHE))) {
    klog("VirtIO failed to allocate memory for descriptor vring");
    goto bad;
  }

  /* Allocate memory for the available vring. */
  size_t avail_sz = vio_roundup(sizeof(vring_avail_t) + num * sizeof(uint16_t));
  paddr_t avail_pa;
  if (!(avail = (void *)kmem_alloc_contig(&avail_pa, avail_sz, PMAP_NOCACHE))) {
    klog("VirtIO failed to allocate memory for available vring");
    goto bad;
  }

  /* Allocate memory for the used vring. */
  size_t used_sz = vio_roundup(sizeof(vring_used_t) + num * sizeof(vring_used_elem_t));
  paddr_t used_pa;
  if (!(used = (void *)kmem_alloc_contig(&used_pa, used_sz, PMAP_NOCACHE))) {
    klog("VirtIO failed to allocate memory for used vring");
    goto bad;
  }

  virtio_vq_setup(dev, id, desc_pa, avail_pa, used_pa);

  virtqueue_t *vq = vio_vq_alloc();
  vq->desc = desc;
  vq->avail = avail;
  vq->used = used;
  vq->name = name;
  vq->id = id;
  vq->num = num;

  virtio_device_t *viodev = virtio_device_of(dev);
  LIST_INSERT_HEAD(&viodev->vqs, vq, link);
  viodev->nvqs++;

  virtio_vq_enable(dev, id);

  /* Lastly, if it is the first virtqueu that we're creating for this device,
   * we have to prepare an interrupt resource and setup the interrupt
   * handler of the VirtIO bus. */
  if (!viodev->irq) {
    viodev->irq = device_take_irq(dev, 0);
    assert(viodev->irq);

    pic_setup_intr(dev, viodev->irq, vio_intr, NULL, dev, "VirtIO interrupt");
  }

  if (vqp)
    *vqp = vq;
  return 0;

bad:
  if (desc)
    kmem_free(desc, desc_sz);
  if (avail)
    kmem_free(avail, avail_sz);
  if (used)
    kmem_free(used, used_sz);
  return ENXIO;
}

void virtio_vq_setup_intr(device_t *dev, virtqueue_t *vq, vq_isr_t isr, void *arg) {
  virtio_device_t *viodev = virtio_device_of(dev);
  assert(viodev->irq);

  vq->isr = isr;
  vq->arg = arg;
}

/* XXX: for now, the scheduling functions just assume that there is enough spece
 * in the available vring. */
static void vio_vq_schedule_nolock(virtqueue_t *vq, vring_desc_t *d) {
  assert(mtx_owned(&vq->lock));

  vring_avail_t *avail = vq->avail;
  uint16_t i = vio_mod(vq, avail->idx++);
  avail->ring[i] = vio_desc2id(vq, d);
}

void virtio_vq_schedule(virtqueue_t *vq, vring_desc_t *d) {
  SCOPED_MTX_LOCK(&vq->lock);
  vio_vq_schedule_nolock(vq, d);
}

void virtio_vq_schedule_from_isr(virtqueue_t *vq, vring_desc_t *d) {
  vio_vq_schedule_nolock(vq, d);
}

void virtio_vq_schedule_blocking(device_t *dev, virtqueue_t *vq, vring_desc_t *d) {
  SCOPED_MTX_LOCK(&vq->lock);

  uint16_t last_uidx = vq->last_uidx;
  vio_vq_schedule_nolock(vq, d);
  virtio_vq_kick(dev, vq->id);

  volatile uint16_t *lidx = &vq->used->idx;
  while (*lidx == last_uidx)
    continue;
}

/*
 * Interrupt handling.
 */

static intr_filter_t vio_intr(void *data) {
  device_t *dev = data;
  virtio_device_t *viodev = virtio_device_of(dev);
  intr_filter_t rv = IF_STRAY;

  virtio_ack_intr(dev);

  virtqueue_t *vq;
  LIST_FOREACH (vq, &viodev->vqs, link) {
    SCOPED_MTX_LOCK(&vq->lock);

    uint16_t new_uidx = vq->used->idx;
    if (vq->last_uidx == new_uidx)
      continue;

    for (uint16_t i = vq->last_uidx, j = vio_mod(vq, i); i != new_uidx; i++, j = vio_nxt(vq, j)) {
      vring_used_elem_t *u = &vq->used->ring[j];
      vring_desc_t *d = &vq->desc[u->id];
      vring_desc_t *rsp_d = NULL;

      for (;;) {
        if ((d->flags & VRING_DESC_F_WRITE) && !rsp_d)
          rsp_d = d;
        if (!(d->flags & VRING_DESC_F_NEXT))
          break;
        d = &vq->desc[d->next];
      }

      if (vq->isr)
        vq->isr(vq->arg, rsp_d, u->len);
    }

    vq->last_uidx = new_uidx;
    rv = IF_FILTERED;
  }

  return rv;
}

/*
 * VirtIO bus initialization.
 */

void virtio_init(device_t *rdev, int unit, device_t *pic) {
  /* We don't need a VirtIO bus when there are no VirtIO devices. */
  if (FDT_finddevice("/soc/virtio_mmio") == FDT_NODEV)
    return;
  device_t *bus = device_add_child(rdev, unit);
  bus->pic = pic;
  bus->devclass = &DEVCLASS(virtio);
}

static int vio_probe(device_t *bus) {
  return bus->devclass == &DEVCLASS(virtio);
}

static int vio_attach(device_t *bus) {
  phandle_t snode = FDT_finddevice("/soc");
  assert(snode);

  int unit = 0;
  for (phandle_t node = FDT_child(snode); node != FDT_NODEV;
       node = FDT_peer(node)) {
    if (!FDT_is_compatible(node, "virtio,mmio"))
      continue;

    device_t *dev;
    int err;
    if ((err = simplebus_add_child_node(bus, node, unit, bus->pic, &dev))) {
      klog("VirtIO failed to add child with error %d", err);
      goto skip;
    }
    dev->bus = DEV_BUS_VIRTIO;

    resource_t *regs = device_take_memory(dev, 0);
    assert(regs);
    if ((err = bus_map_resource(dev, regs))) {
      klog("VirtIO failed to map MMIO registers with error %d", err);
      goto skip_rm;
    }

    if (in(regs, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) {
      klog("VirtIO device with invalid magic value");
      goto skip_rm;
    }
    if (in(regs, VIRTIO_MMIO_VERSION) == 1) {
      klog("VirtIO encountered a lagacy device");
      goto skip_rm;
    }

    uint32_t device_id, vendor_id;
    if (!(device_id = in(regs, VIRTIO_MMIO_DEVICE_ID))) {
      klog("VirtIO detected a stub device (i.e., no device connected)");
      goto skip_rm;
    }
    vendor_id = in(regs, VIRTIO_MMIO_VENDOR_ID);

    out(regs, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_DEVICE_STATUS_RESET);
    out(regs, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_DEVICE_STATUS_ACK);

    vio_dev_alloc(dev, device_id, vendor_id, regs);
    virtio_device_t *viodev = virtio_device_of(dev);
    vio_dev_print_ids(viodev);
    unit++;
    continue;

  skip_rm:
    bus_unmap_resource(dev, regs);
    device_remove_child(bus, dev);
  skip:
    klog("VirtIO device with phandle %d will be skipped", node);
  }

  return bus_generic_probe(bus);
}

static virtio_methods_t vio_if = {
  .read_features = vio_read_features,
  .write_features = vio_write_features,
  .read_status = vio_read_status,
  .set_status = vio_set_status,
  .ack_intr = vio_ack_intr,
  .vq_num = vio_vq_num,
  .vq_setup = vio_vq_setup,
  .vq_enable = vio_vq_enable,
  .vq_kick = vio_vq_kick,
};

static driver_t vio_bus = {
  .desc = "VirtIO bus driver",
  .size = sizeof(vio_state_t),
  .pass = SECOND_PASS,
  .probe = vio_probe,
  .attach = vio_attach,
  .interfaces = { [DIF_VIRTIO] = &vio_if, },
};

DEVCLASS_ENTRY(root, vio_bus);
