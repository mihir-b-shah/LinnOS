
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/namei.h>

// Function prototypes
static int __init lake_fv_init(void);
static void __exit lake_fv_exit(void);

static int __init lake_fv_init(void) {
    // TODO: kernel trampolines

    printk(KERN_INFO "lake_fv: module loaded\n");
    return 0;
}

static void __exit lake_fv_exit(void) {
    // TODO: cleanup
    printk(KERN_INFO "lake_fv: module unloaded\n");
}

// Register the module initialization and exit functions
module_init(lake_fv_init);
module_exit(lake_fv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fstore");
MODULE_AUTHOR("fat rat");
