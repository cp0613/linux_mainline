#include <linux/module.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>

static unsigned int irq = 13;
module_param(irq, uint, 0664);
MODULE_PARM_DESC(irq, "irq");

static void trigger_irq(void)
{
    struct irq_desc *desc = irq_to_desc(irq);
    if (desc && desc->handle_irq) {
        local_irq_disable();
        desc->handle_irq(desc);
        local_irq_enable();
    }
}

static int __init irq_trigger_init(void)
{
    trigger_irq();

    return 0;
}

static void __exit irq_trigger_exit(void)
{
    return;
}

module_init(irq_trigger_init);
module_exit(irq_trigger_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("irq_trigger");
