/*
 * Host fixture for the RC01 UB01 NAND write safety checks.
 *
 * test_nand_write_safety.py extracts the real functions (by symbol) from
 * include/nand.h, drivers/mtd/nand/nand_util.c and common/cmd_flashwrite.c
 * and inserts them at the @@EXTRACTED@@ marker below.  This file only
 * provides the minimal U-Boot types, a simulated NAND (bits can only be
 * programmed 1 -> 0, erase sets 0xff, per-block fault injection) and the
 * scenarios.  Built with -DBASELINE it contains only the API scenarios
 * that the unfixed baseline is expected to fail.
 *
 * SPDX-License-Identifier:	GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>

#define loff_t long long
typedef uint8_t u8;

#define CONFIG_CMD_NAND 1
#define CONFIG_CMD_NAND_TRIMFFS 1
#define CONFIG_NAND_FLASH_INFO_IDX 0
#define CONFIG_SYS_MAX_NAND_DEVICE 2

#define CMD_RET_SUCCESS 0
#define CMD_RET_FAILURE 1
#define CMD_RET_USAGE -1
#define SMEM_PTN_NAME_MAX 16

#define SMEM_BOOT_NAND_FLASH 2
#define SMEM_BOOT_SPI_FLASH 3
#define SMEM_BOOT_MMC_FLASH 5
#define SMEM_BOOT_QSPI_NAND_FLASH 11

#define WATCHDOG_RESET() do { } while (0)
#define ARCH_DMA_MINALIGN 64
#define memalign(a, n) malloc(n)
#define kfree free
#define lldiv(a, b) ((unsigned long long)(a) / (b))
#define do_div(n, base) ({ uint32_t __r = (n) % (base); (n) /= (base); __r; })
#define min(x, y) ((x) < (y) ? (x) : (y))
#define puts(s) fputs((s), stdout)

#define cpu_to_je16(x) (x)
#define cpu_to_je32(x) (x)
#define JFFS2_MAGIC_BITMASK 0x1985
#define JFFS2_NODETYPE_CLEANMARKER 0x2003
struct jffs2_unknown_node {
	uint16_t magic, nodetype;
	uint32_t totlen, hdr_crc;
};

#define MTD_OPS_PLACE_OOB 0
#define MTD_OPS_AUTO_OOB 1
#define MTD_OPS_RAW 2
struct mtd_oob_ops {
	int mode;
	size_t len, retlen, ooblen, oobretlen;
	uint32_t ooboffs;
	uint8_t *datbuf, *oobbuf;
};

#define NAND_BBT_SCANNED 0x40000000
struct nand_ecclayout { uint32_t oobavail; };
struct nand_chip {
	uint8_t *bbt;
	unsigned int options;
	struct { struct nand_ecclayout *layout; } ecc;
};

struct mtd_info {
	const char *name;
	uint64_t size;
	uint32_t erasesize, writesize, oobsize;
	void *priv;
};
typedef struct mtd_info nand_info_t;
typedef struct mtd_info mtd_info_t;

struct erase_info {
	struct mtd_info *mtd;
	unsigned long long addr, len;
	int scrub;
	void (*callback)(struct erase_info *);
};
typedef struct erase_info erase_info_t;

/* ---------------- simulated NAND ---------------- */

#define ES	0x20000u		/* erase block */
#define PS	0x800u			/* page */
#define DEV_SIZE 0x01400000u		/* 20 MiB, 160 blocks */
#define NBLK	(DEV_SIZE / ES)

/* RC01 layout: APPSBL [0x01180000, 0x01300000), APPSBL_1 follows */
#define APPSBL_OFF	0x01180000u
#define APPSBL_SIZE	0x00180000u
#define APPSBL_END	(APPSBL_OFF + APPSBL_SIZE)
#define APPSBL_B0	(APPSBL_OFF / ES)
#define APPSBL_NB	(APPSBL_SIZE / ES)

struct blk {
	int bad, qerr, eerr, eerr_marks_bad, werr, wshort;
	int rerr, rshort, rcorrupt, reuclean, bad_at_query;
	int q, e, w, r;
	unsigned long wbytes;
};

static uint8_t flashmem[DEV_SIZE];
static struct blk B[NBLK];
static int outside_access, oob_err, bad_programmed;
static struct nand_ecclayout sim_layout = { .oobavail = 16 };
static struct nand_chip sim_chip = { .ecc = { .layout = &sim_layout } };

int nand_curr_device = -1;
nand_info_t nand_info[CONFIG_SYS_MAX_NAND_DEVICE];

typedef struct {
	uint32_t flash_type, flash_block_size, flash_secondary_type;
	struct { uint32_t offset, size; } rootfs;
} qca_smem_flash_info_t;
static qca_smem_flash_info_t qca_smem_flash_info;
static uint32_t flash_type_new = -1;
struct smem_ptn { const char *name; uint32_t start, size; };
static struct {
	int len;
	struct smem_ptn parts[2];
} smem_ptable = { 2, {
	{ "APPSBL", APPSBL_OFF / ES, APPSBL_SIZE / ES },
	{ "APPSBL_1", APPSBL_END / ES, APPSBL_SIZE / ES },
} };
#define smem_parts smem_ptable.parts

static void sim_geometry(nand_info_t *n)
{
	n->name = "sim-nand";
	n->size = DEV_SIZE;
	n->erasesize = ES;
	n->writesize = PS;
	n->oobsize = 64;
	n->priv = &sim_chip;
}

static void sim_reset(void)
{
	memset(flashmem, 0xff, sizeof(flashmem));
	/* old image in the target partition, neighbours erased (worst case) */
	memset(flashmem + APPSBL_OFF, 0x5a, APPSBL_SIZE);
	memset(B, 0, sizeof(B));
	outside_access = oob_err = bad_programmed = 0;
	memset(nand_info, 0, sizeof(nand_info));
	sim_geometry(&nand_info[0]);
	nand_curr_device = 0;
	qca_smem_flash_info.flash_type = SMEM_BOOT_NAND_FLASH;
	qca_smem_flash_info.flash_block_size = ES;
	flash_type_new = -1;
	smem_parts[0].start = APPSBL_B0;
	smem_parts[0].size = APPSBL_NB;
}

static int sim_bounds(struct mtd_info *m, loff_t ofs, unsigned long long len)
{
	if (ofs < 0 || (unsigned long long)ofs > m->size ||
	    len > m->size - (unsigned long long)ofs) {
		outside_access++;
		return -EINVAL;
	}
	return 0;
}

int mtd_block_isbad(struct mtd_info *m, loff_t ofs)
{
	struct blk *b;

	if (sim_bounds(m, ofs, 1))
		return -EINVAL;
	b = &B[ofs / ES];
	b->q++;
	if (b->qerr)
		return -EIO;
	if (b->bad_at_query && b->q >= b->bad_at_query)
		b->bad = 1;
	return b->bad;
}

int mtd_erase(struct mtd_info *m, struct erase_info *instr)
{
	struct blk *b;

	if (sim_bounds(m, instr->addr, instr->len) || instr->len != ES ||
	    instr->addr % ES)
		return -EINVAL;
	b = &B[instr->addr / ES];
	b->e++;
	if (b->eerr) {
		if (b->eerr_marks_bad)
			b->bad = 1;
		return -EIO;
	}
	memset(flashmem + instr->addr, 0xff, ES);
	return 0;
}

int mtd_write_oob(struct mtd_info *m, loff_t to, struct mtd_oob_ops *ops)
{
	if (sim_bounds(m, to, 1))
		return -EINVAL;
	return oob_err ? -EIO : 0;
}

int mtd_write(struct mtd_info *m, loff_t to, size_t len, size_t *retlen,
	      const u_char *buf)
{
	size_t done = 0;

	*retlen = 0;
	if (sim_bounds(m, to, len))
		return -EINVAL;
	while (done < len) {
		loff_t a = to + done;
		struct blk *b = &B[a / ES];
		size_t n = min(len - done, (size_t)(ES - a % ES));
		size_t i;

		if (b->werr)
			return -EIO;
		if (b->wshort) {
			b->wshort = 0;
			n /= 2;
			len = done + n;	/* stop after a short program */
		}
		for (i = 0; i < n; i++)
			flashmem[a + i] &= buf[done + i];
		b->w++;
		b->wbytes += n;
		if (b->bad)
			bad_programmed++;
		done += n;
		*retlen = done;
	}
	return 0;
}

int mtd_read(struct mtd_info *m, loff_t from, size_t len, size_t *retlen,
	     u_char *buf)
{
	size_t done = 0;
	int ret = 0;

	*retlen = 0;
	if (sim_bounds(m, from, len))
		return -EINVAL;
	while (done < len) {
		loff_t a = from + done;
		struct blk *b = &B[a / ES];
		size_t n = min(len - done, (size_t)(ES - a % ES));

		b->r++;
		if (b->rerr)
			return -EBADMSG;
		if (b->rshort) {
			b->rshort = 0;	/* one-shot short transfer */
			n /= 2;
			len = done + n;
		}
		memcpy(buf + done, flashmem + a, n);
		if (b->rcorrupt && n)
			buf[done] ^= 0x01;
		if (b->reuclean)
			ret = -EUCLEAN;
		done += n;
		*retlen = done;
	}
	return ret;
}

/* ---------------- run_command stub ---------------- */

static char cmdlog[2048];
static int fail_ipq_nand;

static int run_one(const char *c)
{
	int dev;
	char layout[16];

	if (sscanf(c, "nand device %d", &dev) == 1) {
		if (dev < 0 || dev >= CONFIG_SYS_MAX_NAND_DEVICE ||
		    !nand_info[dev].name)
			return 1;
		nand_curr_device = dev;
		return 0;
	}
	if (sscanf(c, "ipq_nand %15s", layout) == 1) {
		if (fail_ipq_nand)
			return 1;
		/* deinit, then nand_register() of the new layout */
		nand_curr_device = -1;
		memset(&nand_info[0], 0, sizeof(nand_info[0]));
		sim_geometry(&nand_info[0]);
		nand_curr_device = 0;
		return 0;
	}
	if (!strncmp(c, "mmc ", 4) || !strncmp(c, "sf ", 3))
		return 0;	/* other backends: record only */
	return 1;		/* anything else (e.g. numeric nand erase) */
}

int run_command(const char *cmd, int flag)
{
	char buf[256], *p, *amp;

	(void)flag;
	if (strlen(cmdlog) + strlen(cmd) + 2 < sizeof(cmdlog)) {
		strcat(cmdlog, cmd);
		strcat(cmdlog, "|");
	}
	snprintf(buf, sizeof(buf), "%s", cmd);
	p = buf;
	for (;;) {
		char *s, *e;

		amp = strstr(p, "&&");
		if (amp)
			*amp = 0;
		s = p;
		while (*s == ' ')
			s++;
		e = s + strlen(s);
		while (e > s && e[-1] == ' ')
			*--e = 0;
		if (*s && run_one(s))
			return 1;
		if (!amp)
			break;
		p = amp + 2;
	}
	return 0;
}

static void reset_all(void)
{
	sim_reset();
	cmdlog[0] = 0;
	fail_ipq_nand = 0;
}

/* ---------------- do_flash entry stubs (SMEM partition table) -------- */

typedef struct cmd_tbl_s cmd_tbl_t;
typedef struct { unsigned long start, size, blksz; } disk_partition_t;
#define IPQ_NAND_ROOTFS_SIZE (64u << 20)

static char env_fileaddr[16], env_filesize[16];

int smem_getpart(char *name, uint32_t *start, uint32_t *size)
{
	unsigned int i;

	for (i = 0; i < sizeof(smem_parts) / sizeof(smem_parts[0]); i++)
		if (!strcmp(name, smem_parts[i].name)) {
			*start = smem_parts[i].start;
			*size = smem_parts[i].size;
			return 0;
		}
	return -ENOENT;
}

static char *uboot_getenv(const char *n)
{
	if (!strcmp(n, "fileaddr"))
		return env_fileaddr[0] ? env_fileaddr : NULL;
	if (!strcmp(n, "filesize"))
		return env_filesize[0] ? env_filesize : NULL;
	return NULL;
}
#define getenv(n) uboot_getenv(n)
#define simple_strtoul(s, e, b) strtoul((s), (e), (b))
static int get_which_flash_param(char *p) { (void)p; return 0; }
static uint32_t get_part_block_size(struct smem_ptn *p,
				  qca_smem_flash_info_t *sfi)
{ (void)p; return sfi->flash_block_size; }
static uint32_t get_device_id_by_part(struct smem_ptn *p)
{ (void)p; return 0; }
static int smem_bootconfig_info(void) { return -1; }
static unsigned int get_rootfs_active_partition(void) { return 0; }

/* @@EXTRACTED@@ */

/* ---------------- helpers ---------------- */

static uint8_t payload[0x200000] __attribute__((aligned(64)));
static uint8_t rbuf[0x200000];

static void fill_payload(size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		payload[i] = (uint8_t)(i * 7 + (i >> 11) + 1);
	for (i = 0; i < len; i += 255)
		if (payload[i] == 0xff)
			payload[i] = 0x11;
}

/* sum counter over blocks [b0, b1) */
#define SUM(field, b0, b1) ({ int __s = 0, __i; \
	for (__i = (b0); __i < (int)(b1); __i++) __s += B[__i].field; __s; })

static int fails;
static const char *cur;

#define CHECK(cond) do { if (!(cond)) { \
	fflush(stdout); \
	fprintf(stderr, "  check failed (%s:%d): %s\n", cur, __LINE__, \
		#cond); \
	return 1; } } while (0)

/* Content expected for a skip-bad write of len bytes starting at off. */
static int placed_ok(uint32_t off, uint32_t end, size_t len)
{
	uint32_t a = off;
	size_t done = 0;

	while (done < len) {
		if (a >= end)
			return 0;
		if (B[a / ES].bad) {
			a += ES;
			continue;
		}
		size_t n = min(len - done, (size_t)ES);
		if (memcmp(flashmem + a, payload + done, n))
			return 0;
		done += n;
		a += ES;
	}
	return 1;
}

static int neighbours_untouched(void)
{
	/* nothing outside APPSBL is queried, erased or programmed */
	return SUM(q, 0, APPSBL_B0) == 0 && SUM(e, 0, APPSBL_B0) == 0 &&
	       SUM(w, 0, APPSBL_B0) == 0 &&
	       SUM(q, APPSBL_B0 + APPSBL_NB, NBLK) == 0 &&
	       SUM(e, APPSBL_B0 + APPSBL_NB, NBLK) == 0 &&
	       SUM(w, APPSBL_B0 + APPSBL_NB, NBLK) == 0 &&
	       outside_access == 0;
}

static int zero_erase_write(void)
{
	return SUM(e, 0, NBLK) == 0 && SUM(w, 0, NBLK) == 0;
}

/* ---------------- API scenarios (also run against the baseline) ------- */

/* Astra counterexample: lim = 3 PEB, payload 2 PEB, first block bad at
 * scan time, the next block turns bad during the write loop. */
static int t_api_post_scan_bad_bounded(void)
{
	uint32_t off = APPSBL_OFF;
	size_t len = 2 * ES;
	int ret;

	fill_payload(len);
	B[APPSBL_B0].bad = 1;
	B[APPSBL_B0 + 1].bad_at_query = 2;	/* good in scan, bad in loop */
	memset(flashmem + off, 0xff, 4 * ES);
	ret = nand_write_skip_bad(&nand_info[0], off, &len, NULL, 3 * ES,
				  payload, WITH_WR_VERIFY);
	CHECK(ret != 0);
	CHECK(B[APPSBL_B0 + 3].w == 0 && B[APPSBL_B0 + 3].q == 0);
	CHECK(outside_access == 0);
	return 0;
}

static int t_api_negative_query_fails(void)
{
	size_t len = 2 * ES;
	int ret;

	fill_payload(len);
	memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
	B[APPSBL_B0 + 1].qerr = 1;
	ret = nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				  APPSBL_SIZE, payload, WITH_WR_VERIFY);
	CHECK(ret < 0);
	CHECK(SUM(w, APPSBL_B0 + 1, NBLK) == 0);
	return 0;
}

/* Insufficient capacity within lim: the scan must not query past lim. */
static int t_api_scan_stops_at_lim(void)
{
	size_t len = 3 * ES;
	size_t actual;
	int ret;

	fill_payload(len);
	B[APPSBL_B0].bad = 1;
	ret = nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len, &actual,
				  3 * ES, payload, WITH_WR_VERIFY);
	CHECK(ret == -EFBIG);
	CHECK(len == 0);
	CHECK(SUM(q, APPSBL_B0 + 3, NBLK) == 0);
	CHECK(SUM(w, 0, NBLK) == 0);
	return 0;
}

static int t_api_erase_failure_returns(void)
{
	nand_erase_options_t o;
	int ret;

	memset(&o, 0, sizeof(o));
	o.offset = APPSBL_OFF;
	o.length = APPSBL_SIZE;
	o.lim = APPSBL_SIZE;
	o.quiet = 1;
	B[APPSBL_B0 + 2].eerr = 1;
	B[APPSBL_B0 + 2].eerr_marks_bad = 1;
	ret = nand_erase_opts(&nand_info[0], &o);
	CHECK(ret < 0);
	/* stops at the failing block */
	CHECK(SUM(e, APPSBL_B0 + 3, NBLK) == 0);
	return 0;
}

static int t_api_cleanmarker_failure_returns(void)
{
	nand_erase_options_t o;

	memset(&o, 0, sizeof(o));
	o.offset = APPSBL_OFF;
	o.length = 2 * ES;
	o.quiet = 1;
	o.jffs2 = 1;
	oob_err = 1;
	CHECK(nand_erase_opts(&nand_info[0], &o) != 0);
	CHECK(B[APPSBL_B0 + 1].e == 0);
	return 0;
}

static int t_api_short_write_fails(void)
{
	size_t len = 2 * ES;
	int ret;

	fill_payload(len);
	B[APPSBL_B0].bad = 1;		/* use the block loop */
	B[APPSBL_B0 + 1].wshort = 1;
	memset(flashmem + APPSBL_OFF, 0xff, 4 * ES);
	ret = nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				  APPSBL_SIZE, payload, 0);
	CHECK(ret != 0);
	return 0;
}

#ifndef BASELINE
/* ---------------- further API scenarios ---------------- */

static int t_api_erase_known_bad_skip(void)
{
	nand_erase_options_t o;

	memset(&o, 0, sizeof(o));
	o.offset = APPSBL_OFF;
	o.length = 4 * ES;
	o.lim = 4 * ES;
	o.quiet = 1;
	B[APPSBL_B0 + 1].bad = 1;
	CHECK(nand_erase_opts(&nand_info[0], &o) == 0);
	CHECK(B[APPSBL_B0 + 1].e == 0);
	CHECK(B[APPSBL_B0].e == 1 && B[APPSBL_B0 + 2].e == 1 &&
	      B[APPSBL_B0 + 3].e == 1);
	CHECK(SUM(e, APPSBL_B0 + 4, NBLK) == 0);	/* spread=0 counts bad */
	return 0;
}

static int t_api_erase_spread_preserved(void)
{
	nand_erase_options_t o;

	memset(&o, 0, sizeof(o));
	o.offset = APPSBL_OFF;
	o.length = 2 * ES;
	o.spread = 1;
	o.lim = 3 * ES;
	o.quiet = 1;
	B[APPSBL_B0].bad = 1;
	CHECK(nand_erase_opts(&nand_info[0], &o) == 0);
	CHECK(B[APPSBL_B0 + 1].e == 1 && B[APPSBL_B0 + 2].e == 1);
	CHECK(SUM(e, APPSBL_B0 + 3, NBLK) == 0);

	/* spread past the limit is still refused */
	memset(B, 0, sizeof(B));
	B[APPSBL_B0].bad = 1;
	B[APPSBL_B0 + 1].bad = 1;
	CHECK(nand_erase_opts(&nand_info[0], &o) == -EFBIG);
	CHECK(SUM(e, APPSBL_B0 + 3, NBLK) == 0);
	return 0;
}

static int t_api_write_skip_and_verify_ok(void)
{
	size_t len = 3 * ES + 5 * PS;
	int ret;

	fill_payload(len);
	memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
	B[APPSBL_B0 + 1].bad = 1;
	ret = nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				  APPSBL_SIZE, payload, WITH_WR_VERIFY);
	CHECK(ret == 0 && len == 3 * ES + 5 * PS);
	CHECK(B[APPSBL_B0 + 1].w == 0);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));
	return 0;
}

/* cmd_nand style lim = chip end - offset keeps working. */
static int t_api_chip_lim_regression(void)
{
	size_t len = 2 * ES, actual = 0;

	fill_payload(len);
	memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
	B[APPSBL_B0].bad = 1;
	CHECK(nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len, &actual,
				  DEV_SIZE - APPSBL_OFF, payload,
				  WITH_WR_VERIFY) == 0);
	CHECK(actual == 3 * ES);
	CHECK(placed_ok(APPSBL_OFF, DEV_SIZE, len));

	/* the last blocks of the chip: beyond the device is -EINVAL */
	len = 2 * ES;
	B[NBLK - 2].bad = 1;
	CHECK(nand_write_skip_bad(&nand_info[0], DEV_SIZE - 2 * ES, &len,
				  NULL, 2 * ES, payload, 0) != 0);
	len = 2 * ES;
	CHECK(nand_write_skip_bad(&nand_info[0], DEV_SIZE - 2 * ES, &len,
				  NULL, 1ULL << 40, payload, 0) == -EINVAL);
	CHECK(outside_access == 0);
	return 0;
}

static int t_api_trimffs(void)
{
	size_t len = 3 * ES;
	unsigned long full;
	int ret;

	fill_payload(len);
	/* last 10 pages of block 1 and all of block 2 are 0xff */
	memset(payload + 2 * ES - 10 * PS, 0xff, 10 * PS);
	memset(payload + 2 * ES, 0xff, ES);
	memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
	ret = nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				  APPSBL_SIZE, payload,
				  WITH_DROP_FFS | WITH_WR_VERIFY);
	CHECK(ret == 0 && len == 3 * ES);
	full = B[APPSBL_B0].wbytes;
	CHECK(full == ES);
	CHECK(B[APPSBL_B0 + 1].wbytes == ES - 10 * PS);
	CHECK(B[APPSBL_B0 + 2].wbytes == 0);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));
	return 0;
}

static int t_api_write_error_and_verify_errors(void)
{
	size_t len;
	int i;
	/* write error, read-back error, short read-back, mismatch */
	for (i = 0; i < 4; i++) {
		sim_reset();
		len = 2 * ES;
		fill_payload(len);
		memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
		if (i == 0) B[APPSBL_B0 + 1].werr = 1;
		if (i == 1) B[APPSBL_B0 + 1].rerr = 1;
		if (i == 2) B[APPSBL_B0 + 1].rshort = 1;
		if (i == 3) B[APPSBL_B0 + 1].rcorrupt = 1;
		/* fast path (no bad block) */
		CHECK(nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len,
					  NULL, APPSBL_SIZE, payload,
					  WITH_WR_VERIFY) != 0);
		CHECK(SUM(w, APPSBL_B0 + 2, NBLK) == 0);
	}
	for (i = 0; i < 4; i++) {
		sim_reset();
		len = 2 * ES;
		fill_payload(len);
		memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
		B[APPSBL_B0].bad = 1;	/* block loop */
		if (i == 0) B[APPSBL_B0 + 1].werr = 1;
		if (i == 1) B[APPSBL_B0 + 1].rerr = 1;
		if (i == 2) B[APPSBL_B0 + 1].rshort = 1;
		if (i == 3) B[APPSBL_B0 + 1].rcorrupt = 1;
		CHECK(nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len,
					  NULL, APPSBL_SIZE, payload,
					  WITH_WR_VERIFY) != 0);
		/* stops, does not continue the payload in the next block */
		CHECK(SUM(w, APPSBL_B0 + 2, NBLK) == 0);
	}
	/* correctable ECC on read-back is still a success */
	sim_reset();
	len = 2 * ES;
	fill_payload(len);
	memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
	B[APPSBL_B0 + 1].reuclean = 1;
	CHECK(nand_write_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				  APPSBL_SIZE, payload, WITH_WR_VERIFY) == 0);
	return 0;
}

static int t_api_read_regression(void)
{
	size_t len = 3 * ES, actual = 0;
	uint32_t i;

	fill_payload(len);
	memset(flashmem + APPSBL_OFF, 0xff, APPSBL_SIZE);
	B[APPSBL_B0 + 1].bad = 1;
	for (i = 0; i < 3; i++)
		memcpy(flashmem + APPSBL_OFF + (i ? i + 1 : 0) * ES,
		       payload + i * ES, ES);

	/* skipping read, chip-level lim like cmd_nand / bootm */
	memset(rbuf, 0, len);
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, &actual,
				 DEV_SIZE - APPSBL_OFF, rbuf) == 0);
	CHECK(len == 3 * ES && actual == 4 * ES);
	CHECK(!memcmp(rbuf, payload, len));

	/* partition lim: still fits */
	len = 3 * ES;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 4 * ES, rbuf) == 0);
	/* partition lim too small: refused, no query past lim */
	len = 3 * ES;
	memset(B, 0, sizeof(B));
	B[APPSBL_B0 + 1].bad = 1;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 3 * ES, rbuf) == -EFBIG);
	CHECK(SUM(q, APPSBL_B0 + 3, NBLK) == 0);

	/* fast path read */
	len = ES;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 ES, rbuf) == 0 && len == ES);
	CHECK(!memcmp(rbuf, payload, ES));

	/* correctable ECC is success, uncorrectable/short/query fail */
	B[APPSBL_B0].reuclean = 1;
	len = ES;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 ES, rbuf) == 0);
	B[APPSBL_B0].reuclean = 0;
	B[APPSBL_B0].rerr = 1;
	len = ES;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 ES, rbuf) != 0);
	B[APPSBL_B0].rerr = 0;
	B[APPSBL_B0].rshort = 1;
	len = ES;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 ES, rbuf) != 0);
	B[APPSBL_B0 + 2].rshort = 1;	/* in the block loop */
	len = 3 * ES;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 APPSBL_SIZE, rbuf) != 0);
	B[APPSBL_B0 + 2].qerr = 1;
	len = 3 * ES;
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 APPSBL_SIZE, rbuf) != 0);
	CHECK(outside_access == 0);
	return 0;
}

static int t_api_capacity_helper_edges(void)
{
	nand_info_t *n = &nand_info[0];

	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE,
					APPSBL_SIZE) == 0);
	B[APPSBL_B0 + APPSBL_NB - 1].bad = 1;
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE,
					APPSBL_SIZE) == -EFBIG);
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE,
					APPSBL_SIZE - ES) == 0);
	CHECK(SUM(q, APPSBL_B0 + APPSBL_NB, NBLK) == 0);
	/* invalid inputs */
	CHECK(nand_check_range_capacity(n, APPSBL_OFF + PS, APPSBL_SIZE,
					PS) == -EINVAL);
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE + PS,
					PS) == -EINVAL);
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, 0, 0) == -EINVAL);
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE,
					0) == -EINVAL);
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE,
					PS + 1) == -EINVAL);
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, ES,
					2 * ES) == -EINVAL);
	CHECK(nand_check_range_capacity(n, DEV_SIZE, ES, PS) == -EINVAL);
	CHECK(nand_check_range_capacity(n, DEV_SIZE - ES, 2 * ES,
					PS) == -EINVAL);
	CHECK(nand_check_range_capacity(n, 0, DEV_SIZE, PS) == 0);
	n->erasesize = 0;
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE,
					PS) == -EINVAL);
	n->erasesize = 3 * PS;
	CHECK(nand_check_range_capacity(n, APPSBL_OFF, APPSBL_SIZE,
					PS) == -EINVAL);
	sim_geometry(n);
	CHECK(outside_access == 0);
	return 0;
}

/* ---------------- flash (write_to_flash) scenarios ---------------- */

static int flash(uint32_t off, uint32_t size, uint32_t len, char *layout)
{
	return write_to_flash(SMEM_BOOT_NAND_FLASH,
			      (uint32_t)(uintptr_t)payload, off, size, len,
			      layout);
}

/* 12-block APPSBL, full-size image, one bad block: needs 13 blocks. */
static int t_flash_appsbl_near_full_rejected(void)
{
	fill_payload(APPSBL_SIZE);
	B[APPSBL_B0 + 4].bad = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, APPSBL_SIZE, "default") ==
	      CMD_RET_FAILURE);
	CHECK(zero_erase_write());
	CHECK(neighbours_untouched());
	/* old image still there */
	CHECK(flashmem[APPSBL_OFF] == 0x5a);
	return 0;
}

static int t_flash_exact_capacity(void)
{
	size_t len = APPSBL_SIZE - ES;

	fill_payload(len);
	B[APPSBL_B0 + 4].bad = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
	      CMD_RET_SUCCESS);
	CHECK(B[APPSBL_B0 + 4].w == 0 && B[APPSBL_B0 + 4].e == 0);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));
	CHECK(neighbours_untouched());
	CHECK(!strcmp(cmdlog, "nand device 0|"));
	/* bounded read back through the shared read helper */
	memset(rbuf, 0, len);
	CHECK(nand_read_skip_bad(&nand_info[0], APPSBL_OFF, &len, NULL,
				 APPSBL_SIZE, rbuf) == 0);
	CHECK(!memcmp(rbuf, payload, len));
	return 0;
}

static int t_flash_bad_with_margin(void)
{
	size_t len = 6 * ES + 3 * PS;

	fill_payload(len);
	B[APPSBL_B0].bad = 1;
	B[APPSBL_B0 + 5].bad = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
	      CMD_RET_SUCCESS);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));
	CHECK(bad_programmed == 0);
	CHECK(neighbours_untouched());
	/* whole partition was erased (tail beyond the payload included) */
	CHECK(flashmem[APPSBL_END - 1] == 0xff);
	return 0;
}

static int t_flash_normal_no_bad(void)
{
	size_t len = 5 * ES + 7 * PS;

	fill_payload(len);
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
	      CMD_RET_SUCCESS);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));
	CHECK(SUM(e, APPSBL_B0, APPSBL_B0 + APPSBL_NB) == APPSBL_NB);
	CHECK(neighbours_untouched());
	return 0;
}

static int t_flash_all_bad(void)
{
	int i;

	fill_payload(ES);
	for (i = 0; i < (int)APPSBL_NB; i++)
		B[APPSBL_B0 + i].bad = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, PS, "default") ==
	      CMD_RET_FAILURE);
	CHECK(zero_erase_write());
	CHECK(neighbours_untouched());
	return 0;
}

/* Payload fits in the first two blocks, query fails on the last block of
 * the partition that would be erased. */
static int t_flash_tail_query_error(void)
{
	fill_payload(2 * ES);
	B[APPSBL_B0 + APPSBL_NB - 1].qerr = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, 2 * ES, "default") ==
	      CMD_RET_FAILURE);
	CHECK(zero_erase_write());
	CHECK(neighbours_untouched());
	return 0;
}

/* Capacity exactly fits at scan time; another block turns bad when the
 * write loop queries it.  Must fail without touching APPSBL_1. */
static int t_flash_post_scan_bad(void)
{
	size_t len = APPSBL_SIZE - ES;

	fill_payload(len);
	B[APPSBL_B0].bad = 1;
	/* queries: precheck(1), erase(2), write scan(3), write loop(4) */
	B[APPSBL_B0 + 6].bad_at_query = 4;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
	      CMD_RET_FAILURE);
	CHECK(neighbours_untouched());
	CHECK(flashmem[APPSBL_END] == 0xff);
	return 0;
}

/* A block that went bad during the write's own scan is fine if the
 * remaining capacity still fits. */
static int t_flash_scan_time_bad_with_margin(void)
{
	size_t len = 8 * ES;

	fill_payload(len);
	B[APPSBL_B0 + 3].bad_at_query = 3;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
	      CMD_RET_SUCCESS);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));
	CHECK(neighbours_untouched());
	return 0;
}

static int t_flash_erase_failure_no_write(void)
{
	size_t len = 4 * ES;

	fill_payload(len);
	B[APPSBL_B0 + 2].eerr = 1;
	B[APPSBL_B0 + 2].eerr_marks_bad = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
	      CMD_RET_FAILURE);
	CHECK(SUM(w, 0, NBLK) == 0);
	CHECK(SUM(e, APPSBL_B0 + 3, NBLK) == 0);
	CHECK(neighbours_untouched());

	/* same without the driver marking it bad */
	sim_reset();
	fill_payload(len);
	B[APPSBL_B0].eerr = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
	      CMD_RET_FAILURE);
	CHECK(SUM(w, 0, NBLK) == 0);
	return 0;
}

static int t_flash_write_and_readback_errors(void)
{
	int i, j;

	for (j = 0; j < 2; j++) {
		for (i = 0; i < 5; i++) {
			size_t len = 4 * ES;

			sim_reset();
			cmdlog[0] = 0;
			fill_payload(len);
			if (j)
				B[APPSBL_B0].bad = 1;	/* skip loop */
			struct blk *b = &B[APPSBL_B0 + 2];
			if (i == 0) b->werr = 1;
			if (i == 1) b->wshort = 1;
			if (i == 2) b->rerr = 1;
			if (i == 3) b->rshort = 1;
			if (i == 4) b->rcorrupt = 1;
			CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "default") ==
			      CMD_RET_FAILURE);
			CHECK(neighbours_untouched());
		}
	}
	/* ECC-corrected read-back keeps succeeding */
	sim_reset();
	fill_payload(4 * ES);
	B[APPSBL_B0 + 2].reuclean = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, 4 * ES, "default") ==
	      CMD_RET_SUCCESS);
	return 0;
}

/* IPQ806X: ipq_nand rebuilds NAND state; geometry after the switch is
 * used.  Before the switch the device has no usable geometry. */
static int t_flash_layout_switch(void)
{
	size_t len = 2 * ES;

	fill_payload(len);
	nand_info[0].erasesize = 0;
	nand_info[0].writesize = 0;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "linux") ==
	      CMD_RET_SUCCESS);
	CHECK(!strcmp(cmdlog, "nand device 0 && ipq_nand linux|"));
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));

	sim_reset();
	cmdlog[0] = 0;
	fail_ipq_nand = 1;
	CHECK(flash(APPSBL_OFF, APPSBL_SIZE, len, "sbl") == CMD_RET_FAILURE);
	CHECK(zero_erase_write());
	CHECK(SUM(q, 0, NBLK) == 0);

	/* QSPI NAND type takes the same bounded path */
	sim_reset();
	cmdlog[0] = 0;
	fail_ipq_nand = 0;
	CHECK(write_to_flash(SMEM_BOOT_QSPI_NAND_FLASH,
			     (uint32_t)(uintptr_t)payload, APPSBL_OFF,
			     APPSBL_SIZE, len, "default") == CMD_RET_SUCCESS);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, len));
	return 0;
}

static int t_flash_invalid_ranges(void)
{
	/* unaligned partition start, unaligned size,
	 * zero payload, payload > partition, past device, uint32 wrap */
	uint32_t c[][3] = {
		{ APPSBL_OFF + PS, APPSBL_SIZE, PS },
		{ APPSBL_OFF, APPSBL_SIZE - PS, PS },
		{ APPSBL_OFF, APPSBL_SIZE, 0xfffff900u },
		{ APPSBL_OFF, APPSBL_SIZE, 0 },
		{ APPSBL_OFF, APPSBL_SIZE, APPSBL_SIZE + PS },
		{ DEV_SIZE - ES, 2 * ES, PS },
		{ DEV_SIZE, ES, PS },
		{ 0xfffe0000u, 0x40000u, PS },
		{ APPSBL_OFF, 0, 0 },
	};
	unsigned int i;

	fill_payload(4 * ES);
	for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		sim_reset();
		CHECK(flash(c[i][0], c[i][1], c[i][2], "default") ==
		      CMD_RET_FAILURE);
		CHECK(zero_erase_write());
		CHECK(outside_access == 0);
	}
	return 0;
}

/* MMC and NOR keep their command routes and units: compare with the
 * baseline write_to_flash when available, and with literal commands. */
static int route(int type, uint32_t a, uint32_t o, uint32_t p, uint32_t f,
		 int base, char *out, size_t outlen)
{
	int ret;

	cmdlog[0] = 0;
#ifdef HAVE_BASE_WRITE_TO_FLASH
	if (base)
		ret = base_write_to_flash(type, a, o, p, f, "default");
	else
#endif
		ret = write_to_flash(type, a, o, p, f, "default");
	(void)base;
	snprintf(out, outlen, "%d:%s", ret, cmdlog);
	return ret;
}

static int t_flash_other_backends_unchanged(void)
{
	char n[512], b[512];

	route(SMEM_BOOT_MMC_FLASH, 0x44000000, 0x22, 0x40, 0x10, 0, n,
	      sizeof(n));
	CHECK(!strcmp(n, "0:mmc erase 0x22 0x40 && "
		      "mmc write 0x44000000 0x22 0x10 && |"));
	route(SMEM_BOOT_SPI_FLASH, 0x44000000, 0x180000, 0x10000, 0x8000, 0,
	      n, sizeof(n));
	CHECK(!strcmp(n, "0:sf probe && sf erase 0x180000 0x10000 && "
		      "sf write 0x44000000 0x180000 0x8000 && |"));
	CHECK(SUM(q, 0, NBLK) == 0 && zero_erase_write());
#ifdef HAVE_BASE_WRITE_TO_FLASH
	route(SMEM_BOOT_MMC_FLASH, 0x44000000, 0x22, 0x40, 0x10, 0, n,
	      sizeof(n));
	route(SMEM_BOOT_MMC_FLASH, 0x44000000, 0x22, 0x40, 0x10, 1, b,
	      sizeof(b));
	CHECK(!strcmp(n, b));
	route(SMEM_BOOT_SPI_FLASH, 0x44000000, 0x180000, 0x10000, 0x8000, 0,
	      n, sizeof(n));
	route(SMEM_BOOT_SPI_FLASH, 0x44000000, 0x180000, 0x10000, 0x8000, 1,
	      b, sizeof(b));
	CHECK(!strcmp(n, b));
	fprintf(stderr, "  routes match baseline write_to_flash\n");
#else
	(void)b;
#endif
	return 0;
}

/* The real `flash <part> <addr> <size>` entry: SMEM blocks -> bytes,
 * page rounding, image size check, then the bounded NAND bridge. */
static int flash_cmd(const char *part, uint32_t len)
{
	char a[16], l[16];
	char *argv[] = { "flash", (char *)part, a, l, NULL };

	snprintf(a, sizeof(a), "%x", (uint32_t)(uintptr_t)payload);
	snprintf(l, sizeof(l), "%x", len);
	return do_flash(NULL, 0, 4, argv);
}

static int t_do_flash_entry(void)
{
	/* full-size APPSBL image, one bad block: rejected, zero erase */
	fill_payload(APPSBL_SIZE);
	B[APPSBL_B0 + 7].bad = 1;
	CHECK(flash_cmd("APPSBL", APPSBL_SIZE) != 0);
	CHECK(zero_erase_write());
	CHECK(neighbours_untouched());

	/* unaligned image size is page rounded and fits with the margin */
	sim_reset();
	fill_payload(APPSBL_SIZE - ES);
	CHECK(flash_cmd("APPSBL", APPSBL_SIZE - ES - 100) == 0);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, APPSBL_SIZE - ES - 100));
	CHECK(neighbours_untouched());

	/* page rounding overflow is refused before narrowing */
	sim_reset();
	CHECK(flash_cmd("APPSBL", 0xfffff900u) != 0);
	CHECK(zero_erase_write());

	/* image larger than the partition: refused before the bridge */
	sim_reset();
	CHECK(flash_cmd("APPSBL", APPSBL_SIZE + PS) != 0);
	CHECK(zero_erase_write() && SUM(q, 0, NBLK) == 0);

	/* unknown partition */
	sim_reset();
	CHECK(flash_cmd("nonexistent", PS) != 0);
	CHECK(zero_erase_write());
	return 0;
}

static int t_do_flash_smem_overflow(void)
{
	uint32_t out, len;

	fill_payload(PS);
	smem_parts[0].start = 0x8001;
	smem_parts[0].size = 1;
	CHECK(flash_cmd("APPSBL", PS) != 0);
	CHECK(zero_erase_write() && SUM(q, 0, NBLK) == 0);

	sim_reset();
	smem_parts[0].size = 0x800c;
	CHECK(flash_cmd("APPSBL", PS) != 0);
	CHECK(zero_erase_write() && SUM(q, 0, NBLK) == 0);

	/* The same real byte resolver protects mixed-storage callers. */
	CHECK(getpart_offset_size("APPSBL", &out, &len) != 0);
	sim_reset();
	qca_smem_flash_info.flash_block_size = 0;
	CHECK(flash_cmd("APPSBL", PS) != 0);
	CHECK(zero_erase_write() && SUM(q, 0, NBLK) == 0);

	sim_reset();
	smem_parts[0].size = ~0u;
	CHECK(getpart_offset_size("APPSBL", &out, &len) == 0);
	CHECK(out == APPSBL_OFF && len == DEV_SIZE - APPSBL_OFF);
	smem_parts[0].start = DEV_SIZE / ES + 1;
	CHECK(getpart_offset_size("APPSBL", &out, &len) != 0);
	return 0;
}

#ifdef CONFIG_IPQ806X
static int t_do_flash_layout_rounding(void)
{
	fill_payload(2 * PS);
	nand_info[0].writesize = 0;
	CHECK(flash_cmd("APPSBL", PS + 1) == 0);
	CHECK(nand_info[0].writesize == PS);
	CHECK(placed_ok(APPSBL_OFF, APPSBL_END, 2 * PS));
	CHECK(neighbours_untouched());
	return 0;
}
#endif
#endif /* !BASELINE */

struct test { const char *name; int (*fn)(void); };

#define T(x) { #x, x }
static const struct test tests[] = {
	T(t_api_post_scan_bad_bounded),
	T(t_api_negative_query_fails),
	T(t_api_scan_stops_at_lim),
	T(t_api_erase_failure_returns),
	T(t_api_cleanmarker_failure_returns),
	T(t_api_short_write_fails),
#ifndef BASELINE
	T(t_api_erase_known_bad_skip),
	T(t_api_erase_spread_preserved),
	T(t_api_write_skip_and_verify_ok),
	T(t_api_chip_lim_regression),
	T(t_api_trimffs),
	T(t_api_write_error_and_verify_errors),
	T(t_api_read_regression),
	T(t_api_capacity_helper_edges),
	T(t_flash_appsbl_near_full_rejected),
	T(t_flash_exact_capacity),
	T(t_flash_bad_with_margin),
	T(t_flash_normal_no_bad),
	T(t_flash_all_bad),
	T(t_flash_tail_query_error),
	T(t_flash_post_scan_bad),
	T(t_flash_scan_time_bad_with_margin),
	T(t_flash_erase_failure_no_write),
	T(t_flash_write_and_readback_errors),
	T(t_flash_layout_switch),
	T(t_flash_invalid_ranges),
	T(t_flash_other_backends_unchanged),
	T(t_do_flash_entry),
	T(t_do_flash_smem_overflow),
#ifdef CONFIG_IPQ806X
	T(t_do_flash_layout_rounding),
#endif
#endif
};

int main(void)
{
	unsigned int i;

	if ((uintptr_t)payload >> 32) {
		fprintf(stderr, "payload not addressable by uint32_t\n");
		return 2;
	}
	/* U-Boot messages go to stdout, results to stderr */
	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int r;

		reset_all();
		cur = tests[i].name;
		printf("=== %s\n", cur);
		r = tests[i].fn();
		fflush(stdout);
		fprintf(stderr, "%s %s\n", r ? "FAIL" : "PASS", cur);
		fails += r != 0;
	}
	return fails ? 1 : 0;
}
