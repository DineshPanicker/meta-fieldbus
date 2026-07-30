#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define MODBUS_MAX_FRAME 256 /* max size of one Modbus RTU frame we'll buffer in either direction */

/* Per-device driver state. One instance is allocated per serdev_device that
 * matches this driver (see modbus_probe()).
 */
struct modbus_priv
{
    struct serdev_device *serdev; /* back-pointer to the underlying serial device, used to read/write the UART */
    struct miscdevice miscdev;    /* embedded misc-device registration; exposes this instance as /dev/modbusN */
    u8 rxbuf[MODBUS_MAX_FRAME];   /* accumulates bytes received from the serial line until userspace reads them */
    size_t rxlen;                 /* number of valid bytes currently sitting in rxbuf */
};

/* serdev callback: invoked by the serial core whenever new bytes arrive on
 * the UART for this device. Runs in the serdev driver's receive context,
 * not in a userspace read() call.
 *   sdev  - the serial device that received data
 *   buf   - newly arrived raw bytes (owned by the framework, valid only during this call)
 *   count - number of bytes available in buf
 * Returns the number of bytes consumed; serdev redelivers whatever isn't
 * consumed, so returning less than count is valid backpressure.
 */
static size_t modbus_receive_buf(struct serdev_device *sdev,
                                 const u8 *buf, size_t count)
{
    struct modbus_priv *priv = serdev_device_get_drvdata(sdev);
    size_t space = sizeof(priv->rxbuf) - priv->rxlen;
    size_t n = min(count, space);

    if (n)
    {
        memcpy(priv->rxbuf + priv->rxlen, buf, n);
        priv->rxlen += n;
    }

    /* Buffer full with no framing yet (4b): drop to avoid livelock.
     * 4c's t3.5 timer will drain the buffer and this won't trigger. */
    if (n < count)
    {
        priv->rxlen = 0; /* discard and restart */
        return count;    /* tell serdev we consumed everything */
    }

    return n; /* consumed n bytes */
}

/* Callback table handed to the serdev core so it knows how to notify this
 * driver of incoming data. This is the serdev analogue of file_operations.
 */
static const struct serdev_device_ops modbus_serdev_ops = {
    .receive_buf = modbus_receive_buf, /* called on every chunk of incoming UART data */
};

/* read() handler for /dev/modbusN: hands buffered RX bytes to userspace. */
static ssize_t modbus_read(struct file *f, char __user *ubuf,
                           size_t len, loff_t *off)
{
    /* f->private_data was set to &priv->miscdev by the misc-device open path;
     * container_of walks back from that embedded member to the enclosing
     * modbus_priv so we can reach rxbuf/rxlen.
     */
    struct modbus_priv *priv = container_of(f->private_data,
                                            struct modbus_priv, miscdev);
    ssize_t ret;

    /* Copy out of rxbuf starting at *off, advancing *off as bytes are consumed. */
    ret = simple_read_from_buffer(ubuf, len, off, priv->rxbuf, priv->rxlen);
    /* Once the caller has read past the end of the buffered data, reset the
     * buffer so the next receive_buf() call starts a fresh frame at offset 0.
     */
    if (ret > 0 && *off >= (loff_t)priv->rxlen)
        priv->rxlen = 0;
    return ret;
}

/* write() handler for /dev/modbusN: forwards a userspace buffer out over the
 * serial line, e.g. to send a Modbus RTU request frame.
 */
static ssize_t modbus_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
    struct modbus_priv *priv = container_of(f->private_data, struct modbus_priv, miscdev);
    u8 tx[MODBUS_MAX_FRAME]; /* kernel-space staging buffer for the outgoing frame */

    /* Reject writes larger than we can stage; keeps memcpy/copy_from_user bounded. */
    if (len > sizeof(tx))
        return -EINVAL;
    /* Safely copy the frame from user space into the kernel buffer. */
    if (copy_from_user(tx, ubuf, len))
        return -EFAULT;
    /* Hand the bytes to the serdev core to transmit on the UART. */
    return serdev_device_write_buf(priv->serdev, tx, len);
}

/* File operations exposed at /dev/modbusN. */
static const struct file_operations modbus_fops = {
    .owner = THIS_MODULE, /* ties this fops to the module for refcounting, prevents unload while open */
    .read = modbus_read,
    .write = modbus_write,
};

/* Called by the serdev/device-tree core when a device matching
 * modbus_of_match is found and bound to this driver.
 */
static int modbus_probe(struct serdev_device *sdev)
{
    struct modbus_priv *priv;
    int ret;

    /* Allocate state that's automatically freed when sdev is removed
     * (devm_ ties the allocation's lifetime to the device).
     */
    priv = devm_kzalloc(&sdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->serdev = sdev;
    /* Attach priv to sdev so later callbacks (receive_buf, remove) can fetch it back. */
    serdev_device_set_drvdata(sdev, priv);
    /* Register our receive_buf callback with the serdev core. */
    serdev_device_set_client_ops(sdev, &modbus_serdev_ops);

    /* Open the underlying UART/tty for this serdev device. */
    ret = serdev_device_open(sdev);
    if (ret)
        return ret;

    /* Configure the line for Modbus RTU: 115200 baud, 8N1, no flow control. */
    serdev_device_set_baudrate(sdev, 115200);
    serdev_device_set_flow_control(sdev, false);
    serdev_device_set_parity(sdev, SERDEV_PARITY_NONE);

    /* Fill in the embedded miscdevice and register /dev/modbus0. */
    priv->miscdev.minor = MISC_DYNAMIC_MINOR; /* let the kernel pick a free minor number */
    priv->miscdev.name = "modbus0";           /* device node will appear as /dev/modbus0 */
    priv->miscdev.fops = &modbus_fops;
    priv->miscdev.mode = 0666; /* rw for all users */
    ret = misc_register(&priv->miscdev);
    if (ret)
        goto err_close;

    dev_info(&sdev->dev, "modbus-rtu: probed on %s, 115200 8N1\n",
             dev_name(&sdev->dev));
    return 0;

err_close:
    /* Registration failed: undo the open() before returning the error. */
    serdev_device_close(sdev);
    return ret;
}

/* Called when the serdev device is unbound/removed; mirrors modbus_probe(). */
static void modbus_remove(struct serdev_device *sdev)
{
    struct modbus_priv *priv = serdev_device_get_drvdata(sdev);

    misc_deregister(&priv->miscdev); /* remove /dev/modbus0 */
    serdev_device_close(sdev);       /* close the UART */
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

MODULE_LICENSE("GPL"); /* required for the module to use GPL-only kernel symbols */
MODULE_AUTHOR("Dinesh Sreekumar Panicker");
MODULE_DESCRIPTION("Increment 4b: serdev binding for Modbus RTU on RS485");
