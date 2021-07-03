#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>


static int generic_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
    return 0;
}
 
static void generic_post_handler(struct kprobe *p, struct pt_regs *regs,
                                 unsigned long flags)
{

}
 
static struct kprobe kp = {
        .symbol_name = "security_file_mprotect",
        .pre_handler = generic_pre_handler,
        .post_handler = generic_post_handler,
};

static int generic_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    return 0;
}
 
static int generic_ent_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    return 0;
}
 
struct kretprobe kr[] = {
    {
        .kp.symbol_name = "security_file_mprotect",
        .data_size = 16,
        .handler = generic_ret_handler,
        .entry_handler = generic_ent_handler,
        /* .maxactive = 32, */
    },
    {
        .kp.symbol_name = "ext4_file_write_iter",
        .data_size = 16,
        .handler = generic_ret_handler,
        .entry_handler = generic_ent_handler,
        /* .maxactive = 32, */
    },
};

struct kretprobe *krs[] = {&kr[0], &kr[1]};

static int reg_kprobe = 0;
static int reg_kretprobe = 1;
static int krp_insts = 0;

module_param(krp_insts, int, S_IRUSR|S_IRGRP|S_IROTH);
module_param(reg_kprobe, int, S_IRUSR|S_IRGRP|S_IROTH);
module_param(reg_kretprobe, int, S_IRUSR|S_IRGRP|S_IROTH);

static int __init kprobe_init(void)
{
    int ret;

    if (reg_kprobe) {
        ret = register_kprobe(&kp);
        if (ret < 0) {
            printk(KERN_DEBUG "register_kprobe failed, returned %d\n", ret);
            return ret;
        }
        printk(KERN_DEBUG "Planted kprobe at %p\n", kp.addr);
    }
    if (reg_kretprobe) {
        kr[0].maxactive = kr[1].maxactive = krp_insts;
        ret = register_kretprobes(krs, 2);
        if (ret < 0) {
            printk(KERN_DEBUG "register_kretprobe failed, returned %d\n", ret);
            return ret;
        }
        printk(KERN_DEBUG "Planted kretprobe registered.\n");
    }
    return 0;
}

static void __exit kprobe_exit(void)
{
    if (reg_kprobe) {
       unregister_kprobe(&kp);
       printk(KERN_DEBUG "kprobe at %p unregistered\n", kp.addr);
    }

    if (reg_kretprobe) {
        unregister_kretprobes(krs, 2);
        printk(KERN_DEBUG "kretprobe unregistered\n");
    }
}

module_init(kprobe_init)
module_exit(kprobe_exit)
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matt Wu");
