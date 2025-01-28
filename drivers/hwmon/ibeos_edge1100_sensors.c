#include <linux/module.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/math.h>
#include <linux/slab.h>
#include <asm/io.h>

#include <linux/ibeos_edge1100.h>

/* channel map
 * ADC 0 - 7
 * 0: 2.5V reference (raw voltage reading)
 * 1: ISOC (reserved)
 * 2: VDD_SOC
 * 3: VPX_CH3 GP channel available on P2 and XMC
 * 4: VPX_CH4 GP channel available on P2 and XMC
 * 5: TS_R380 temperature sensor
 * 6: TS_R379 temperature sensor
 * 7: TS_R378 temperature sensor
 *
 * NOTE: channel 0 is used to correct readings on channels 1 - 4
 */

#define ADC_VREG(x) (0x80+(x*4))
#define ADC_TREG(x) (0x94+(x*4))
#define ADC_REF_MV 2500
//#define TEMP_CONST_R 49900
//#define TEMP_CONST_C 590040
#define TEMP_VALID_MAX 4049
#define TEMP_VALID_MIN 70
#define TEMP_START 150000

/* first entry is 150degC, last is -55degC. each entry subtracts 1degC */
static const uint16_t temp_entries[] = {
    70, 71, 73, 75, 77, 78, 80, 82, 84, 86, 88, 90, 93, 95, 97, 100,
    102, 105, 107, 110, 113, 116, 118, 121, 125, 128, 131, 134, 138, 142, 145, 149,
    153, 157, 161, 165, 170, 174, 179, 184, 189, 194, 199, 205, 210, 216, 222, 228,
    235, 241, 248, 255, 262, 269, 277, 285, 293, 301, 310, 319, 328, 337, 347, 357,
    368, 378, 389, 401, 412, 424, 437, 450, 463, 477, 491, 505, 520, 535, 551, 568,
    584, 602, 620, 638, 657, 676, 696, 717, 738, 760, 782, 805, 829, 853, 878, 904,
    930, 957, 985, 1013, 1042, 1072, 1103, 1134, 1166, 1198, 1232, 1266, 1300, 1336, 1372, 1409,
    1446, 1484, 1523, 1563, 1603, 1643, 1684, 1726, 1768, 1811, 1854, 1898, 1942, 1986, 2031, 2076,
    2121, 2166, 2212, 2258, 2303, 2349, 2395, 2440, 2486, 2531, 2576, 2621, 2665, 2709, 2753, 2796,
    2839, 2881, 2923, 2964, 3004, 3044, 3082, 3121, 3158, 3195, 3231, 3265, 3300, 3333, 3365, 3397,
    3427, 3457, 3486, 3514, 3541, 3567, 3592, 3616, 3640, 3662, 3684, 3705, 3725, 3744, 3763, 3780,
    3797, 3813, 3829, 3844, 3858, 3871, 3884, 3896, 3908, 3919, 3929, 3939, 3949, 3958, 3966, 3974,
    3982, 3989, 3996, 4002, 4008, 4014, 4019, 4024, 4029, 4034, 4038, 4042, 4045, 4049,
    4050 /* NOTE: 4050 is out of range, but it allows the algorithm to accept 4049 */
};

struct edge2_hwmon_state {
    struct device *hwmon_dev;
    struct device *parent;
    time64_t last_ref_reading;
    uint32_t ref_2p5;
    void __iomem *bar;
};

static struct edge2_hwmon_state *single; /* for cleanup */

static const struct hwmon_channel_info * const edge2_sensors_info[] = {
    HWMON_CHANNEL_INFO(in,
                       HWMON_I_INPUT,
                       HWMON_I_INPUT,
                       HWMON_I_INPUT,
                       HWMON_I_INPUT,
                       HWMON_I_INPUT),
    HWMON_CHANNEL_INFO(temp,
                       HWMON_T_INPUT,
                       HWMON_T_INPUT,
                       HWMON_T_INPUT),
    NULL
};

static umode_t edge2_sensors_is_visible(const void *_data, enum hwmon_sensor_types type, u32 attr, int channel)
{
    return 0444;
}

static int edge2_sensors_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
    struct edge2_hwmon_state * const ehs = dev_get_drvdata(dev);
    const time64_t now = ktime_get_seconds();
    uint32_t temp=0;

    //dev_info(ehs->parent, "read ch %d type %d\n", channel, type);
    if(ehs->last_ref_reading!=now) {
        /* by doing an equality check, rollover won't matter */
        /* there's a race condition here, in that two threads might do this at the same time -- doesn't matter */
        /* update 2.5V reference for calibration */

        ehs->ref_2p5 = ioread32(ehs->bar + ADC_VREG(0));
        ehs->last_ref_reading = now;
    }

    if(type==hwmon_in) {
        /* voltages */
        if(channel>4) return -EOPNOTSUPP;

        temp = ioread32(ehs->bar + ADC_VREG(channel));

        if(!ehs->ref_2p5) dev_warn_once(ehs->parent, "edge2_sensors: 2p5 is zero\n");

        if(channel&&ehs->ref_2p5) {
            /* for channels 1-4, calculate calibrated value */
            temp = DIV_ROUND_CLOSEST((ADC_REF_MV * temp), ehs->ref_2p5);
        }

        *val = temp;
    }
    else {
        /* temperatures */
        const int sz = (sizeof(temp_entries)/sizeof(*temp_entries));
        int i=0;

        if(channel>2) return -EOPNOTSUPP;

        temp = ioread32(ehs->bar + ADC_TREG(channel));

        if((temp<TEMP_VALID_MIN)||(temp>TEMP_VALID_MAX)) {
            dev_warn_once(ehs->parent, "edge2_sensors: temperature channel %d (value %d) is out of range)\n", channel, temp);
            return -EIO;
        }

        for(; i<(sz-1); ++i) {
            if((temp>=temp_entries[i])&&(temp<temp_entries[i+1])) {
                /* found a valid point in the table */
                const uint16_t l = temp_entries[i];
                const uint16_t r = temp_entries[i+1];
                if(temp>=l) {
                    /* found a valid point in the table */
                    const uint16_t diff_measured = temp-l;
                    const uint16_t diff_table = r-l;
                    const int32_t l_temp = TEMP_START-(i*1000); /* degrees mC */

                    if(!diff_measured) *val = l_temp;
                    else *val = l_temp - ((1000/diff_table)*diff_measured);
                    break;
                }

                break;
            }
        }
        
        BUG_ON(i==(sz-1)); /* should not happen due to range check above */
    }

    return 0;
}

static const struct hwmon_ops edge2_sensors_ops = {
    .is_visible = edge2_sensors_is_visible,
    .read = edge2_sensors_read,
};

static const struct hwmon_chip_info edge2_sensors_chip_info = {
    .ops = &edge2_sensors_ops,
    .info = edge2_sensors_info
};

static int __init init_edge2_hwmon(void)
{
    struct edge2_hwmon_state *ehs;
    struct device *dev;
    int ret = 0;

    if(single) return -EBUSY; /* already installed? */

    dev = ibeos_edge2_get_dev(0);
    if(!dev) return -ENODEV;

    ehs = kzalloc(sizeof(*ehs), GFP_KERNEL);
    if(!ehs) return -ENOMEM;

    ehs->parent = dev;

    ehs->bar = ibeos_edge2_get_bar(0, 2);
    if(!ehs->bar) return -ENODEV;

    ehs->hwmon_dev = hwmon_device_register_with_info(dev, "edge2_sensors", ehs, &edge2_sensors_chip_info, NULL);
    if(IS_ERR(ehs->hwmon_dev)) {
        ret = PTR_ERR(ehs->hwmon_dev);
        goto err_alloc;
    }

    single = ehs;
    return 0;

err_alloc:
    kfree(ehs);

    return ret;
}

static void __exit exit_edge2_hwmon(void)
{
    BUG_ON(!single);

    hwmon_device_unregister(single->hwmon_dev);
    kfree(single);
    single = NULL;
}

module_init(init_edge2_hwmon);
module_exit(exit_edge2_hwmon);

MODULE_AUTHOR("Steven Seeger <steven@efsi.com>");
MODULE_DESCRIPTION("Ibeos Edge2 hwmon driver");
MODULE_LICENSE("GPL v2");
