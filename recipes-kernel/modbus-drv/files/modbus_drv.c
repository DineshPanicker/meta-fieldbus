/* modbus_drv.c -- increment 4c: hrtimer t3.5 frame detection + CRC16 + kfifo */
#include <linux/module.h>          /* core header for loadable kernel modules */
#include <linux/mod_devicetable.h> /* struct of_device_id */
#include <linux/of.h>              /* devicetree matching helpers */
#include <linux/serdev.h>          /* serdev_device, serdev_device_ops, module_serdev_device_driver() */
#include <linux/miscdevice.h>      /* struct miscdevice, misc_register()/misc_deregister() */
#include <linux/fs.h>              /* struct file, struct file_operations */
#include <linux/uaccess.h>         /* copy_to_user()/copy_from_user() */
#include <linux/kfifo.h>           /* lock-free single-producer/single-consumer ring buffer */
#include <linux/hrtimer.h>         /* high-resolution timer for the 1.75ms T3.5 silence detector */
#include <linux/ktime.h>           /* ktime_get_ns() for frame timestamps */
#include <linux/wait.h>            /* wait_queue_head_t, wait_event_interruptible(), wake_up_interruptible() */

#define MODBUS_T35_NS (1750000ULL) /* 1.75 ms, fixed per spec >19200 baud: max inter-byte gap within one frame */
#define MODBUS_MAX_FRAME 256       /* max size of one Modbus RTU frame */
#define FIFO_SIZE 4096             /* bytes; must be a power of two (kfifo requirement) */

/* One complete, CRC-validated frame, handed from timer context (producer)
 * to a blocking read() in process context (consumer) via the kfifo below.
 */
struct modbus_record
{
    u64 ts_ns;                 /* time the frame was recognized as complete (your T2) */
    u16 len;                   /* number of valid bytes in data[] */
    u8 data[MODBUS_MAX_FRAME]; /* raw frame bytes, including the trailing CRC */
};

/* Per-device driver state; one instance per bound serdev_device. */
struct modbus_priv
{
    struct serdev_device *serdev; /* underlying serial device */
    struct miscdevice miscdev;    /* embedded misc-device registration; exposes /dev/modbus0 */

    struct hrtimer t35_timer;   /* fires when 1.75ms of silence has passed -- signals "frame complete" */
    u8 rxbuf[MODBUS_MAX_FRAME]; /* in-progress frame being assembled by modbus_receive_buf() */
    size_t rxlen;               /* number of valid bytes currently in rxbuf */

    struct kfifo fifo;       /* ring buffer of completed struct modbus_record entries */
    wait_queue_head_t readq; /* wakes blocked read() callers when a new record lands in fifo */
};

/* Standard Modbus CRC16 (polynomial 0xA001, init 0xFFFF), computed over
 * buf[0..len-1]. Modbus transmits the result little-endian as the frame's
 * last two bytes.
 */
static u16 modbus_crc16(const u8 *buf, size_t len)
{
    u16 crc = 0xFFFF; /* CRC register, seeded per spec */
    size_t i;
    int b;

    for (i = 0; i < len; i++)
    {
        crc ^= buf[i]; /* XOR next byte into the low byte of the CRC register */
        for (b = 0; b < 8; b++)
            /* Shift right once per bit; XOR the poly in only when the bit shifted out was 1. */
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

/* hrtimer callback: fires exactly MODBUS_T35_NS after the last received
 * byte, provided no newer byte re-armed the timer in the meantime (see
 * modbus_receive_buf()). That silence is what Modbus RTU defines as the
 * frame boundary. Runs in softirq/hrtimer context on PREEMPT_RT, not
 * process context -- keep this fast and avoid sleeping.
 */
static enum hrtimer_restart modbus_t35_expired(struct hrtimer *t)
{
    /* Recover the owning modbus_priv from the embedded hrtimer pointer. */
    struct modbus_priv *priv = container_of(t, struct modbus_priv, t35_timer);
    struct modbus_record rec;
    u16 rx_crc, calc_crc;

    if (priv->rxlen < 4)
    { /* addr + func + crc(2) is the minimum possible frame; too short to be valid */
        priv->rxlen = 0;
        return HRTIMER_NORESTART;
    }

    /* Modbus RTU appends CRC16 little-endian as the last two bytes of the frame. */
    rx_crc = priv->rxbuf[priv->rxlen - 2] |
             (priv->rxbuf[priv->rxlen - 1] << 8);
    /* Recompute the CRC over everything except those trailing CRC bytes. */
    calc_crc = modbus_crc16(priv->rxbuf, priv->rxlen - 2);

    if (rx_crc != calc_crc)
    {
        priv->rxlen = 0; /* bad frame (noise, collision, etc.) -- drop it (4d will count this) */
        return HRTIMER_NORESTART;
    }

    /* Frame is valid: snapshot it into a record for the fifo. */
    rec.ts_ns = ktime_get_ns();
    rec.len = priv->rxlen;
    memcpy(rec.data, priv->rxbuf, priv->rxlen);

    /* Only push if there's room for a whole record, else silently drop it
     * (a slow/absent reader shouldn't corrupt the fifo or block this timer).
     */
    if (kfifo_avail(&priv->fifo) >= sizeof(rec))
    {
        kfifo_in(&priv->fifo, &rec, sizeof(rec));
        wake_up_interruptible(&priv->readq); /* wake any read() blocked waiting for data */
    }

    priv->rxlen = 0;          /* reset the assembly buffer for the next frame */
    return HRTIMER_NORESTART; /* one-shot: modbus_receive_buf() re-arms it on the next byte */
}

/* serdev RX callback: invoked by the serial core for every chunk of bytes
 * arriving on the UART. Buffers them and (re)arms the silence timer so
 * that MODBUS_T35_NS of quiet after the last byte triggers frame completion.
 */
static size_t modbus_receive_buf(struct serdev_device *sdev,
                                 const u8 *buf, size_t count)
{
    struct modbus_priv *priv = serdev_device_get_drvdata(sdev);
    size_t space = sizeof(priv->rxbuf) - priv->rxlen; /* remaining room in rxbuf */
    size_t n = min(count, space);

    if (n)
    {
        memcpy(priv->rxbuf + priv->rxlen, buf, n);
        priv->rxlen += n;
    }

    /* Any byte -- valid or not -- re-arms the 1.75 ms silence timer; this
     * is the actual RTU framing rule (frame ends on inter-byte silence,
     * not on any particular byte pattern).
     */
    hrtimer_start(&priv->t35_timer, ns_to_ktime(MODBUS_T35_NS),
                  HRTIMER_MODE_REL);

    if (n < count)
    {                    /* rxbuf overflowed: frame is longer than we can hold */
        priv->rxlen = 0; /* drop the partial frame */
        return count;    /* still claim all bytes consumed so serdev doesn't redeliver them */
    }
    return n;
}

/* Wires modbus_receive_buf() into the serdev core as the RX callback. */
static const struct serdev_device_ops modbus_serdev_ops = {
    .receive_buf = modbus_receive_buf,
};

/* read() handler for /dev/modbus0: returns exactly one complete, validated
 * frame record per call, blocking until one is available unless O_NONBLOCK
 * was set on open.
 */
static ssize_t modbus_read(struct file *f, char __user *ubuf,
                           size_t len, loff_t *off)
{
    /* f->private_data points at the embedded miscdevice; recover the
     * enclosing modbus_priv to reach the fifo/waitqueue.
     */
    struct modbus_priv *priv = container_of(f->private_data,
                                            struct modbus_priv, miscdev);
    struct modbus_record rec;
    unsigned int copied;
    int ret;

    if (kfifo_is_empty(&priv->fifo))
    {
        if (f->f_flags & O_NONBLOCK)
            return -EAGAIN; /* caller asked not to block; nothing ready yet */
        /* Sleep until modbus_t35_expired() pushes a record and wakes readq. */
        ret = wait_event_interruptible(priv->readq,
                                       !kfifo_is_empty(&priv->fifo));
        if (ret)
            return ret; /* interrupted by a signal */
    }

    /* Pop exactly one record out of the fifo. */
    copied = kfifo_out(&priv->fifo, &rec, sizeof(rec));
    if (copied != sizeof(rec))
        return -EIO; /* shouldn't happen: fifo only ever holds whole records */

    /* Never hand back more than the record actually occupies. */
    if (len > sizeof(rec))
        len = sizeof(rec);
    if (copy_to_user(ubuf, &rec, len))
        return -EFAULT;
    return len;
}

/* write() handler for /dev/modbus0: forwards a userspace buffer out over
 * the serial line as-is (e.g. a Modbus RTU request the caller already built,
 * CRC included).
 */
static ssize_t modbus_write(struct file *f, const char __user *ubuf,
                            size_t len, loff_t *off)
{
    struct modbus_priv *priv = container_of(f->private_data,
                                            struct modbus_priv, miscdev);
    u8 tx[MODBUS_MAX_FRAME]; /* kernel-space staging buffer for the outgoing frame */

    if (len > sizeof(tx))
        return -EINVAL; /* reject anything we can't stage */
    if (copy_from_user(tx, ubuf, len))
        return -EFAULT;
    return serdev_device_write_buf(priv->serdev, tx, len);
}

/* File operations exposed at /dev/modbus0. */
static const struct file_operations modbus_fops = {
    .owner = THIS_MODULE,
    .read = modbus_read,
    .write = modbus_write,
};

/* Called when a serdev device matching modbus_of_match is bound to this driver. */
static int modbus_probe(struct serdev_device *sdev)
{
    struct modbus_priv *priv;
    int ret;

    /* devm_ ties this allocation's lifetime to sdev; freed automatically on remove. */
    priv = devm_kzalloc(&sdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->serdev = sdev;
    serdev_device_set_drvdata(sdev, priv);                  /* so later callbacks can fetch priv back */
    serdev_device_set_client_ops(sdev, &modbus_serdev_ops); /* register RX callback */

    /* Allocate the ring buffer that holds completed frame records. */
    ret = kfifo_alloc(&priv->fifo, FIFO_SIZE, GFP_KERNEL);
    if (ret)
        return ret;

    init_waitqueue_head(&priv->readq); /* used to block/wake read() callers */
    /* Set up (but don't yet start) the T3.5 silence-detection timer. using the legacy form */
    hrtimer_init(&priv->t35_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    priv->t35_timer.function = modbus_t35_expired;

    /* Open the underlying UART/tty for this serdev device. */
    ret = serdev_device_open(sdev);
    if (ret)
        goto err_fifo;

    /* Configure the line for Modbus RTU: 115200 baud, 8N1, no flow control. */
    serdev_device_set_baudrate(sdev, 115200);
    serdev_device_set_flow_control(sdev, false);
    serdev_device_set_parity(sdev, SERDEV_PARITY_NONE);

    /* Fill in the embedded miscdevice and register /dev/modbus0. */
    priv->miscdev.minor = MISC_DYNAMIC_MINOR;
    priv->miscdev.name = "modbus0";
    priv->miscdev.fops = &modbus_fops;
    priv->miscdev.mode = 0666;
    ret = misc_register(&priv->miscdev);
    if (ret)
        goto err_close;

    dev_info(&sdev->dev, "modbus-rtu: ready on %s, 115200 8N1, t3.5=1.75ms\n",
             dev_name(&sdev->dev));
    return 0;

err_close:
    serdev_device_close(sdev); /* undo serdev_device_open() */
err_fifo:
    kfifo_free(&priv->fifo); /* undo kfifo_alloc() */
    return ret;
}

/* Called when the serdev device is unbound/removed; mirrors modbus_probe(). */
static void modbus_remove(struct serdev_device *sdev)
{
    struct modbus_priv *priv = serdev_device_get_drvdata(sdev);

    hrtimer_cancel(&priv->t35_timer); /* stop the timer before freeing state it references */
    misc_deregister(&priv->miscdev);  /* remove /dev/modbus0 */
    serdev_device_close(sdev);        /* close the UART */
    kfifo_free(&priv->fifo);          /* release the ring buffer */
    dev_info(&sdev->dev, "modbus-rtu: removed\n");
}

/* Devicetree compatible strings this driver matches against. */
static const struct of_device_id modbus_of_match[] = {
    {.compatible = "dinesh,modbus-rtu"},
    {}};
MODULE_DEVICE_TABLE(of, modbus_of_match); /* lets modprobe autoload this module for matching DT nodes */

/* Top-level serdev driver registration: ties probe/remove to the DT match table. */
static struct serdev_device_driver modbus_driver = {
    .probe = modbus_probe,
    .remove = modbus_remove,
    .driver = {
        .name = "modbus-rtu",
        .of_match_table = modbus_of_match,
    },
};
/* Generates module_init()/module_exit() that register/unregister modbus_driver. */
module_serdev_device_driver(modbus_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dinesh Sreekumar Panicker");
MODULE_DESCRIPTION("Increment 4c: Modbus RTU frame detection on RS485");
