/*
 * Host fixture for the RC01 UB03 NAND environment save checks.
 *
 * test_env_save.py inserts, at the @@EXTRACTED@@ marker, the real
 * struct nand_erase_options, the nand_read/nand_write/nand_block_isbad
 * inlines of include/nand.h and the real nand_erase_opts and
 * nand_check_range_capacity of drivers/mtd/nand/nand_util.c (by symbol),
 * then #includes the complete real common/env_nand.c (ENV_NAND_C), so its
 * own preprocessor structure (CMD_SAVEENV, SPL, redundant) is compiled as
 * is.  The U-Boot headers env_nand.c includes are empty stubs; this file
 * provides the few types and a simulated NAND: bits only program 1 -> 0,
 * erase sets 0xff, per-block fault injection, access accounting.
 *
 * env_export() is simulated with deterministic content and a real CRC32,
 * env_import() records what the real load path (nand_env_relocate_spec ->
 * readenv) delivers.
 *
 * Build parameters: ENV_FMT (CONFIG_ENV_SIZE, format range), PART_NB (ENV
 * partition size in erase blocks), optional CONFIG_ENV_OFFSET_REDUND.
 * Run "<exe> --list" for the scenario names and "<exe> <name>" for one
 * scenario; the result is "PASS <name>" or "FAIL <name>" on stderr.
 *
 * SPDX-License-Identifier:	GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>

#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif

#define loff_t long long
typedef unsigned char u_char;
typedef unsigned long ulong;

#define WATCHDOG_RESET() do { } while (0)
#define ARCH_DMA_MINALIGN 64
#define kfree free
#define lldiv(a, b) ((unsigned long long)(a) / (b))
#define do_div(n, base) ({ uint32_t __r = (n) % (base); (n) /= (base); __r; })
#define min(x, y) ((x) < (y) ? (x) : (y))

#define cpu_to_je16(x) (x)
#define cpu_to_je32(x) (x)
#define JFFS2_MAGIC_BITMASK 0x1985
#define JFFS2_NODETYPE_CLEANMARKER 0x2003
struct jffs2_unknown_node {
	uint16_t magic, nodetype;
	uint32_t totlen, hdr_crc;
};

#define MTD_OPS_AUTO_OOB 1
#define MTD_OOB_AUTO 1
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

struct erase_info {
	struct mtd_info *mtd;
	unsigned long long addr, len;
	int scrub;
};
typedef struct erase_info erase_info_t;

/* ---------------- U-Boot environment glue ---------------- */

#define CONFIG_SYS_MAX_NAND_DEVICE 1
nand_info_t nand_info[CONFIG_SYS_MAX_NAND_DEVICE];

loff_t board_env_offset, board_env_size, board_env_range;
#ifdef CONFIG_ENV_OFFSET_REDUND
#undef CONFIG_ENV_OFFSET_REDUND
loff_t board_env_redund_offset;
#define CONFIG_ENV_OFFSET_REDUND board_env_redund_offset
#endif
#define CONFIG_ENV_OFFSET board_env_offset
#define CONFIG_ENV_SIZE ENV_FMT
#define CONFIG_ENV_RANGE board_env_range

#ifdef CONFIG_ENV_OFFSET_REDUND
#define ENV_HEADER_SIZE 5
#else
#define ENV_HEADER_SIZE 4
#endif
#define ENV_SIZE (CONFIG_ENV_SIZE - ENV_HEADER_SIZE)
typedef struct environment_s {
	uint32_t crc;
#ifdef CONFIG_ENV_OFFSET_REDUND
	unsigned char flags;
#endif
	unsigned char data[ENV_SIZE];
} __attribute__((packed)) env_t;

struct global_data {
	ulong env_addr;
	int env_valid;
	ulong flags;
};
struct global_data sim_gd;
struct global_data *gd = &sim_gd;
#define DECLARE_GLOBAL_DATA_PTR
env_t *env_ptr;
const unsigned char default_environment[] = "";

/* Message capture: U-Boot output goes to stdout and a log buffer. */
static char logbuf[1 << 16];
static size_t loglen;
int sim_vlog(const char *fmt, va_list ap)
{
	va_list cp;
	int n;

	va_copy(cp, ap);
	n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, cp);
	va_end(cp);
	if (n > 0)
		loglen = min(loglen + n, sizeof(logbuf) - 1);
	return vprintf(fmt, ap);
}
int sim_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = sim_vlog(fmt, ap);
	va_end(ap);
	return n;
}
int sim_puts(const char *s)
{
	return sim_printf("%s", s);
}
#define printf sim_printf
#define puts sim_puts

/* Allocation accounting with fault injection (n-th call fails). */
int alloc_calls, alloc_fail_at, alloc_live;
void *sim_alloc(size_t n)
{
	void *p;

	if (++alloc_calls == alloc_fail_at)
		return NULL;
	p = malloc(n);
	if (p)
		alloc_live++;
	return p;
}
void sim_free(void *p)
{
	if (p)
		alloc_live--;
	free(p);
}
#define memalign(a, n) sim_alloc(n)
#define malloc(n) sim_alloc(n)
#define free(p) sim_free(p)

uint32_t crc32(uint32_t crc, const unsigned char *p, unsigned int len)
{
	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

/* env_export: deterministic content per generation, real CRC. */
static unsigned char exported[ENV_FMT];
int export_fail, export_calls, export_gen = 1;
int env_export(env_t *env_out)
{
	export_calls++;
	if (export_fail)
		return 1;
	for (size_t i = 0; i < ENV_SIZE; i++)
		env_out->data[i] = (unsigned char)(i * 31 + export_gen * 7 +
						   (i >> 9));
	memcpy(env_out->data, "bootcmd=bootipq", 16);
	env_out->crc = crc32(0, env_out->data, ENV_SIZE);
	memcpy(exported, env_out, ENV_FMT);
	return 0;
}

static unsigned char imported[ENV_FMT];
int import_calls, import_crc_ok, default_calls;
int env_import(const char *buf, int check)
{
	const env_t *ep = (const env_t *)buf;

	import_calls++;
	import_crc_ok = crc32(0, ep->data, ENV_SIZE) == ep->crc;
	memcpy(imported, buf, ENV_FMT);
	return 1;
}
void set_default_env(const char *s)
{
	default_calls++;
}
int nand_spl_load_image(uint32_t offs, unsigned int size, void *dst)
{
	return 1;
}

/* ---------------- simulated NAND ---------------- */

#define ES	0x20000u		/* erase block */
#define PS	0x800u			/* page */
#define NBLK	24u
#define DEV_SIZE (NBLK * ES)		/* 3 MiB */
#define P0_DEFAULT 8u			/* ENV partition start block */

struct blk {
	int bad, qerr, qerr_after_erase, eerr;
	int werr, wshort, wzero, wdrop;
	int rerr, rshort, rzero, rcorrupt, reuclean;
	int q, e, w, r;
};

static uint8_t flashmem[DEV_SIZE];
static uint8_t snapshot[DEV_SIZE];
static struct blk B[NBLK];
int outside_access, bad_programmed;
static struct nand_ecclayout sim_layout = { .oobavail = 16 };
static struct nand_chip sim_chip = { .ecc = { .layout = &sim_layout } };
unsigned int part_b0 = P0_DEFAULT, part_nb = PART_NB;

static int sim_bounds(struct mtd_info *m, loff_t ofs, unsigned long long len)
{
	if (ofs < 0 || (unsigned long long)ofs >= m->size ||
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
	if (b->qerr || (b->qerr_after_erase && b->e))
		return -EIO;
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
	if (b->eerr)
		return -EIO;
	memset(flashmem + instr->addr, 0xff, ES);
	return 0;
}

int mtd_write_oob(struct mtd_info *m, loff_t to, struct mtd_oob_ops *ops)
{
	return 0;
}

int mtd_write(struct mtd_info *m, loff_t to, size_t len, size_t *retlen,
	      const u_char *buf)
{
	struct blk *b;
	size_t n = len;

	*retlen = 0;
	if (sim_bounds(m, to, len ? len : 1))
		return -EINVAL;
	b = &B[to / ES];
	b->w++;
	if (b->bad)
		bad_programmed++;
	if (b->werr)
		return -EIO;
	if (b->wzero)
		n = 0;
	else if (b->wshort)
		n = len / 2;
	if (!b->wdrop)
		for (size_t i = 0; i < n; i++)
			flashmem[to + i] &= buf[i];
	*retlen = n;
	return 0;
}

int mtd_read(struct mtd_info *m, loff_t from, size_t len, size_t *retlen,
	     u_char *buf)
{
	struct blk *b;
	size_t n = len;

	*retlen = 0;
	if (sim_bounds(m, from, len ? len : 1))
		return -EINVAL;
	b = &B[from / ES];
	b->r++;
	if (b->rerr)
		return b->rerr;
	if (b->rzero)
		n = 0;
	else if (b->rshort)
		n = len / 2;
	memcpy(buf, flashmem + from, n);
	if (b->rcorrupt && n)
		buf[n / 3] ^= 0x01;
	*retlen = n;
	return b->reuclean ? -EUCLEAN : 0;
}

/* @@EXTRACTED@@ */

#include ENV_NAND_C

/* ---------------- scenarios ---------------- */

#if defined(CMD_SAVEENV) && !defined(CONFIG_SPL_BUILD)

#define P0 part_b0
#define PEND (part_b0 + part_nb)
#define NEED_BLOCKS ((ENV_FMT + ES - 1) / ES)

#define CHECK(c) do { if (!(c)) { \
	fprintf(stdout, "\n  check failed (%s:%d): %s\n", __FILE__, __LINE__, #c); \
	return false; } } while (0)

static void sim_setup(unsigned int b0)
{
	memset(flashmem, 0xa5, sizeof(flashmem));	/* neighbours */
	part_b0 = b0;
	part_nb = PART_NB;
	/* old environment in the partition */
	memset(flashmem + (size_t)b0 * ES, 0x5a, (size_t)part_nb * ES);
	memcpy(snapshot, flashmem, sizeof(flashmem));
	memset(B, 0, sizeof(B));
	outside_access = bad_programmed = 0;
	memset(nand_info, 0, sizeof(nand_info));
	nand_info[0].name = "sim-nand";
	nand_info[0].size = DEV_SIZE;
	nand_info[0].erasesize = ES;
	nand_info[0].writesize = PS;
	nand_info[0].oobsize = 64;
	nand_info[0].priv = &sim_chip;
	nand_env_device = 0;
	board_env_offset = (loff_t)b0 * ES;
	board_env_size = (loff_t)part_nb * ES;
	board_env_range = ENV_FMT;		/* NAND: CONFIG_ENV_SIZE_MAX */
	alloc_calls = alloc_fail_at = alloc_live = 0;
	export_fail = export_calls = 0;
	export_gen++;
	import_calls = import_crc_ok = default_calls = 0;
	loglen = 0;
	logbuf[0] = '\0';
	memset(&sim_gd, 0, sizeof(sim_gd));
	sim_gd.env_valid = 1;
}

static bool in_part(unsigned int blk)
{
	return blk >= P0 && blk < PEND;
}

/* No access outside the partition, neighbours unchanged, no leak. */
static bool clean_neighbours(void)
{
	for (unsigned int i = 0; i < NBLK; i++) {
		if (in_part(i))
			continue;
		if (B[i].q || B[i].e || B[i].w || B[i].r ||
		    memcmp(flashmem + (size_t)i * ES, snapshot + (size_t)i * ES, ES)) {
			printf("  block %u outside the partition touched\n", i);
			return false;
		}
	}
	return !outside_access && !bad_programmed && !alloc_live;
}

__attribute__((unused)) static int total(int which)
{
	int n = 0;

	for (unsigned int i = 0; i < NBLK; i++)
		n += which == 'e' ? B[i].e : which == 'w' ? B[i].w :
		     which == 'r' ? B[i].r : B[i].q;
	return n;
}

/* Nothing erased or written anywhere, old ENV intact. */
__attribute__((unused)) static bool untouched(void)
{
	return !total('e') && !total('w') &&
	       !memcmp(flashmem, snapshot, sizeof(flashmem));
}

/*
 * Expected format distribution (the original readenv walk): good blocks
 * of the partition in order, min(remaining, ES) bytes from each start.
 */
__attribute__((unused)) static bool layout_matches(const unsigned char *want)
{
	size_t done = 0;

	for (unsigned int i = P0; i < PEND && done < ENV_FMT; i++) {
		size_t len = min((size_t)ENV_FMT - done, (size_t)ES);

		if (B[i].bad)
			continue;
		if (memcmp(flashmem + (size_t)i * ES, want + done, len))
			return false;
		done += len;
	}
	return done == ENV_FMT;
}

/* The real load path: nand_env_relocate_spec -> readenv -> env_import. */
__attribute__((unused)) static bool import_matches(void)
{
	import_calls = default_calls = 0;
	nand_env_relocate_spec();
	return import_calls == 1 && !default_calls && import_crc_ok &&
	       !memcmp(imported, exported, ENV_FMT) && !alloc_live;
}

static bool log_has(const char *s)
{
	return strstr(logbuf, s) != NULL;
}

/* index of the last block that holds format data with no bad blocks */
#define LAST_DATA (P0 + NEED_BLOCKS - 1)

#ifndef CONFIG_ENV_OFFSET_REDUND

static bool save_ok_common(void)
{
	CHECK(nand_saveenv() == 0);
	CHECK(log_has("OK\n") && !log_has("FAILED"));
	CHECK(layout_matches(exported));
	for (unsigned int i = P0; i < PEND; i++)
		CHECK(B[i].bad ? !B[i].e && !B[i].w : B[i].e == 1);
	CHECK(clean_neighbours());
	CHECK(import_matches());
	return true;
}

static bool t_save_ok(void)
{
	sim_setup(P0_DEFAULT);
	CHECK(save_ok_common());
	/* only the blocks that hold the format are written */
	for (unsigned int i = P0; i < PEND; i++)
		CHECK(i <= LAST_DATA ? B[i].w == 1 : !B[i].w);
	return true;
}

static bool t_save_first_bad(void)
{
	sim_setup(P0_DEFAULT);
	B[P0].bad = 1;
	CHECK(save_ok_common());
	CHECK(B[P0 + 1].w == 1);
	return true;
}

static bool t_save_middle_bad(void)
{
	sim_setup(P0_DEFAULT);
	B[P0 + 1].bad = 1;
	CHECK(save_ok_common());
	CHECK(B[P0].w == 1);
	CHECK(NEED_BLOCKS == 1 || B[P0 + 2].w == 1);
	return true;
}

/* Capacity exactly sufficient: only the last NEED_BLOCKS blocks good. */
static bool t_save_exact_capacity(void)
{
	sim_setup(P0_DEFAULT);
	for (unsigned int i = P0; i < PEND - NEED_BLOCKS; i++)
		B[i].bad = 1;
	CHECK(save_ok_common());
	return true;
}

static bool t_device_end_ok(void)
{
	sim_setup(NBLK - PART_NB);
	B[P0].bad = 1;
	CHECK(save_ok_common());
	return true;
}

static bool t_precheck_tail_query_error(void)
{
	sim_setup(P0_DEFAULT);
	B[PEND - 1].qerr = 1;
	CHECK(nand_saveenv() != 0);
	CHECK(untouched());
	CHECK(!log_has("OK\n") && log_has("nothing erased"));
	CHECK(clean_neighbours());
	return true;
}

static bool t_precheck_capacity(void)
{
	sim_setup(P0_DEFAULT);
	for (unsigned int i = P0; i < PEND - NEED_BLOCKS + 1; i++)
		B[i].bad = 1;
	CHECK(nand_saveenv() != 0);
	CHECK(untouched());
	CHECK(!log_has("OK\n"));
	CHECK(clean_neighbours());
	return true;
}

/*
 * One good block too few.  With a single block format this means no good
 * block at all; with a multi-block format the good blocks would be erased
 * by a save without the check.
 */
static bool t_device_end_capacity(void)
{
	sim_setup(NBLK - PART_NB);
	for (unsigned int i = P0; i < PEND; i++)
		B[i].bad = i + NEED_BLOCKS - 1 < PEND;
	CHECK(nand_saveenv() != 0);
	CHECK(untouched());
	CHECK(clean_neighbours());
	return true;
}

static bool t_precheck_invalid_area(void)
{
	sim_setup(P0_DEFAULT);
	board_env_offset += PS;		/* not erase block aligned */
	CHECK(nand_saveenv() != 0);
	CHECK(untouched() && !outside_access && !alloc_live);

	sim_setup(P0_DEFAULT);
	board_env_range = board_env_size + ES;	/* format larger than area */
	CHECK(nand_saveenv() != 0);
	CHECK(untouched() && clean_neighbours());

	sim_setup(P0_DEFAULT);
	board_env_size = (loff_t)DEV_SIZE;	/* area past the device end */
	CHECK(nand_saveenv() != 0);
	CHECK(untouched() && !outside_access && !alloc_live);
	return true;
}

static bool unaligned_physical_area(int first_bad)
{
	sim_setup(P0_DEFAULT);
	board_env_size = (NEED_BLOCKS + 1) * ES - ES / 2;
	B[P0].bad = first_bad;
	CHECK(nand_saveenv() != 0);
	CHECK(untouched() && !total('q') && !total('r'));
	CHECK(!log_has("OK\n") && !outside_access && !alloc_live);
	return true;
}

static bool t_unaligned_physical_area(void)
{
	return unaligned_physical_area(0);
}

static bool t_unaligned_physical_area_first_bad(void)
{
	return unaligned_physical_area(1);
}

static bool t_alloc_fail_first(void)
{
	sim_setup(P0_DEFAULT);
	alloc_fail_at = 1;
	CHECK(nand_saveenv() != 0);
	CHECK(untouched() && !total('q') && clean_neighbours());
	return true;
}

static bool t_alloc_fail_check_buffer(void)
{
	sim_setup(P0_DEFAULT);
	alloc_fail_at = 2;
	CHECK(nand_saveenv() != 0);
	CHECK(untouched() && !total('q') && clean_neighbours());
	return true;
}

static bool t_export_fail(void)
{
	sim_setup(P0_DEFAULT);
	export_fail = 1;
	CHECK(nand_saveenv() != 0);
	CHECK(untouched() && !total('q') && clean_neighbours());
	return true;
}

static bool t_erase_error(void)
{
	sim_setup(P0_DEFAULT);
	B[PEND - 1].eerr = 1;
	CHECK(nand_saveenv() != 0);
	CHECK(!total('w') && !log_has("OK\n"));
	CHECK(clean_neighbours());
	return true;
}

static bool write_fault(int field)
{
	sim_setup(P0_DEFAULT);
	switch (field) {
	case 'e': B[LAST_DATA].werr = 1; break;
	case 's': B[LAST_DATA].wshort = 1; break;
	case 'z': B[LAST_DATA].wzero = 1; break;
	case 'd': B[LAST_DATA].wdrop = 1; break;
	}
	CHECK(nand_saveenv() != 0);
	CHECK(log_has("FAILED") && !log_has("OK\n"));
	/* stop at the failure, never continue into further blocks */
	for (unsigned int i = LAST_DATA + 1; i < PEND; i++)
		CHECK(!B[i].w);
	CHECK(clean_neighbours());
	return true;
}

static bool t_write_error(void) { return write_fault('e'); }
static bool t_write_short(void) { return write_fault('s'); }
static bool t_write_zero(void) { return write_fault('z'); }
/* program silently lost: only the read back can see it */
static bool t_verify_mismatch(void) { return write_fault('d'); }

static bool read_fault(int field, bool expect_ok)
{
	sim_setup(P0_DEFAULT);
	switch (field) {
	case 'b': B[LAST_DATA].rerr = -EBADMSG; break;
	case 'i': B[LAST_DATA].rerr = -EIO; break;
	case 's': B[LAST_DATA].rshort = 1; break;
	case 'z': B[LAST_DATA].rzero = 1; break;
	case 'c': B[LAST_DATA].rcorrupt = 1; break;
	case 'u': B[LAST_DATA].reuclean = 1; break;
	case 'U': B[LAST_DATA].reuclean = B[LAST_DATA].rcorrupt = 1; break;
	}
	if (expect_ok) {
		CHECK(nand_saveenv() == 0);
		CHECK(log_has("OK\n") && !log_has("FAILED"));
		CHECK(layout_matches(exported));
	} else {
		CHECK(nand_saveenv() != 0);
		CHECK(log_has("FAILED") && !log_has("OK\n"));
	}
	/* the read back used the written blocks */
	CHECK(B[LAST_DATA].r >= 1);
	CHECK(clean_neighbours());
	return true;
}

static bool t_verify_read_ebadmsg(void) { return read_fault('b', false); }
static bool t_verify_read_eio(void) { return read_fault('i', false); }
static bool t_verify_short_read(void) { return read_fault('s', false); }
static bool t_verify_zero_read(void) { return read_fault('z', false); }
static bool t_verify_corrupt_read(void) { return read_fault('c', false); }
static bool t_verify_euclean_ok(void) { return read_fault('u', true); }
static bool t_verify_euclean_mismatch(void) { return read_fault('U', false); }

/* A bad block query that fails during the write walk is an error. */
static bool t_walk_query_error(void)
{
	sim_setup(P0_DEFAULT);
	B[P0].qerr_after_erase = 1;
	CHECK(nand_saveenv() != 0);
	CHECK(!log_has("OK\n"));
	CHECK(!B[P0 + NEED_BLOCKS].w);
	CHECK(clean_neighbours());
	return true;
}

/* ---- load path (readenv), bounded and with the same distribution ---- */

static void place(const unsigned char *src)
{
	size_t done = 0;

	for (unsigned int i = P0; i < PEND && done < ENV_FMT; i++) {
		size_t len = min((size_t)ENV_FMT - done, (size_t)ES);

		if (B[i].bad)
			continue;
		memcpy(flashmem + (size_t)i * ES, src + done, len);
		done += len;
	}
	memcpy(snapshot, flashmem, sizeof(flashmem));
}

static bool import_case(int fault, bool expect_ok)
{
	env_t *e = calloc(1, ENV_FMT);

	sim_setup(P0_DEFAULT);
	B[P0].bad = 1;
	B[P0 + 2].bad = 1;
	env_export(e);
	alloc_live = 0;
	place((unsigned char *)e);
	free(e);
	alloc_live = 0;
	switch (fault) {
	case 's': B[P0 + 1].rshort = 1; break;
	case 'b': B[P0 + 1].rerr = -EBADMSG; break;
	case 'z': B[P0 + 1].rzero = 1; break;
	case 'q': B[P0 + 1].qerr = 1; break;
	case 'u': B[P0 + 1].reuclean = 1; break;
	}
	nand_env_relocate_spec();
	if (expect_ok)
		CHECK(import_calls == 1 && import_crc_ok && !default_calls &&
		      !memcmp(imported, exported, ENV_FMT));
	else
		CHECK(!import_calls && default_calls == 1);
	CHECK(clean_neighbours() && !total('w') && !total('e'));
	return true;
}

static bool t_import_bad_blocks(void) { return import_case(0, true); }
static bool t_import_euclean(void) { return import_case('u', true); }
static bool t_import_short_read(void) { return import_case('s', false); }
static bool t_import_zero_read(void) { return import_case('z', false); }
static bool t_import_ebadmsg(void) { return import_case('b', false); }
static bool t_import_query_error(void) { return import_case('q', false); }

static bool t_import_device_end_all_bad(void)
{
	sim_setup(NBLK - PART_NB);
	for (unsigned int i = P0; i < PEND; i++)
		B[i].bad = 1;
	nand_env_relocate_spec();
	CHECK(!import_calls && default_calls == 1);
	CHECK(clean_neighbours());
	return true;
}

/* Saving twice: the second save replaces the first completely. */
static bool t_save_twice(void)
{
	sim_setup(P0_DEFAULT);
	CHECK(nand_saveenv() == 0);
	export_gen++;
	loglen = 0;
	CHECK(nand_saveenv() == 0);
	CHECK(layout_matches(exported) && import_matches());
	CHECK(clean_neighbours());
	return true;
}

#define SCENARIOS(X) \
	X(t_save_ok) X(t_save_first_bad) X(t_save_middle_bad) \
	X(t_save_exact_capacity) X(t_device_end_ok) X(t_save_twice) \
	X(t_precheck_tail_query_error) X(t_precheck_capacity) \
	X(t_device_end_capacity) X(t_precheck_invalid_area) \
	X(t_unaligned_physical_area) X(t_unaligned_physical_area_first_bad) \
	X(t_alloc_fail_first) X(t_alloc_fail_check_buffer) X(t_export_fail) \
	X(t_erase_error) X(t_write_error) X(t_write_short) X(t_write_zero) \
	X(t_verify_mismatch) X(t_verify_read_ebadmsg) X(t_verify_read_eio) \
	X(t_verify_short_read) X(t_verify_zero_read) \
	X(t_verify_corrupt_read) X(t_verify_euclean_ok) \
	X(t_verify_euclean_mismatch) X(t_walk_query_error) \
	X(t_import_bad_blocks) X(t_import_euclean) X(t_import_short_read) \
	X(t_import_zero_read) X(t_import_ebadmsg) X(t_import_query_error) \
	X(t_import_device_end_all_bad)

#else /* CONFIG_ENV_OFFSET_REDUND: the compiled redundant path still works */

static bool t_redund_save(void)
{
	sim_setup(P0_DEFAULT);
	/* second copy in the last partition block, written first */
	board_env_redund_offset = (loff_t)(PEND - 1) * ES;
	sim_gd.env_valid = 1;
	CHECK(nand_saveenv() == 0);
	CHECK(B[PEND - 1].e == 1 && B[PEND - 1].w >= 1);
	/* crc and data as exported; flags is set by nand_saveenv afterwards */
	CHECK(!memcmp(flashmem + (size_t)(PEND - 1) * ES, exported, 4));
	CHECK(!memcmp(flashmem + (size_t)(PEND - 1) * ES + 5, exported + 5,
		      min((size_t)ENV_FMT, (size_t)ES) - 5));
	CHECK(log_has("OK\n") && clean_neighbours());
	return true;
}

static bool t_redund_fallback(void)
{
	sim_setup(P0_DEFAULT);
	board_env_redund_offset = (loff_t)(PEND - 1) * ES;
	B[PEND - 1].bad = 1;	/* second copy unusable: checked, not erased */
	sim_gd.env_valid = 1;
	CHECK(nand_saveenv() == 0);
	CHECK(!B[PEND - 1].e && !B[PEND - 1].w);
	CHECK(log_has("redundancy is lost"));
	CHECK(B[P0].e == 1 && B[P0].w >= 1 && clean_neighbours());
	return true;
}

#define SCENARIOS(X) X(t_redund_save) X(t_redund_fallback)

#endif /* CONFIG_ENV_OFFSET_REDUND */

static const struct { const char *name; bool (*fn)(void); } scenarios[] = {
#define ENTRY(n) { #n, n },
	SCENARIOS(ENTRY)
};

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		bool ok;

		if (argc > 1 && !strcmp(argv[1], "--list")) {
			fprintf(stderr, "%s\n", scenarios[i].name);
			continue;
		}
		if (argc > 1 && strcmp(argv[1], scenarios[i].name))
			continue;
		fprintf(stdout, "== %s\n", scenarios[i].name);
		ok = scenarios[i].fn();
		fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL",
			scenarios[i].name);
	}
	return 0;
}

#else /* compile-only configurations */

int main(void)
{
	return 0;
}

#endif
