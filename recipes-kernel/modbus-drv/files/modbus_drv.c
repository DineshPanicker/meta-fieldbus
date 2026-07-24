#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

static const char msg[] = "modbus0: hello from increment 4a!";

static ssize_t hello_read(struct file *f, char __user *buf,
                          size_t len, loff_t *off)
{
    return simple_read_from_buffer(buf, len, off, msg, sizeof(msg) - 1);
}

static const struct file_operations hello_fops = {
    .owner = THIS_MODULE,
    .read = hello_read,
};
static struct miscdevice hello_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "modbus0",
    .fops = &hello_fops,
    .mode = 0666,
};
module_misc_device(hello_dev);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dinesh Sreekumar Panicker");
MODULE_DESCRIPTION("Increment 4a: miscdevice skeleton for modbus driver");