#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <asm/kvm_para.h>

#define PROC_NAME "gpa_hpa"
#define BUF_SIZE 256

struct proc_data {
    char buffer[BUF_SIZE];
    bool updated;
};

static struct proc_dir_entry *proc_entry;

static inline void hypercall_gpa_to_hpa(unsigned long gpa,
                                        unsigned long *hpa,
                                        unsigned long *pfn,
                                        unsigned long *flags)
{
    unsigned long _hpa, _pfn, _flags;

    asm volatile(KVM_HYPERCALL
                 : "=a"(_hpa), "=d"(_pfn), "=S"(_flags)
                 : "a"(60), "b"(gpa)
                 : "memory");

    if (hpa)
        *hpa = _hpa;
    if (pfn)
        *pfn = _pfn;
    if (flags)
        *flags = _flags;
}

static int gpa_open(struct inode *inode, struct file *file)
{
    struct proc_data *data = kmalloc(sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
    data->updated = false;
    file->private_data = data;
    return 0;
}

static int gpa_release(struct inode *inode, struct file *file)
{
    kfree(file->private_data);
    return 0;
}

static ssize_t gpa_write(struct file *file, const char __user *ubuf,
                         size_t count, loff_t *ppos)
{
    struct proc_data *data = file->private_data;
    char buf[64];
    unsigned long long gpa;
    unsigned long flags, pfn, hpa;

    if (count >= sizeof(buf))
        return -EINVAL;

    if (copy_from_user(buf, ubuf, count))
        return -EFAULT;

    buf[count] = '\0';

    if (kstrtoull(buf, 16, &gpa))
        return -EFAULT;

    hypercall_gpa_to_hpa(gpa, &hpa, &pfn, &flags);

    snprintf(data->buffer, BUF_SIZE,
             "HPA=0x%lx PFN=0x%lx FLAGS=0x%lx\n",
             hpa, pfn, flags);
    data->updated = true;
    return count;
}

static ssize_t gpa_read(struct file *file, char __user *ubuf,
                        size_t count, loff_t *ppos)
{
    struct proc_data *data = file->private_data;
    int len = strlen(data->buffer);

    if (data->updated) {
        *ppos = 0;
        data->updated = false;
    }

    if (*ppos >= len)
        return 0;

    if (count > len - *ppos)
        count = len - *ppos;

    if (copy_to_user(ubuf, data->buffer + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static const struct proc_ops gpa_fops = {
    .proc_open    = gpa_open,
    .proc_release = gpa_release,
    .proc_read    = gpa_read,
    .proc_write   = gpa_write,
};

static int __init mod_init(void)
{
    proc_entry = proc_create(PROC_NAME, 0666, NULL, &gpa_fops);
    if (!proc_entry)
        return -ENOMEM;

    pr_info("gpa_hpa module loaded\n");
    return 0;
}

static void __exit mod_exit(void)
{
    proc_remove(proc_entry);
    pr_info("gpa_hpa module unloaded\n");
}

module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
