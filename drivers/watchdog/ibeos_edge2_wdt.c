#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/watchdog.h>
#include <asm/io.h>

#include <linux/ibeos_edge2.h>

#define DEFAULT_TIMEOUT 30
#define MIN_TIMEOUT 2
#define MAX_TIMEOUT 1024

#define WDT_REG_PET 0x40
#define WDT_REG_TIMEOUT 0x44
#define WDT_PET_VALUE 0x94f81f14

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
                 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct ibeos_edge2_wdt {
    struct watchdog_device wdt;
    void __iomem *bar;
};

static struct ibeos_edge2_wdt iew;

static const struct watchdog_info ibeos_edge2_wdt_info = {
    .options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
    .identity = "Ibeos Edge-1100 Watchdog",
};

static void ibeos_edge2_wdt_pet(void)
{
    iowrite32(WDT_PET_VALUE, iew.bar+WDT_REG_PET);
}

static void ibeos_edge2_wdt_change(uint32_t seconds)
{
    iowrite32(seconds, iew.bar+WDT_REG_TIMEOUT);
    ibeos_edge2_wdt_pet();
}

static int ibeos_edge2_wdt_start(struct watchdog_device *wdd)
{
    //struct ibeos_edge2_wdt *iew = container_of(wdd, struct ibeos_edge2_wdt, wdt);
    ibeos_edge2_wdt_change(wdd->timeout);

    return 0;
}

static int ibeos_edge2_wdt_stop(struct watchdog_device *wdd)
{
    ibeos_edge2_wdt_change(0);

    return 0;
}

static int ibeos_edge2_wdt_ping(struct watchdog_device *wdd)
{
    ibeos_edge2_wdt_pet();

    return 0;
}

static int ibeos_edge2_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
    wdd->timeout = timeout;
    ibeos_edge2_wdt_change(timeout);

    return 0;
}

static const struct watchdog_ops ibeos_edge2_wdt_ops = {
    .start = ibeos_edge2_wdt_start,
    .stop = ibeos_edge2_wdt_stop,
    .set_timeout = ibeos_edge2_wdt_set_timeout,
    .ping = ibeos_edge2_wdt_ping,
};

static int __init init_edge2_wdt(void)
{
    int ret;

    pr_info("initializing ibeos edge-1100 watchdog driver\n");

    memset(&iew, 0, sizeof(iew));

    iew.bar = ibeos_edge2_get_bar(0, 2);
    if(!iew.bar) return -ENODEV;

    iew.wdt.info = &ibeos_edge2_wdt_info;
    iew.wdt.ops = &ibeos_edge2_wdt_ops;
    iew.wdt.timeout = DEFAULT_TIMEOUT;
    iew.wdt.min_timeout = MIN_TIMEOUT;
    iew.wdt.max_timeout = MAX_TIMEOUT;

    watchdog_set_drvdata(&iew.wdt, &iew);
    watchdog_set_nowayout(&iew.wdt, nowayout);

    ret = watchdog_register_device(&iew.wdt);
    if(ret) return ret;

    pr_info("initialized ibeos edge-1100 watchdog driver\n");
    return 0;
}

static void __exit exit_edge2_wdt(void)
{
    watchdog_unregister_device(&iew.wdt);
}

module_init(init_edge2_wdt);
module_exit(exit_edge2_wdt);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Steven Seeger");
MODULE_DESCRIPTION("Watchdog driver for Ibeos Edge-1100");
