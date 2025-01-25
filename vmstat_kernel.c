#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/time.h>
#include <linux/sysinfo.h>
#include <linux/blkdev.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/sched/stat.h>
#include <linux/mm_types.h>
#include <linux/atomic.h>
#include <linux/vm_event_item.h>
#include <linux/sched/signal.h>
#include <linux/rcupdate.h>

#define DEVICE_NAME "vmstat_dev"
#define VMSTAT_GET_DATA _IOR('v', 1, struct vmstat_data)

struct vmstat_data {
    unsigned long procs_running;
    unsigned long procs_blocked;
    
    unsigned long mem_swpd;
    unsigned long mem_free;
    unsigned long mem_buff;
    unsigned long mem_cache;
    
    unsigned long swap_si;
    unsigned long swap_so;
    
    unsigned long io_bi;
    unsigned long io_bo;
    
    unsigned long system_in;
    unsigned long system_cs;
    
    unsigned long cpu_us;
    unsigned long cpu_sy;
    unsigned long cpu_id;
    unsigned long cpu_wa;
    unsigned long cpu_st;
};

static dev_t dev;
static struct cdev cdev;
static struct class *vmstat_class;

static void get_cpu_usage(u64 *user, u64 *nice, u64 *system, u64 *idle,
                         u64 *iowait, u64 *irq, u64 *softirq, u64 *steal);

static long vmstat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vmstat_data data;
    struct sysinfo si;
    u64 user, nice, system, idle, iowait, irq, softirq, steal;
    u64 total;
    int cpu;
    struct vm_event_state *this_state;
    struct task_struct *p;
    
    if (cmd != VMSTAT_GET_DATA)
        return -EINVAL;

    si_meminfo(&si);
    
    data.procs_running = 0;
    data.procs_blocked = 0;
    rcu_read_lock();
    for_each_process(p) {
        long task_state = READ_ONCE(p->__state);
        if (task_state == TASK_RUNNING)
            data.procs_running++;
        if (task_state & TASK_UNINTERRUPTIBLE)
            data.procs_blocked++;
    }
    rcu_read_unlock();

    data.mem_free = si.freeram << (PAGE_SHIFT - 10);
    data.mem_buff = si.bufferram << (PAGE_SHIFT - 10);
    data.mem_cache = global_node_page_state(NR_FILE_PAGES) << (PAGE_SHIFT - 10);
    data.mem_swpd = si.totalswap - si.freeswap;

    this_state = &per_cpu(vm_event_states, 0);
    data.swap_si = this_state->event[PSWPIN] >> 1;
    data.swap_so = this_state->event[PSWPOUT] >> 1;
    data.io_bi = this_state->event[PGPGIN] >> 1;
    data.io_bo = this_state->event[PGPGOUT] >> 1;

    for_each_online_cpu(cpu) {
        if (cpu == 0) continue;
        this_state = &per_cpu(vm_event_states, cpu);
        data.swap_si += this_state->event[PSWPIN] >> 1;
        data.swap_so += this_state->event[PSWPOUT] >> 1;
        data.io_bi += this_state->event[PGPGIN] >> 1;
        data.io_bo += this_state->event[PGPGOUT] >> 1;
    }

    unsigned long uptime = jiffies_to_msecs(jiffies) / 1000;
    if (uptime > 0) {
        data.swap_si /= uptime;
        data.swap_so /= uptime;
        data.io_bi /= uptime;
        data.io_bo /= uptime;
    }

    data.system_in = 0;
    data.system_cs = 0;
    
    for_each_possible_cpu(cpu) {
        unsigned long irqs = kstat_cpu_irqs_sum(cpu);
        data.system_in += irqs;
        data.system_cs += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
    }
    
    if (uptime > 0) {
        data.system_in /= uptime;
        data.system_cs /= uptime;
    }

    get_cpu_usage(&user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    total = user + nice + system + idle + iowait + irq + softirq + steal;
    
    if (total > 0) {
        data.cpu_us = 100 * (user + nice) / total;
        data.cpu_sy = 100 * (system + irq + softirq) / total;
        data.cpu_id = 100 * idle / total;
        data.cpu_wa = 100 * iowait / total;
        data.cpu_st = 100 * steal / total;
    } else {
        data.cpu_us = data.cpu_sy = data.cpu_id = data.cpu_wa = data.cpu_st = 0;
    }

    if (copy_to_user((struct vmstat_data *)arg, &data, sizeof(data)))
        return -EFAULT;

    return 0;
}

static void get_cpu_usage(u64 *user, u64 *nice, u64 *system, u64 *idle,
                         u64 *iowait, u64 *irq, u64 *softirq, u64 *steal)
{
    int i;
    struct kernel_cpustat kcpustat;
    
    *user = *nice = *system = *idle = *iowait = *irq = *softirq = *steal = 0;

    for_each_possible_cpu(i) {
        kcpustat = kcpustat_cpu(i);
        *user += kcpustat.cpustat[CPUTIME_USER];
        *nice += kcpustat.cpustat[CPUTIME_NICE];
        *system += kcpustat.cpustat[CPUTIME_SYSTEM];
        *idle += kcpustat.cpustat[CPUTIME_IDLE];
        *iowait += kcpustat.cpustat[CPUTIME_IOWAIT];
        *irq += kcpustat.cpustat[CPUTIME_IRQ];
        *softirq += kcpustat.cpustat[CPUTIME_SOFTIRQ];
        *steal += kcpustat.cpustat[CPUTIME_STEAL];
    }
}

static struct file_operations vmstat_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = vmstat_ioctl,
};

static int __init vmstat_init(void)
{
    if (alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME) < 0)
        return -1;
    
    vmstat_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(vmstat_class)) {
        unregister_chrdev_region(dev, 1);
        return PTR_ERR(vmstat_class);
    }
    
    if (device_create(vmstat_class, NULL, dev, NULL, DEVICE_NAME) == NULL) {
        class_destroy(vmstat_class);
        unregister_chrdev_region(dev, 1);
        return -1;
    }
    
    cdev_init(&cdev, &vmstat_fops);
    if (cdev_add(&cdev, dev, 1) < 0) {
        device_destroy(vmstat_class, dev);
        class_destroy(vmstat_class);
        unregister_chrdev_region(dev, 1);
        return -1;
    }
    
    return 0;
}

static void __exit vmstat_exit(void)
{
    cdev_del(&cdev);
    device_destroy(vmstat_class, dev);
    class_destroy(vmstat_class);
    unregister_chrdev_region(dev, 1);
}

module_init(vmstat_init);
module_exit(vmstat_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nikita");
MODULE_DESCRIPTION("VMStat kernel module");

