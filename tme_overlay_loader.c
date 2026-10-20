// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/firmware.h>

static int overlay_id;
static int __init tme_overlay_init(void)
{
    const struct firmware *firmware;
    int rc;
    if (!of_machine_is_compatible("ayn,thor")) return -ENODEV;
    rc = request_firmware_direct(&firmware, "aynthor/tme-overlay.dtbo", NULL);
    if (rc) return rc;
    rc = of_overlay_fdt_apply(firmware->data, firmware->size, &overlay_id, NULL);
    release_firmware(firmware);
    return rc;
}
static void __exit tme_overlay_exit(void)
{
    of_overlay_remove(&overlay_id);
}
module_init(tme_overlay_init);
module_exit(tme_overlay_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Thor experimental TME-only device-tree overlay");
