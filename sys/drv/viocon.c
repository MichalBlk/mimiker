/*
 * VirtIO console driver.
 */
#define KL_LOG KL_DEV
#include <sys/bus.h>
#include <sys/devclass.h>
#include <sys/klog.h>
#include <sys/kmem.h>
#include <dev/uart.h>
#include <dev/virtio.h>

/* This value is used with kmem_alloc_contig so it must be
 * a power of 2 that is not smaller than PAGESIZE. */
#define VIOCON_BUFSIZE PAGESIZE

static_assert(powerof2(VIOCON_BUFSIZE) && VIOCON_BUFSIZE >= PAGESIZE,
    "VirtIO console buffer size is invalid");

typedef struct viocon_state {
  device_t *dev;

  ringbuf_t rx_rb;
  virtqueue_t *rx_vq;
  uint8_t *rx_buf;

  virtqueue_t *tx_vq;
  uint8_t *tx_buf;
} viocon_state_t;

/*
 * UART interface.
 */

static bool viocon_rx_ready(void *state) {
  viocon_state_t *viocon = state;
  return viocon->rx_rb.count;
}

static uint8_t viocon_getc(void *state) {
  viocon_state_t *viocon = state;
  uint8_t c;
  ringbuf_getb(&viocon->rx_rb, &c);
  return c;
}

static bool viocon_tx_ready(void *state) {
  return true;
}

static void viocon_putc(void *state, uint8_t c) {
  viocon_state_t *viocon = state;
  vring_desc_t *d = viocon->tx_vq->desc;
  viocon->tx_buf[0] = c;
  virtio_vq_schedule_blocking(viocon->dev, viocon->tx_vq, d);
}

static void viocon_tx_enable(void *state) {
  /* The TTY system assumes that the device will trigger an interrupt
   * after a call to this procedure. For this reason, let's write a single
   * character from the UART ring buffer, which will cause an interrupt
   * upon completion. */
  viocon_state_t *viocon = state;
  uart_state_t *uart = viocon->dev->state;
  uint8_t c;
  ringbuf_getb(&uart->u_tx_buf, &c);
  viocon_putc(viocon, c);
}

static void viocon_tx_disable(void *state) {
  /* Nothing to do here. */
}

/*
 * Virtqueue interrupt handling.
 */

static void viocon_rx_intr(void *state, vring_desc_t *d, size_t len) {
  viocon_state_t *viocon = state;
  ringbuf_putnb(&viocon->rx_rb, viocon->rx_buf, len);

  uart_intr(viocon->dev);

  virtio_vq_schedule_from_isr(viocon->rx_vq, d);
  virtio_vq_kick(viocon->dev, viocon->rx_vq->id);
}

/*
 * Driver interface.
 */

static int viocon_probe(device_t *dev) {
  virtio_device_t *viodev = virtio_device_of(dev);
  assert(viodev);
  return viodev->device_id == VIRTIO_DEVICE_ID_CONSOLE;
}

static int viocon_attach(device_t *dev) {
  viocon_state_t *viocon =
    kmalloc(M_DEV, sizeof(viocon_state_t), M_WAITOK | M_ZERO);
  int err = 0;

  viocon->dev = dev;
  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_DRIVER);

  if ((err = virtio_select_features(dev, VIRTIO_F_VERSION_1)))
    goto bad;

  if ((err = virtio_vq_init(dev, 0, "receiveq", &viocon->rx_vq)))
    goto bad;
  if ((err = virtio_vq_init(dev, 1, "transmitq", &viocon->tx_vq)))
    goto bad;

  /* Initialize receiving. */
  paddr_t buf_pa;
  viocon->rx_buf = (void *)kmem_alloc_contig(&buf_pa, VIOCON_BUFSIZE, PMAP_NOCACHE);
  assert(viocon->rx_buf);

  vring_desc_t *d = viocon->rx_vq->desc;
  d->addr = buf_pa;
  d->len = VIOCON_BUFSIZE;
  d->flags = VRING_DESC_F_WRITE;

  virtio_vq_schedule(viocon->rx_vq, d);

  ringbuf_init(&viocon->rx_rb, kmalloc(M_DEV, VIOCON_BUFSIZE, M_ZERO), VIOCON_BUFSIZE);

  virtio_vq_setup_intr(dev, viocon->rx_vq, viocon_rx_intr, viocon);

  /* Initialize transmitting. */
  d = viocon->tx_vq->desc;
  viocon->tx_buf = (void *)kmem_alloc_contig(&buf_pa, PAGESIZE, PMAP_NOCACHE);
  assert(viocon->tx_buf);
  d->addr = buf_pa;
  d->len = sizeof(uint8_t);

  /* NOTE: we don't need to register a TX intr handling routine,
   * as the function would simply be empty. */

  tty_t *tty = tty_alloc();
  tty->t_termios.c_ispeed = 115200;
  tty->t_termios.c_ospeed = 115200;
  tty->t_ops.t_notify_out = uart_tty_notify_out;

  uart_init(dev, "VirtIO console", VIOCON_BUFSIZE, viocon, tty);

  /* Prepare /dev/uart interface. */
  tty_makedev(NULL, "uart", tty);

  /* At this point the device is ready to go. */
  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK);
  virtio_vq_kick(dev, viocon->rx_vq->id);
  return 0;

bad:
  virtio_set_status(dev, VIRTIO_CONFIG_DEVICE_STATUS_FAILED);
  return err;
}

static uart_methods_t viocon_methods = {
  .rx_ready = viocon_rx_ready,
  .getc = viocon_getc,
  .tx_ready = viocon_tx_ready,
  .putc = viocon_putc,
  .tx_enable = viocon_tx_enable,
  .tx_disable = viocon_tx_disable,
};

static driver_t viocon_driver = {
  .desc = "VirtIO console",
  .size = sizeof(uart_state_t),
  .pass = SECOND_PASS,
  .probe = viocon_probe,
  .attach = viocon_attach,
  .interfaces =
    {
      [DIF_UART] = &viocon_methods,
    },
};

DEVCLASS_ENTRY(virtio, viocon_driver);
