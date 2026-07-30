#include <linux/module.h>      /* core header for loadable kernel modules (MODULE_LICENSE, THIS_MODULE, etc.) */
#include <linux/miscdevice.h>  /* struct miscdevice, module_misc_device(), MISC_DYNAMIC_MINOR */
#include <linux/fs.h>          /* struct file, struct file_operations */
#include <linux/uaccess.h>     /* helpers for safely copying data to/from user space */

/* Fixed message returned to any userspace reader of /dev/modbus0 */
static const char msg[] = "modbus0: hello from increment 4a!";

/* Handler invoked when userspace does read() on /dev/modbus0.
 * f   - open file instance (unused here)
 * buf - destination buffer in user space
 * len - number of bytes the caller wants to read
 * off - current file offset, updated by the helper as data is consumed
 */
static ssize_t hello_read(struct file *f, char __user *buf,
                          size_t len, loff_t *off)
{
    /* Copies from the kernel-space msg buffer into the user-space buf,
     * honoring *off so repeated reads eventually return 0 (EOF) instead
     * of looping forever. sizeof(msg) - 1 excludes the string's NUL terminator.
     */
    return simple_read_from_buffer(buf, len, off, msg, sizeof(msg) - 1);
}

/* Maps file operations (like read) to our implementation; anything not
 * set here (write, ioctl, etc.) is left unsupported.
 */
static const struct file_operations hello_fops = {
    .owner = THIS_MODULE,   /* ties this fops to the module for refcounting, prevents unload while open */
    .read = hello_read,     /* called for read() syscalls on the device node */
};

/* Describes the misc character device to register with the kernel */
static struct miscdevice hello_dev = {
    .minor = MISC_DYNAMIC_MINOR,  /* let the kernel pick a free minor number instead of hardcoding one */
    .name = "modbus0",            /* device node will appear as /dev/modbus0 */
    .fops = &hello_fops,          /* file operations table defined above */
    .mode = 0666,                 /* permissions on the created device node (rw for all) */
};

/* Convenience macro that generates module_init()/module_exit() functions
 * which register/deregister hello_dev automatically on load/unload.
 */
module_misc_device(hello_dev); // init call

MODULE_LICENSE("GPL");        /* required for the module to use GPL-only kernel symbols */
MODULE_AUTHOR("Dinesh Sreekumar Panicker");
MODULE_DESCRIPTION("Increment 4a: miscdevice skeleton for modbus driver");
