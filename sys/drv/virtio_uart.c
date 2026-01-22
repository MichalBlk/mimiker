#define KL_LOG KL_DEV
#include <sys/bus.h>
#include <sys/devclass.h>
#include <sys/fdt.h>
#include <sys/klog.h>
#include <sys/kmem.h>
#include <sys/sched.h>
#include <sys/thread.h>
#include <dev/uart.h>

#define VCD_QUEUE_NUM_MAX 2

#define VIRTIO_REG_MAGIC_VALUE 0x00
#define VIRTIO_REG_VERSION 0x04
#define VIRTIO_REG_DEVICE_ID 0x08
#define VIRTIO_REG_VENDOR_ID 0x0c
#define VIRTIO_REG_QUEUE_SELECT 0x30
#define VIRTIO_REG_QUEUE_SIZE_MAX 0x34
#define VIRTIO_REG_QUEUE_READY 0x44
#define VIRTIO_REG_QUEUE_NOTIFY 0x50
#define VIRTIO_REG_INTERRUPT_STATUS 0x60
#define VIRTIO_REG_INTERRUPT_ACK 0x64
#define VIRTIO_REG_STATUS 0x70
#define VIRTIO_REG_QUEUE_DESC_LOW 0x80
#define VIRTIO_REG_QUEUE_DRIVER_LOW 0x90
#define VIRTIO_REG_QUEUE_DEVICE_LOW 0xa0

#define VIRTIO_MAGIC_VALUE 0x74726976

#define VIRTIO_VERSION 2

#define VIRTIO_DEVICE_ID_CONSOLE 3 

#define VIRTIO_VENDOR_ID_QEMU 0x554d4551

#define VIRTIO_STATUS_DRIVER_OK 0x4

#define VIRTIO_INTERRUPT_USED_BUF 0x1

#define VIRTIO_CONSOLE_RX_QUEUE_NUM 0
#define VIRTIO_CONSOLE_TX_QUEUE_NUM 1

#define VU_BUFSIZE 128

typedef struct {
  void *addr;
  uint32_t __pad;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} desc_t;

typedef struct {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[VCD_QUEUE_NUM_MAX];
  uint16_t __pad;
} avail_ring_t;

typedef struct {
  uint16_t flags;
  uint16_t idx;
  struct {
    uint32_t idx;
    uint32_t len;
  } ring[VCD_QUEUE_NUM_MAX];
  uint16_t __pad;
} used_ring_t;

typedef struct vu_state {
  device_t *dev;
  resource_t *regs;
  resource_t *irq;

  desc_t *rx_d;
  avail_ring_t *rx_ar;
  used_ring_t *rx_ur;
  uint8_t *rx_buf;
  ringbuf_t rx_rb;
  int rx_uidx;

  desc_t *tx_d;
  avail_ring_t *tx_ar;
  used_ring_t *tx_ur;
  uint8_t *tx_buf;
  ringbuf_t tx_rb;
  int tx_uidx;
  bool tx_busy;
} vu_state_t;

#define rd(r) bus_read_4(vu->regs, (r))
#define wr(r, v) bus_write_4(vu->regs, (r), (v))

static paddr_t pa(void *va) {
  paddr_t rv;
  pmap_kextract((vaddr_t)va, &rv);
  return rv;
}

static bool vu_rx_ready(void *state) {
  vu_state_t *vu = state;
  return vu->rx_rb.count;
}

static uint8_t vu_getc(void *state) {
  vu_state_t *vu = state;
  uint8_t c;
  ringbuf_getb(&vu->rx_rb, &c);
  return c;
}

static bool vu_tx_ready(void *state) {
  vu_state_t *vu = state;
  return !vu->tx_busy && vu->tx_rb.count != vu->tx_rb.size;
}

static void vu_putc(void *state, uint8_t c) {
  vu_state_t *vu = state;
  if (c) {
    ringbuf_putb(&vu->tx_rb, c);
  } else {
    vu->tx_d->len = vu->tx_rb.count;
    ringbuf_getnb(&vu->tx_rb, vu->tx_buf, vu->tx_rb.count);
    vu->tx_ar->idx++;
    vu->tx_busy = true;
    wr(VIRTIO_REG_QUEUE_NOTIFY, VIRTIO_CONSOLE_TX_QUEUE_NUM);
  }
}

static intr_filter_t vu_intr(void *data) {
  device_t *dev = data;
  uart_state_t *uart = dev->state;
  vu_state_t *vu = uart->u_state;

  wr(VIRTIO_REG_INTERRUPT_ACK, VIRTIO_INTERRUPT_USED_BUF);

  if (vu->rx_ur->idx != vu->rx_uidx) {
    ringbuf_putnb(&vu->rx_rb, vu->rx_buf,
      vu->rx_ur->ring[vu->rx_uidx & (VCD_QUEUE_NUM_MAX - 1)].len);
    vu->rx_uidx++;
    vu->rx_ar->idx++;
    wr(VIRTIO_REG_QUEUE_NOTIFY, VIRTIO_CONSOLE_RX_QUEUE_NUM);
  }

  if (vu->tx_ur->idx != vu->tx_uidx) {
    vu->tx_busy = false;
    vu->tx_uidx++;
  }

  for (int rv = uart_intr(data); rv == IF_FILTERED; rv = uart_intr(data))
    continue;

  return IF_FILTERED;
}

static void vu_tx_enable(void *state) {
  vu_state_t *vu = state;
  if (!vu->tx_busy)
    vu_intr(vu->dev);
}

static void vu_tx_disable(void *state) {
}

static int vu_probe(device_t *dev) {
  return FDT_is_compatible(dev->node, "virtio,mmio");
}

static int vu_attach(device_t *dev) {
  vu_state_t *vu =
    kmalloc(M_DEV, sizeof(vu_state_t), M_WAITOK | M_ZERO);
  int err = 0;

  vu->dev = dev;

  tty_t *tty = tty_alloc();
  tty->t_termios.c_ispeed = 115200;
  tty->t_termios.c_ospeed = 115200;
  tty->t_ops.t_notify_out = uart_tty_notify_out;

  vu->regs = device_take_memory(dev, 0);
  assert(vu->regs);

  if ((err = bus_map_resource(dev, vu->regs)))
    return err;

  wr(VIRTIO_REG_STATUS, 0);

  if (rd(VIRTIO_REG_MAGIC_VALUE) != VIRTIO_MAGIC_VALUE)
    return ENXIO;
  if (rd(VIRTIO_REG_VERSION) != VIRTIO_VERSION)
    return ENXIO;
  if (rd(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_ID_CONSOLE)
    return ENXIO;
  if (rd(VIRTIO_REG_VENDOR_ID) != VIRTIO_VENDOR_ID_QEMU)
    return ENXIO;
  if (rd(VIRTIO_REG_QUEUE_SIZE_MAX) != VCD_QUEUE_NUM_MAX)
    return ENXIO;
  if (rd(VIRTIO_REG_STATUS))
    return ENXIO;
  if (rd(VIRTIO_REG_QUEUE_READY))
    return ENXIO;

  wr(VIRTIO_REG_STATUS, VIRTIO_STATUS_DRIVER_OK);
  if (rd(VIRTIO_REG_STATUS) != VIRTIO_STATUS_DRIVER_OK)
    return ENXIO;

  wr(VIRTIO_REG_QUEUE_SELECT, VIRTIO_CONSOLE_RX_QUEUE_NUM);
  if (rd(VIRTIO_REG_QUEUE_READY))
    return ENXIO;

  vu->rx_d = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);
  vu->rx_ar = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);
  vu->rx_ur = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);
  vu->rx_buf = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);

  wr(VIRTIO_REG_QUEUE_DESC_LOW, pa(vu->rx_d));
  wr(VIRTIO_REG_QUEUE_DRIVER_LOW, pa(vu->rx_ar));
  wr(VIRTIO_REG_QUEUE_DEVICE_LOW, pa(vu->rx_ur));

  vu->rx_d->addr = (void *)pa(vu->rx_buf);
  vu->rx_d->len = VU_BUFSIZE;

  vu->rx_ar->idx = 0;
  vu->rx_ar->ring[0] = vu->rx_ar->ring[1] = 0;

  vu->rx_ur->idx = 0;

  ringbuf_init(&vu->rx_rb, kmalloc(M_DEV, VU_BUFSIZE, M_ZERO), VU_BUFSIZE);

  wr(VIRTIO_REG_QUEUE_READY, 1);

  wr(VIRTIO_REG_QUEUE_SELECT, VIRTIO_CONSOLE_TX_QUEUE_NUM);
  if (rd(VIRTIO_REG_QUEUE_READY))
    return ENXIO;

  vu->tx_d = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);
  vu->tx_ar = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);
  vu->tx_ur = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);
  vu->tx_buf = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);

  wr(VIRTIO_REG_QUEUE_DESC_LOW, pa(vu->tx_d));
  wr(VIRTIO_REG_QUEUE_DRIVER_LOW, pa(vu->tx_ar));
  wr(VIRTIO_REG_QUEUE_DEVICE_LOW, pa(vu->tx_ur));

  vu->tx_d->addr = (void *)pa(vu->tx_buf);

  vu->tx_ar->idx = 0;
  vu->tx_ar->ring[0] = vu->tx_ar->ring[1] = 0;

  vu->tx_ur->idx = 0;

  ringbuf_init(&vu->tx_rb, kmalloc(M_DEV, VU_BUFSIZE, M_ZERO), VU_BUFSIZE);

  wr(VIRTIO_REG_QUEUE_READY, 1);

  uart_init(dev, "virtio uart", VU_BUFSIZE, vu, tty);

  vu->irq = device_take_irq(dev, 0);
  assert(vu->irq);

  pic_setup_intr(dev, vu->irq, vu_intr, NULL, dev, "virtio uart");

  /* Prepare /dev/uart interface. */
  tty_makedev(NULL, "uart", tty);

  vu->rx_ar->idx++;
  wr(VIRTIO_REG_QUEUE_NOTIFY, VIRTIO_CONSOLE_RX_QUEUE_NUM);

  return 0;
}

static uart_methods_t vu_methods = {
  .rx_ready = vu_rx_ready,
  .getc = vu_getc,
  .tx_ready = vu_tx_ready,
  .putc = vu_putc,
  .tx_enable = vu_tx_enable,
  .tx_disable = vu_tx_disable,
};

static driver_t vu_driver = {
  .desc = "vu",
  .size = sizeof(uart_state_t),
  .pass = SECOND_PASS,
  .probe = vu_probe,
  .attach = vu_attach,
  .interfaces =
    {
      [DIF_UART] = &vu_methods,
    },
};

DEVCLASS_ENTRY(root, vu_driver);
