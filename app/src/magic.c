/*
 * Copyright (c) 2023 Jamie M.
 *
 * All right reserved. This code is not apache or FOSS/copyleft licensed.
 */

#include <zephyr/linker/sections.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/device.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

Z_GENERIC_SECTION(.fake_boot_string) __attribute__((used))
const uint8_t boot_string[] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const struct device *flash_device =
		DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_flash_controller));

extern const uint32_t _fake_boot_string_start[];

static int fw_bootloader_handler(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	uint8_t data[] = {0x00, 0x00, 0x00, 0x00};

	rc = flash_write(flash_device, (off_t)_fake_boot_string_start, data, sizeof(data));

	if (rc == 0) {
		shell_print(sh, "Going to bootloader...");
		k_sleep(K_MSEC(300));
		sys_reboot(SYS_REBOOT_COLD);
	} else {
		shell_error(sh, "Failed to erase fake boot string: %d", rc);
	}

	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fw_cmd,
	/* Command handlers */
	SHELL_CMD(bootloader, NULL, "Go to bootloader", fw_bootloader_handler),

	/* Array terminator. */
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fw, &fw_cmd, "Firmware commands", NULL);
