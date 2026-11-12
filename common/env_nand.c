/*
 * (C) Copyright 2000-2010
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2008
 * Stuart Wood, Lab X Technologies <stuart.wood@labxtechnologies.com>
 *
 * (C) Copyright 2004
 * Jian Zhang, Texas Instruments, jzhang@ti.com.
 *
 * (C) Copyright 2001 Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Andreas Heppel <aheppel@sysgo.de>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <environment.h>
#include <linux/stddef.h>
#include <malloc.h>
#include <memalign.h>
#include <nand.h>
#include <search.h>
#include <errno.h>

#if defined(CONFIG_CMD_SAVEENV) && defined(CONFIG_CMD_NAND)
#define CMD_SAVEENV
#elif defined(CONFIG_ENV_OFFSET_REDUND)
#error CONFIG_ENV_OFFSET_REDUND must have CONFIG_CMD_SAVEENV & CONFIG_CMD_NAND
#endif

#if defined(CONFIG_ENV_SIZE_REDUND) &&	\
	(CONFIG_ENV_SIZE_REDUND != CONFIG_ENV_SIZE)
#error CONFIG_ENV_SIZE_REDUND should be the same as CONFIG_ENV_SIZE
#endif

#ifndef CONFIG_ENV_RANGE
#define CONFIG_ENV_RANGE	CONFIG_ENV_SIZE
#endif

char *nand_env_name_spec = "NAND";

#if defined(ENV_IS_EMBEDDED)
env_t *nand_env_ptr = &environment;
#elif defined(CONFIG_NAND_ENV_DST)
env_t *nand_env_ptr = (env_t *)CONFIG_NAND_ENV_DST;
#else /* ! ENV_IS_EMBEDDED */
env_t *nand_env_ptr;
#endif /* ENV_IS_EMBEDDED */

DECLARE_GLOBAL_DATA_PTR;

int nand_env_device = 0;
/*
 * This is called before nand_init() so we can't read NAND to
 * validate env data.
 *
 * Mark it OK for now. env_relocate() in env_common.c will call our
 * relocate function which does the real validation.
 *
 * When using a NAND boot image (like sequoia_nand), the environment
 * can be embedded or attached to the U-Boot image in NAND flash.
 * This way the SPL loads not only the U-Boot image from NAND but
 * also the environment.
 */
int nand_env_init(void)
{
#if defined(ENV_IS_EMBEDDED) || defined(CONFIG_NAND_ENV_DST)
	int crc1_ok = 0, crc2_ok = 0;
	env_t *tmp_env1;

#ifdef CONFIG_ENV_OFFSET_REDUND
	env_t *tmp_env2;

	tmp_env2 = (env_t *)((ulong)env_ptr + CONFIG_ENV_SIZE);
	crc2_ok = crc32(0, tmp_env2->data, ENV_SIZE) == tmp_env2->crc;
#endif
	tmp_env1 = env_ptr;
	crc1_ok = crc32(0, tmp_env1->data, ENV_SIZE) == tmp_env1->crc;

	if (!crc1_ok && !crc2_ok) {
		gd->env_addr	= 0;
		gd->env_valid	= 0;

		return 0;
	} else if (crc1_ok && !crc2_ok) {
		gd->env_valid = 1;
	}
#ifdef CONFIG_ENV_OFFSET_REDUND
	else if (!crc1_ok && crc2_ok) {
		gd->env_valid = 2;
	} else {
		/* both ok - check serial */
		if (tmp_env1->flags == 255 && tmp_env2->flags == 0)
			gd->env_valid = 2;
		else if (tmp_env2->flags == 255 && tmp_env1->flags == 0)
			gd->env_valid = 1;
		else if (tmp_env1->flags > tmp_env2->flags)
			gd->env_valid = 1;
		else if (tmp_env2->flags > tmp_env1->flags)
			gd->env_valid = 2;
		else /* flags are equal - almost impossible */
			gd->env_valid = 1;
	}

	if (gd->env_valid == 2)
		env_ptr = tmp_env2;
	else
#endif
	if (gd->env_valid == 1)
		env_ptr = tmp_env1;

	gd->env_addr = (ulong)env_ptr->data;

#else /* ENV_IS_EMBEDDED || CONFIG_NAND_ENV_DST */
	gd->env_addr	= (ulong)&default_environment[0];
	gd->env_valid	= 1;
#endif /* ENV_IS_EMBEDDED || CONFIG_NAND_ENV_DST */

	return 0;
}

#if defined(CMD_SAVEENV) || !defined(CONFIG_SPL_BUILD)
/*
 * Transfer the CONFIG_ENV_RANGE bytes of the environment format to or from
 * the physical ENV area [offset, offset + span) of the ENV NAND device.
 * The data is spread over the good erase blocks in order, each block holds
 * min(remaining, erasesize) bytes from its start, known bad blocks are
 * skipped.  Saving and loading use this same distribution.
 *
 * Every query and transfer stays inside the area (and the device).  A
 * failing bad block query, a transfer error, a short or zero transfer or
 * running out of good blocks fails.  A corrected read (-EUCLEAN) of the
 * full length is accepted; callers that need it compare the content.
 *
 * Returns 0 on success, 1 on failure.
 */
static int env_nand_xfer(loff_t offset, loff_t span, u_char *buf, int write)
{
	nand_info_t *nand = &nand_info[nand_env_device];
	size_t total = CONFIG_ENV_RANGE;
	size_t done = 0, len, req;
	loff_t end, blocksize = nand->erasesize;
	int ret;

	if (!blocksize || (blocksize & (blocksize - 1)) || !total ||
	    total > CONFIG_ENV_SIZE || offset < 0 || span <= 0 ||
	    (offset & (blocksize - 1)) || nand->size <= 0 ||
	    offset >= nand->size || span > nand->size - offset) {
		puts("Invalid NAND environment area\n");
		return 1;
	}
	end = offset + span;

	while (done < total) {
		if (offset >= end || blocksize > end - offset) {
			puts("No good block left in the environment area\n");
			return 1;
		}

		ret = nand_block_isbad(nand, offset);
		if (ret < 0) {
			printf("Bad block query at 0x%08llx failed %d\n",
			       (unsigned long long)offset, ret);
			return 1;
		}
		if (ret) {
			offset += blocksize;
			continue;
		}

		len = min(total - done, (size_t)blocksize);
		req = len;
		if (write) {
			ret = nand_write(nand, offset, &len, &buf[done]);
		} else {
			ret = nand_read(nand, offset, &len, &buf[done]);
			if (ret == -EUCLEAN)
				ret = 0;
		}
		if (ret || len != req) {
			printf("NAND %s at 0x%08llx failed %d (0x%zx of 0x%zx)\n",
			       write ? "write" : "read",
			       (unsigned long long)offset, ret, len, req);
			return 1;
		}

		done += len;
		offset += blocksize;
	}

	return 0;
}
#endif

#ifdef CMD_SAVEENV
/*
 * The legacy NAND code saved the environment in the first NAND device i.e.,
 * nand_dev_desc + 0. This is also the behaviour using the new NAND code.
 */
static int writeenv(loff_t offset, loff_t span, u_char *buf)
{
	return env_nand_xfer(offset, span, buf, 1);
}

struct env_location {
	const char *name;
	nand_erase_options_t erase_opts;
};

/*
 * Check the whole area that is going to be erased before erasing anything,
 * write the exported environment and read it back into a separate buffer
 * the same way it is loaded.  Success (and "OK") only after the complete
 * format range including the CRC compares equal.  There is a single copy:
 * a failure after the erase leaves the area partially written.
 */
static int erase_and_write_env(const struct env_location *location,
		u_char *env_new, u_char *env_check)
{
	nand_info_t *nand = &nand_info[nand_env_device];
	const nand_erase_options_t *opts = &location->erase_opts;
	loff_t blocksize = nand->erasesize, span = opts->length, padding;
	int ret;

	/* The physical area is a limit, never a length to round up. */
	if (!blocksize || (blocksize & (blocksize - 1)) ||
	    board_env_size <= 0 || (board_env_size & (blocksize - 1)) ||
	    span <= 0 || span > board_env_size) {
		puts("Invalid physical NAND environment area, nothing erased\n");
		return 1;
	}
	/* A redundant copy's format-sized erase must fit its physical area. */
	padding = span & (blocksize - 1);
	if (padding) {
		padding = blocksize - padding;
		if (padding > board_env_size - span) {
			puts("NAND environment erase exceeds its area, nothing erased\n");
			return 1;
		}
		span += padding;
	}

	ret = nand_check_range_capacity(nand, opts->offset, span,
					CONFIG_ENV_RANGE);
	if (ret) {
		printf("Error: %s environment area check failed (%d), "
		       "nothing erased\n", location->name, ret);
		return 1;
	}

	printf("Erasing %s...\n", location->name);
	if (nand_erase_opts(nand, opts))
		return 1;

	printf("Writing to %s... ", location->name);
	ret = writeenv(opts->offset, span, env_new);
	if (!ret) {
		ret = env_nand_xfer(opts->offset, span, env_check, 0);
		if (!ret && memcmp(env_new, env_check, CONFIG_ENV_RANGE)) {
			puts("read back mismatch ");
			ret = 1;
		}
	}
	puts(ret ? "FAILED!\n" : "OK\n");

	return ret;
}

#ifdef CONFIG_ENV_OFFSET_REDUND
static unsigned char env_flags;
#endif

int nand_saveenv(void)
{
	int	ret = 0;
	int	env_idx = 0;
	struct env_location location[] = {
		{
			.name = "NAND",
			.erase_opts = {
				.length = board_env_size,
				.offset = CONFIG_ENV_OFFSET,
			},
		},
#ifdef CONFIG_ENV_OFFSET_REDUND
		{
			.name = "redundant NAND",
			.erase_opts = {
				.length = CONFIG_ENV_RANGE,
				.offset = CONFIG_ENV_OFFSET_REDUND,
			},
		},
#endif
	};

	env_t *env_new;
	u_char *env_check;

	if (!CONFIG_ENV_RANGE || CONFIG_ENV_RANGE > board_env_size ||
	    CONFIG_ENV_RANGE > CONFIG_ENV_SIZE)
		return 1;

	/* All buffers are allocated before anything is erased. */
	env_new = (env_t *)memalign(ARCH_DMA_MINALIGN, CONFIG_ENV_SIZE);
	if (!env_new) {
		printf("Error: Cannot allocate %d bytes\n", CONFIG_ENV_SIZE);
		return 1;
	}

	env_check = (u_char *)memalign(ARCH_DMA_MINALIGN, CONFIG_ENV_SIZE);
	if (!env_check) {
		printf("Error: Cannot allocate %d bytes\n", CONFIG_ENV_SIZE);
		free(env_new);
		return 1;
	}

	ret = env_export(env_new);
	if (ret) {
		free(env_check);
		free(env_new);
		return ret;
	}

#ifdef CONFIG_ENV_OFFSET_REDUND
	env_new->flags = ++env_flags; /* increase the serial */
	env_idx = (gd->env_valid == 1);
#endif
	ret = erase_and_write_env(&location[env_idx], (u_char *)env_new,
				  env_check);
#ifdef CONFIG_ENV_OFFSET_REDUND
	if (!ret) {
		/* preset other copy for next write */
		gd->env_valid = gd->env_valid == 2 ? 1 : 2;
		free(env_check);
		free(env_new);
		return ret;
	}

	env_idx = (env_idx + 1) & 1;
	ret = erase_and_write_env(&location[env_idx], (u_char *)env_new,
				  env_check);
	if (!ret)
		printf("Warning: primary env write failed,"
				" redundancy is lost!\n");
#endif
	free(env_check);
	free(env_new);
	return ret;
}
#endif /* CMD_SAVEENV */

#if defined(CONFIG_SPL_BUILD)
static int readenv(size_t offset, u_char *buf)
{
	return nand_spl_load_image(offset, CONFIG_ENV_SIZE, buf);
}
#else
static int readenv(size_t offset, u_char *buf)
{
	return env_nand_xfer(offset, board_env_size, buf, 0);
}
#endif /* #if defined(CONFIG_SPL_BUILD) */

#ifdef CONFIG_ENV_OFFSET_OOB
int get_nand_env_oob(nand_info_t *nand, unsigned long *result)
{
	struct mtd_oob_ops ops;
	uint32_t oob_buf[ENV_OFFSET_SIZE / sizeof(uint32_t)];
	int ret;

	ops.datbuf	= NULL;
	ops.mode	= MTD_OOB_AUTO;
	ops.ooboffs	= 0;
	ops.ooblen	= ENV_OFFSET_SIZE;
	ops.oobbuf	= (void *)oob_buf;

	ret = nand->read_oob(nand, ENV_OFFSET_SIZE, &ops);
	if (ret) {
		printf("error reading OOB block 0\n");
		return ret;
	}

	if (oob_buf[0] == ENV_OOB_MARKER) {
		*result = oob_buf[1] * nand->erasesize;
	} else if (oob_buf[0] == ENV_OOB_MARKER_OLD) {
		*result = oob_buf[1];
	} else {
		printf("No dynamic environment marker in OOB block 0\n");
		return -ENOENT;
	}

	return 0;
}
#endif

#ifdef CONFIG_ENV_OFFSET_REDUND
void nand_env_relocate_spec(void)
{
#if !defined(ENV_IS_EMBEDDED)
	int read1_fail = 0, read2_fail = 0;
	int crc1_ok = 0, crc2_ok = 0;
	env_t *ep, *tmp_env1, *tmp_env2;

	tmp_env1 = (env_t *)malloc(CONFIG_ENV_SIZE);
	tmp_env2 = (env_t *)malloc(CONFIG_ENV_SIZE);
	if (tmp_env1 == NULL || tmp_env2 == NULL) {
		puts("Can't allocate buffers for environment\n");
		set_default_env("!malloc() failed");
		goto done;
	}

	read1_fail = readenv(CONFIG_ENV_OFFSET, (u_char *) tmp_env1);
	read2_fail = readenv(CONFIG_ENV_OFFSET_REDUND, (u_char *) tmp_env2);

	if (read1_fail && read2_fail)
		puts("*** Error - No Valid Environment Area found\n");
	else if (read1_fail || read2_fail)
		puts("*** Warning - some problems detected "
		     "reading environment; recovered successfully\n");

	crc1_ok = !read1_fail &&
		(crc32(0, tmp_env1->data, ENV_SIZE) == tmp_env1->crc);
	crc2_ok = !read2_fail &&
		(crc32(0, tmp_env2->data, ENV_SIZE) == tmp_env2->crc);

	if (!crc1_ok && !crc2_ok) {
		set_default_env("!bad CRC");
		goto done;
	} else if (crc1_ok && !crc2_ok) {
		gd->env_valid = 1;
	} else if (!crc1_ok && crc2_ok) {
		gd->env_valid = 2;
	} else {
		/* both ok - check serial */
		if (tmp_env1->flags == 255 && tmp_env2->flags == 0)
			gd->env_valid = 2;
		else if (tmp_env2->flags == 255 && tmp_env1->flags == 0)
			gd->env_valid = 1;
		else if (tmp_env1->flags > tmp_env2->flags)
			gd->env_valid = 1;
		else if (tmp_env2->flags > tmp_env1->flags)
			gd->env_valid = 2;
		else /* flags are equal - almost impossible */
			gd->env_valid = 1;
	}

	free(env_ptr);

	if (gd->env_valid == 1)
		ep = tmp_env1;
	else
		ep = tmp_env2;

	env_flags = ep->flags;
	env_import((char *)ep, 0);

done:
	free(tmp_env1);
	free(tmp_env2);

#endif /* ! ENV_IS_EMBEDDED */
}
#else /* ! CONFIG_ENV_OFFSET_REDUND */
/*
 * The legacy NAND code saved the environment in the first NAND
 * device i.e., nand_dev_desc + 0. This is also the behaviour using
 * the new NAND code.
 */
void nand_env_relocate_spec(void)
{
#if !defined(ENV_IS_EMBEDDED)
	int ret;
	char *buf = NULL;
	buf = (char *)memalign(ARCH_DMA_MINALIGN, CONFIG_ENV_SIZE);
	if (!buf) {
		printf("Error: Cannot allocate %d bytes\n", CONFIG_ENV_SIZE);
		set_default_env(NULL);
		return;
        }

#if defined(CONFIG_ENV_OFFSET_OOB)
	ret = get_nand_env_oob(&nand_info[nand_env_device], &nand_env_oob_offset);
	/*
	 * If unable to read environment offset from NAND OOB then fall through
	 * to the normal environment reading code below
	 */
	if (!ret) {
		printf("Found Environment offset in OOB..\n");
	} else {
		set_default_env("!no env offset in OOB");
		free(buf);
		return;
	}
#endif

	ret = readenv(CONFIG_ENV_OFFSET, (u_char *)buf);
	if (ret) {
		set_default_env("!readenv() failed");
		free(buf);
		return;
	}

	env_import(buf, 1);
	free(buf);
#endif /* ! ENV_IS_EMBEDDED */
}
#endif /* CONFIG_ENV_OFFSET_REDUND */
