#ifndef _DEV_VIRTIO_H_
#define _DEV_VIRTIO_H_

#include <sys/device.h>
#include <dev/virtio_reg.h>

typedef struct virtqueue virtqueue_t;

typedef LIST_HEAD(vq_list, virtqueue) vq_list_t;

typedef struct virtio_device {
  vq_list_t vqs;
  resource_t *regs;
  resource_t *irq;
  uint32_t device_id;
  uint32_t vendor_id;
  uint64_t features;
  int nvqs;
} virtio_device_t;

typedef void (*vq_isr_t)(void *, vring_desc_t *, size_t);

struct virtqueue {
  mtx_t lock;
  LIST_ENTRY(virtqueue) link;
  vring_desc_t *desc;
  vring_avail_t *avail;
  vring_used_t *used;
  vq_isr_t isr;
  void *arg;
  const char *name;
  uint32_t id;
  uint16_t num;
  uint16_t last_uidx;
};

static inline virtio_device_t *virtio_device_of(device_t *dev) {
  return dev->bus == DEV_BUS_VIRTIO ? dev->instance : NULL;
}

void virtio_init(device_t *bus, int unit, device_t *pic);

/*
 * VirtIO interface.
 */

typedef uint64_t (*virtio_read_features_t)(device_t *);
typedef void (*virtio_write_features_t)(device_t *, uint64_t);
typedef uint32_t (*virtio_read_status_t)(device_t *);
typedef void (*virtio_set_status_t)(device_t *, uint32_t);
typedef void (*virtio_ack_intr_t)(device_t *);
typedef uint16_t (*virtio_vq_num_t)(device_t *, uint32_t);
typedef void (*virtio_vq_setup_t)(device_t *, uint32_t, uint64_t, uint64_t, uint64_t);
typedef void (*virtio_vq_enable_t)(device_t *, uint32_t);
typedef void (*virtio_vq_kick_t)(device_t *, uint32_t);

typedef struct virtio_methods {
  virtio_read_features_t read_features;
  virtio_write_features_t write_features;
  virtio_read_status_t read_status;
  virtio_set_status_t set_status;
  virtio_ack_intr_t ack_intr;
  virtio_vq_num_t vq_num;
  virtio_vq_setup_t vq_setup;
  virtio_vq_enable_t vq_enable;
  virtio_vq_kick_t vq_kick;
} virtio_methods_t;

static inline virtio_methods_t *virtio_methods(device_t *dev) {
  return (virtio_methods_t *)dev->driver->interfaces[DIF_VIRTIO];
}

static inline uint64_t virtio_read_features(device_t *dev) {
  return virtio_methods(dev->parent)->read_features(dev);
}

static inline void virtio_write_features(device_t *dev, uint64_t val) {
  virtio_methods(dev->parent)->write_features(dev, val);
}

static inline uint32_t virtio_read_status(device_t *dev) {
  return virtio_methods(dev->parent)->read_status(dev);
}

static inline void virtio_set_status(device_t *dev, uint32_t val) {
  virtio_methods(dev->parent)->set_status(dev, val);
}

static inline void virtio_ack_intr(device_t *dev) {
  virtio_methods(dev->parent)->ack_intr(dev);
}

static inline uint16_t virtio_vq_num(device_t *dev, uint32_t id) {
  return virtio_methods(dev->parent)->vq_num(dev, id);
}

static inline void virtio_vq_setup(device_t *dev, uint32_t id, uint64_t desc_pa, uint64_t avail_pa, uint64_t used_pa) {
  virtio_methods(dev->parent)->vq_setup(dev, id, desc_pa, avail_pa, used_pa);
}

static inline void virtio_vq_enable(device_t *dev, uint32_t id) {
  virtio_methods(dev->parent)->vq_enable(dev, id);
}

static inline void virtio_vq_kick(device_t *dev, uint32_t id) {
  virtio_methods(dev->parent)->vq_kick(dev, id);
}

/*
 * VirtIO standard functions.
 */

int virtio_select_features(device_t *dev, uint64_t val);

/*
 * Virtqueue handling functions.
 */

int virtio_vq_init(device_t *dev, uint32_t id, const char *name, virtqueue_t **vqp);
void virtio_vq_setup_intr(device_t *dev, virtqueue_t *vq, vq_isr_t isr, void *arg);
void virtio_vq_schedule(virtqueue_t *vq, vring_desc_t *d);
void virtio_vq_schedule_from_isr(virtqueue_t *vq, vring_desc_t *d);
void virtio_vq_schedule_blocking(device_t *dev, virtqueue_t *vq, vring_desc_t *d);

#endif /* !_DEV_VIRTIO_H_ */
