/*
 * VirtIO GPU driver.
 *
 * TODO: currently, all graphical applications in Mimiker rely on the vga
 * interface exposed by /dev/vga. For this reason, the VirtIO GPU driver
 * (poorly) emulates this imterface. In the future, the driver should provide
 * a /dev/fb device instead.
 */
#define KL_LOG KL_DEV
#include <stdatomic.h>
#include <sys/bus.h>
#include <sys/devfs.h>
#include <sys/devclass.h>
#include <sys/errno.h>
#include <sys/fb.h>
#include <sys/fcntl.h>
#include <sys/klog.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/pmap.h>
#include <sys/uio.h>
#include <dev/virtio.h>
#include <dev/virtio_gpu.h>

/* Each supported command will use at most VIOGPU_BUFCNT descriptors. */
#define VIOGPU_BUFCNT 3
#define VIOGPU_BUFSIZE PAGESIZE

/* XXX: for now, we assume a fixed resolution. */
#define VIOGPU_FBWIDTH 640
#define VIOGPU_FBHEIGHT 480
#define VIOGPU_FBBPP 8
#define VIOGPU_FBPIXELCNT (VIOGPU_FBWIDTH * VIOGPU_FBHEIGHT)
#define VIOGPU_FBSIZE (VIOGPU_FBPIXELCNT * sizeof(uint32_t))
#define VIOGPU_FBPALSIZE 256
#define VIOGPU_FBRESID 1

#define VIOGPU_SCANOUTID 0

typedef struct viogpu_buf {
  void *va;
  paddr_t pa;
} viogpu_buf_t;

typedef struct viogpu_state {
  viogpu_buf_t buf[VIOGPU_BUFCNT];
  virtqueue_t *ctrl_vq;
  fb_palette_t fb_pal;
  fb_info_t fb_info;
  uint32_t *fb;
  uint8_t *fb_buf;
  paddr_t fb_pa;
  atomic_int usecnt;
} viogpu_state_t;

/*
 * VirtIO GPU commands.
 */

static int viogpu_display_info(device_t *dev) {
  viogpu_state_t *viogpu = dev->state;
  viogpu_buf_t *buf = viogpu->buf;

  /* Prepare the command descriptor. */
  virtio_gpu_ctrl_hdr_t *cmd = buf[0].va;
  cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
  vring_desc_t *cd = viogpu->ctrl_vq->desc;
  cd->addr = buf[0].pa;
  cd->len = sizeof(*cmd);
  cd->flags = VRING_DESC_F_NEXT;
  cd->next = 1;

  /* Prepare the response descriptor. */
  virtio_gpu_resp_display_info_t *rsp = buf[1].va;
  vring_desc_t *rd = cd + 1;
  rd->addr = buf[1].pa;
  rd->len = sizeof(*rsp);
  rd->flags = VRING_DESC_F_WRITE;

  virtio_vq_schedule_blocking(dev, viogpu->ctrl_vq, cd);

  if (rsp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
    klog("VirtIO GPU has failed to execute the display info command");
    return ENXIO;
  }

  if (!rsp->pmodes[VIOGPU_SCANOUTID].enabled) {
    klog("VirtIO GPU encountered a disabled scanout 0");
    return ENXIO;
  }

  uint32_t width = rsp->pmodes[VIOGPU_SCANOUTID].r.width;
  uint32_t height = rsp->pmodes[VIOGPU_SCANOUTID].r.height;

  klog("VirtIO GPU max resoultion: width=%d, height=%d, bpp=%d", width,
      height, VIOGPU_FBBPP);

  if (width < VIOGPU_FBWIDTH || height < VIOGPU_FBHEIGHT)
    return ENXIO;

  fb_info_t *fb_info = &viogpu->fb_info;
  fb_info->width = VIOGPU_FBWIDTH;
  fb_info->height = VIOGPU_FBHEIGHT;
  fb_info->bpp = VIOGPU_FBBPP;

  klog("VirtIO GPU final resolution: width=%d, height=%d, bpp=%d",
      fb_info->width, fb_info->height, fb_info->bpp);

  return 0;
}

static int viogpu_resource_create_2d(device_t *dev) {
  viogpu_state_t *viogpu = dev->state;
  viogpu_buf_t *buf = viogpu->buf;

  /* Prepare the command descriptor. */
  virtio_gpu_resource_create_2d_t *cmd = buf[0].va;
  cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  cmd->resource_id = VIOGPU_FBRESID;
  cmd->format = VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM;
  cmd->width = viogpu->fb_info.width;
  cmd->height = viogpu->fb_info.height;

  vring_desc_t *cd = viogpu->ctrl_vq->desc;
  cd->addr = buf[0].pa;
  cd->len = sizeof(*cmd);
  cd->flags = VRING_DESC_F_NEXT;
  cd->next = 1;

  /* Prepare the response descriptor. */
  virtio_gpu_ctrl_hdr_t *rsp = buf[1].va;
  vring_desc_t *rd = cd + 1;
  rd->addr = buf[1].pa;
  rd->len = sizeof(*rsp);
  rd->flags = VRING_DESC_F_WRITE;

  klog("last_uidx=%d", viogpu->ctrl_vq->last_uidx);

  virtio_vq_schedule_blocking(dev, viogpu->ctrl_vq, cd);

  if (rsp->type != VIRTIO_GPU_RESP_OK_NODATA) {
    klog("VirtIO GPU has failed to create a 2D resource");
    return ENXIO;
  }

  return 0;
}

static int viogpu_resource_attach_backing(device_t *dev) {
  viogpu_state_t *viogpu = dev->state;
  viogpu_buf_t *buf = viogpu->buf;

  /* Prepare the command descriptor. */
  virtio_gpu_resource_attach_backing_t *cmd = buf[0].va;
  cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  cmd->resource_id = VIOGPU_FBRESID;
  cmd->nr_entries = 1;

  vring_desc_t *cd = viogpu->ctrl_vq->desc;
  cd->addr = buf[0].pa;
  cd->len = sizeof(*cmd);
  cd->flags = VRING_DESC_F_NEXT;
  cd->next = 1;

  /* Prepare the data descriptor. */
  virtio_gpu_mem_entry_t *data = viogpu->buf[1].va;
  data->addr = viogpu->fb_pa;
  data->length = VIOGPU_FBSIZE;

  vring_desc_t *dd = cd + 1;
  dd->addr = buf[1].pa;
  dd->len = sizeof(*data);
  dd->flags = VRING_DESC_F_NEXT;
  dd->next = 2;

  /* Prepare the response descriptor. */
  virtio_gpu_ctrl_hdr_t *rsp = buf[2].va;
  vring_desc_t *rd = dd + 1;
  rd->addr = buf[2].pa;
  rd->len = sizeof(*rsp);
  rd->flags = VRING_DESC_F_WRITE;

  virtio_vq_schedule_blocking(dev, viogpu->ctrl_vq, cd);

  if (rsp->type != VIRTIO_GPU_RESP_OK_NODATA) {
    klog("VirtIO GPU has failed to attach backing storage to the fb resource");
    return ENXIO;
  }

  return 0;
}

static int viogpu_set_scanout(device_t *dev) {
  viogpu_state_t *viogpu = dev->state;
  viogpu_buf_t *buf = viogpu->buf;

  /* Prepare the command descriptor. */
  virtio_gpu_set_scanout_t *cmd = buf[0].va;
  cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  cmd->r.x = 0;
  cmd->r.y = 0;
  cmd->r.width = viogpu->fb_info.width;
  cmd->r.height = viogpu->fb_info.height;
  cmd->scanout_id = VIOGPU_SCANOUTID;
  cmd->resource_id = VIOGPU_FBRESID;

  vring_desc_t *cd = viogpu->ctrl_vq->desc;
  cd->addr = buf[0].pa;
  cd->len = sizeof(*cmd);
  cd->flags = VRING_DESC_F_NEXT;
  cd->next = 1;

  /* Prepare the response descriptor. */
  virtio_gpu_ctrl_hdr_t *rsp = buf[1].va;
  vring_desc_t *rd = cd + 1;
  rd->addr = buf[1].pa;
  rd->len = sizeof(*rsp);
  rd->flags = VRING_DESC_F_WRITE;

  virtio_vq_schedule_blocking(dev, viogpu->ctrl_vq, cd);

  if (rsp->type != VIRTIO_GPU_RESP_OK_NODATA) {
    klog("VirtIO GPU has failed to set the scanout");
    return ENXIO;
  }

  return 0;
}

static int viogpu_transfer_to_host_2d(device_t *dev) {
  viogpu_state_t *viogpu = dev->state;
  viogpu_buf_t *buf = viogpu->buf;

  /* Prepare the command descriptor. */
  virtio_gpu_transfer_to_host_2d_t *cmd = buf[0].va;
  cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  cmd->r.x = 0;
  cmd->r.y = 0;
  cmd->r.width = viogpu->fb_info.width;
  cmd->r.height = viogpu->fb_info.height;
  cmd->offset = 0;
  cmd->resource_id = VIOGPU_FBRESID;

  vring_desc_t *cd = viogpu->ctrl_vq->desc;
  cd->addr = buf[0].pa;
  cd->len = sizeof(*cmd);
  cd->flags = VRING_DESC_F_NEXT;
  cd->next = 1;

  /* Prepare the response descriptor. */
  virtio_gpu_ctrl_hdr_t *rsp = buf[1].va;
  vring_desc_t *rd = cd + 1;
  rd->addr = buf[1].pa;
  rd->len = sizeof(*rsp);
  rd->flags = VRING_DESC_F_WRITE;

  virtio_vq_schedule_blocking(dev, viogpu->ctrl_vq, cd);

  if (rsp->type != VIRTIO_GPU_RESP_OK_NODATA) {
    klog("VirtIO GPU has failed to transfer the fb resource to the host");
    return ENXIO;
  }

  return 0;
}

static int viogpu_resource_flush(device_t *dev) {
  viogpu_state_t *viogpu = dev->state;
  viogpu_buf_t *buf = viogpu->buf;

  /* Prepare the command descriptor. */
  virtio_gpu_resource_flush_t *cmd = buf[0].va;
  cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  cmd->r.x = 0;
  cmd->r.y = 0;
  cmd->r.width = viogpu->fb_info.width;
  cmd->r.height = viogpu->fb_info.height;
  cmd->resource_id = VIOGPU_FBRESID;

  vring_desc_t *cd = viogpu->ctrl_vq->desc;
  cd->addr = buf[0].pa;
  cd->len = sizeof(*cmd);
  cd->flags = VRING_DESC_F_NEXT;
  cd->next = 1;

  /* Prepare the response descriptor. */
  virtio_gpu_ctrl_hdr_t *rsp = buf[1].va;
  vring_desc_t *rd = cd + 1;
  rd->addr = buf[1].pa;
  rd->len = sizeof(*rsp);
  rd->flags = VRING_DESC_F_WRITE;

  virtio_vq_schedule_blocking(dev, viogpu->ctrl_vq, cd);

  if (rsp->type != VIRTIO_GPU_RESP_OK_NODATA) {
    klog("VirtIO GPU has failed to flush the fb resource");
    return ENXIO;
  }

  return 0;
}

static int viogpu_draw(device_t *dev) {
  int err = 0;

  if ((err = viogpu_transfer_to_host_2d(dev)))
    return err;
  if ((err = viogpu_resource_flush(dev)))
    return err;

  return 0;
}


/*
 * Device file interface.
 */

static int viogpu_open(devnode_t *dev, file_t *fp, int oflags) {
  device_t *device = dev->data;
  viogpu_state_t *viogpu = device->state;

  if ((oflags & O_ACCMODE) != O_WRONLY)
    return EACCES;

  /* Disallow opening the file more than once. */
  int expected = 0;
  if (!atomic_compare_exchange_strong(&viogpu->usecnt, &expected, 1))
    return EBUSY;

  return 0;
}

static int viogpu_close(devnode_t *dev, file_t *fp) {
  device_t *device = dev->data;
  viogpu_state_t *viogpu = device->state;
  //memset(viogpu->fb, 0, VIOGPU_FBSIZE);
  atomic_store(&viogpu->usecnt, 0);
  return 0;
}

static int viogpu_write(devnode_t *dev, uio_t *uio) {
  device_t *device = dev->data;
  viogpu_state_t *viogpu = device->state;
  int n = uio->uio_resid;
  int err = 0;

  if ((err = uiomove_frombuf(viogpu->fb_buf, VIOGPU_FBPIXELCNT, uio)))
    return err;

  for (int i = 0; i < n; i++) {
    uint8_t idx = viogpu->fb_buf[i];
    fb_color_t *color = &viogpu->fb_pal.colors[idx];
    uint32_t r = color->r;
    uint32_t g = color->g;
    uint32_t b = color->b;
    viogpu->fb[i] = (b << 16) | (g << 8) | r;
  }

  if ((err = viogpu_draw(device)))
    return err;

  return 0;
}

static int viogpu_ioctl(devnode_t *dev, u_long cmd, void *data, int fflags) {
  device_t *device = dev->data;
  viogpu_state_t *viogpu = device->state;

  if (cmd == FBIOCGET_FBINFO) {
    memcpy(data, &viogpu->fb_info, sizeof(fb_info_t));
    return 0;
  }

  if (cmd == FBIOCSET_FBINFO)
    return memcmp(&viogpu->fb_info, data, sizeof(fb_info_t)) ? EINVAL : 0;

  if (cmd == FBIOCSET_PALETTE) {
    fb_palette_t *pal = data;
    if (pal->len == 0 || pal->len > VIOGPU_FBPALSIZE)
      return EINVAL;
    return copyin(pal->colors, viogpu->fb_pal.colors, pal->len * sizeof(fb_color_t));
  }

  return EINVAL;
}

static devops_t viogpu_devops = {
  .d_type = DT_SEEKABLE,
  .d_open = viogpu_open,
  .d_close = viogpu_close,
  .d_write = viogpu_write,
  .d_ioctl = viogpu_ioctl,
};

/*
 * Driver interface.
 */

static int viogpu_probe(device_t *dev) {
  virtio_device_t *viodev = virtio_device_of(dev);
  assert(viodev);
  return viodev->device_id == VIRTIO_DEVICE_ID_GPU;
}

static int viogpu_attach(device_t *dev) {
  viogpu_state_t *viogpu = dev->state;
  int err = 0;

  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_DRIVER);

  if ((err = virtio_select_features(dev, VIRTIO_F_VERSION_1)))
    goto bad;

  if ((err = virtio_vq_init(dev, 0, "controlq", &viogpu->ctrl_vq)))
    goto bad;

  for (int i = 0; i < VIOGPU_BUFCNT; i++) {
    viogpu_buf_t *buf = &viogpu->buf[i];
    buf->va = (void *)kmem_alloc_contig(&buf->pa, VIOGPU_BUFSIZE, PMAP_NOCACHE);
    assert(buf->va);
  }

  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK);

  if ((err = viogpu_display_info(dev)))
    goto bad;

  if ((err = viogpu_resource_create_2d(dev)))
    goto bad;

  /* Prepare the framebuffer. */
  size_t fb_sz = 1 << (log2(VIOGPU_FBSIZE) + 1);
  if (!(viogpu->fb = (void *)kmem_alloc_contig(&viogpu->fb_pa, fb_sz, PMAP_NOCACHE))) {
    err = ENXIO;
    goto bad;
  }
  if (!(viogpu->fb_buf = kmalloc(M_DEV, VIOGPU_FBPIXELCNT, 0))) {
    err = ENXIO;
    goto bad;
  }
  memset(viogpu->fb, 0, VIOGPU_FBSIZE);

  fb_palette_t *fb_pal = &viogpu->fb_pal;
  fb_pal->len = VIOGPU_FBPALSIZE;
  if (!(fb_pal->colors = kmalloc(M_DEV, VIOGPU_FBPALSIZE * sizeof(fb_color_t), M_ZERO))) {
    err = ENXIO;
    goto bad;
  }

  if ((err = viogpu_resource_attach_backing(dev)))
    goto bad;

  if ((err = viogpu_set_scanout(dev)))
    goto bad;

  if ((err = viogpu_draw(dev)))
    goto bad;

  /* Install /dev/vga device file. */
  devfs_makedev_new(NULL, "vga", &viogpu_devops, dev, NULL);

  return 0;

bad:
  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_FAILED);
  return err;
}

static driver_t viogpu_driver = {
  .desc = "VirtIO GPU",
  .size = sizeof(viogpu_state_t),
  .pass = SECOND_PASS,
  .probe = viogpu_probe,
  .attach = viogpu_attach,
};

DEVCLASS_ENTRY(virtio, viogpu_driver);
