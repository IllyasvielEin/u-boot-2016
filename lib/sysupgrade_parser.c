#include <common.h>
#include <command.h>
#include <sysupgrade_parser.h>
#include <ipq_api.h>
#include <asm/arch-qca-common/smem.h>
#ifdef CONFIG_CMD_NAND
#include <nand.h>
#endif
#ifdef CONFIG_IPQ40XX
#include <../board/qca/arm/common/fdt_info.h>
#endif
#ifdef CONFIG_CMD_UBI
#include <ubi_uboot.h>
#endif

static unsigned long tar_parse_size(const char *s, int len) {
	unsigned long val = 0;
	int i;
	for (i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
		val = val * 8 + (s[i] - '0');
	return val;
}

static int tar_is_kernel_name(const char *name) {
	return strstr(name, "kernel") != NULL ||
		strstr(name, "hlos") != NULL ||
		strstr(name, "fit") != NULL;
}

static int tar_is_rootfs_name(const char *name) {
	return strstr(name, "root") != NULL ||
		strstr(name, "squashfs") != NULL;
}

static sysupgrade_fw_parts parse_sysupgrade(void *address) {
	sysupgrade_fw_parts parts;
	uintptr_t base = (uintptr_t)address, pos, data;
	int header_off = (memcmp((void *)(base + 257), "ustar", 5) == 0) ? 0 : 10;
	int max_entries = 32, entry, i, j;
	char *hdr;
	unsigned long fsize, padded;
	const uint32_t MAGIC_HSQS = 0x73717368;
	uint8_t *sb;
	u64 bytes_used;

	memset(&parts, 0, sizeof(parts));

	if (memcmp((void *)(base + header_off + 257), "ustar", 5) == 0 ||
	    memcmp((void *)(base + 257), "ustar", 5) == 0) {
		pos = base + header_off;

		for (entry = 0; entry < max_entries; entry++) {
			hdr = (char *)pos;

			if (hdr[0] == 0)
				break;

			if (memcmp(hdr + 257, "ustar", 5) != 0)
				break;

			fsize = tar_parse_size(hdr + 124, 12);
			data = pos + 512;
			padded = (fsize + 511) & ~511UL;

			if (fsize > 0 && !parts.kernel_data && tar_is_kernel_name(hdr)) {
				parts.kernel_data = (const void *)data;
				parts.kernel_size = fsize;
			} else if (fsize > 0 && !parts.rootfs_data && tar_is_rootfs_name(hdr)) {
				parts.rootfs_data = (const void *)data;
				parts.rootfs_size = fsize;
			}

			pos = data + padded;
		}
	} else {
		for (i = header_off; i < 64 * 1024 * 1024; i += 4) {
			if (*(uint32_t *)(base + i) == MAGIC_HSQS) {
				parts.kernel_data = (const void *)(base + header_off);
				parts.kernel_size = i - header_off;
				parts.rootfs_data = (const void *)(base + i);
				sb = (uint8_t *)parts.rootfs_data;
				bytes_used = 0;
				for (j = 0; j < 8; j++)
					bytes_used |= (u64)sb[40 + j] << (j * 8);
				if (bytes_used > 0)
					parts.rootfs_size = (size_t)bytes_used;
				break;
			}
		}
	}

	sysupgrade_parts_valid(&parts) ?
		printf("kernel %lu, rootfs %lu\n",
				(unsigned long)parts.kernel_size, (unsigned long)parts.rootfs_size) :
		printf("parse failed\n");

	return parts;
}

sysupgrade_fw_parts parse_sysupgrade_firmware(void *address) {
	return parse_sysupgrade(address);
}

#ifdef CONFIG_CMD_UBI
int sysupgrade_ubi_init(void) {
	qca_smem_flash_info_t *sfi = &qca_smem_flash_info;

	if (sfi->rootfs.offset == 0xBAD0FF5E || !sfi->rootfs.size)
		return -1;

#ifdef IPQ_UBI_VOL_WRITE_SUPPORT
	if (sfi->flash_type == SMEM_BOOT_NAND_FLASH ||
		sfi->flash_type == SMEM_BOOT_QSPI_NAND_FLASH ||
		(sfi->flash_type == SMEM_BOOT_SPI_FLASH &&
			get_which_flash_param("rootfs") > 0)) {
		if (ubi_set_rootfs_part() == 0)
			return 0;
	}
#endif

	int nand_dev;
#ifdef CONFIG_IPQ40XX
	nand_dev = is_spi_nand_available();
#else
	nand_dev = CONFIG_NAND_FLASH_INFO_IDX;
#endif

	char cmd[256];
	char *msmparts = getenv("msmparts");
	snprintf(cmd, sizeof(cmd),
		 "nand device %d && "
		 "setenv mtdids nand%d=nand%d && "
		 "setenv mtdparts mtdparts=nand%d:0x%llx@0x%llx(fs)%s%s",
		 nand_dev, nand_dev, nand_dev, nand_dev, sfi->rootfs.size,
		 sfi->rootfs.offset, msmparts ? "," : "", msmparts ? msmparts : "");

	return run_command(cmd, 0);
}

/*
 * Volume operations run on the device attached by "ubi part fs"; the name
 * lookup does not attach again.  An existing volume must be removed
 * successfully, a missing one is simply created.  "ubi create" returns a
 * command status, so a failed create stops the following write.
 */
static int ubi_write_volume(const char *name, const void *data, size_t size) {
	char buf[256];

	if (ubi_get_volume_id((char *)name) >= 0) {
		snprintf(buf, sizeof(buf), "ubi remove %s", name);
		if (run_command(buf, 0) != 0) {
			printf("%s remove failed\n", name);
			return -1;
		}
	}

	snprintf(buf, sizeof(buf),
		 "ubi create %s 0x%llx && "
		 "ubi write 0x%lx %s 0x%llx",
		 name, (u64)size,
		 (unsigned long)data, name,
		 (u64)size);

	if (run_command(buf, 0) != 0) {
		printf("%s write failed\n", name);
		return -1;
	}
	return 0;
}

#ifdef CONFIG_IPQ9574_XIAOMI_BE10000
/* RC01 keeps kernel/rootfs/rootfs_data at the fixed volume IDs 0/1/2. */
static int rc01_check_volume_ids(void) {
	static const char *const names[] = { "kernel", "rootfs", "rootfs_data" };
	int i, id;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		id = ubi_get_volume_id((char *)names[i]);
		if (id != i) {
			printf("UBI volume %s has ID %d, expected %d\n",
			       names[i], id, i);
			return -1;
		}
	}
	return 0;
}
#endif

int sysupgrade_write_ubi_volumes(sysupgrade_fw_parts *parts, int backup_enabled) {
#ifdef CONFIG_IPQ9574_XIAOMI_BE10000
	/*
	 * Backup volumes would be created with automatic IDs and could take
	 * the fixed rootfs_data ID 2.  Reject before any UBI attach or change.
	 */
	if (backup_enabled) {
		printf("SYSUPGRADE backup volumes are not supported on this board, nothing written\n");
		return -1;
	}
#endif

	if (sysupgrade_ubi_init() != 0) {
		printf("UBI init failed\n");
		return -1;
	}

	if (run_command("ubi part fs", 0) != 0) {
		printf("UBI attach failed\n");
		return -1;
	}

	if (ubi_get_volume_id("rootfs_data") >= 0 &&
	    run_command("ubi remove rootfs_data", 0) != 0) {
		printf("rootfs_data remove failed\n");
		return -1;
	}

	if (parts->kernel_data && parts->kernel_size > 0 &&
	    ubi_write_volume("kernel", parts->kernel_data, parts->kernel_size) != 0)
		return -1;

	if (parts->rootfs_data && parts->rootfs_size > 0 &&
	    ubi_write_volume("rootfs", parts->rootfs_data, parts->rootfs_size) != 0)
		return -1;

	/* Selected backup volumes are required; no rollback of the above. */
	if (backup_enabled) {
		if (parts->kernel_data && parts->kernel_size > 0 &&
		    ubi_write_volume("kernel_1", parts->kernel_data, parts->kernel_size) != 0)
			return -1;
		if (parts->rootfs_data && parts->rootfs_size > 0 &&
		    ubi_write_volume("rootfs_1", parts->rootfs_data, parts->rootfs_size) != 0)
			return -1;
	}

	if (run_command("ubi create rootfs_data", 0) != 0) {
		printf("rootfs_data create failed\n");
		return -1;
	}

#ifdef CONFIG_IPQ9574_XIAOMI_BE10000
	if (rc01_check_volume_ids() != 0)
		return -1;
#endif

	return 0;
}
#endif