/*
 * Host fixture for the RC01 UB02 upgrade result checks.
 *
 * test_upgrade_results.py extracts the real functions (by symbol) and
 * inserts the types at @@TYPES@@ and the functions at @@EXTRACTED@@:
 * the hush list runner, cmd_process, the "flash" and "ubi" commands, the
 * SYSUPGRADE UBI writer, the web upgrade handlers and failsafe_httpd_poll.
 * This file only provides minimal U-Boot types, simulated NAND/UBI/MMC/SF
 * back ends, a command tokenizer feeding the real hush runner, and the
 * scenarios.  Each scenario prints "PASS name" or "FAIL name" on stderr.
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

#define __U_BOOT__ 1
#define CONFIG_CMD_NAND 1
#define CONFIG_CMD_UBI 1
#define CONFIG_LWIP_HTTPD 1
#define IPQ_UBI_VOL_WRITE_SUPPORT 1
#define CONFIG_NAND_FLASH_INFO_IDX 0
#define CONFIG_SYS_MAX_NAND_DEVICE 2
#define CONFIG_SYS_HUSH_PARSER 1

#define loff_t long long
typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned char uchar;
typedef unsigned char u_char;
typedef uint8_t u8;
typedef unsigned long long u64;

#define WATCHDOG_RESET() do { } while (0)
#define puts(s) fputs((s), stdout)
#define debug(...) do { } while (0)
#define debug_printf(...) do { } while (0)
#define simple_strtoul strtoul
#define simple_strtoull strtoull
#define kfree free
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((typeof(x))(a) - 1))
#define FLAG_CONT_ON_NEWLINE (1 << 3)
#define CMD_FLAG_ENV 0x0004

enum command_ret_t {
	CMD_RET_SUCCESS,
	CMD_RET_FAILURE,
	CMD_RET_USAGE = -1,
};

struct cmd_tbl_s {
	const char *name;
	int maxargs;
	int repeatable;
	int (*cmd)(struct cmd_tbl_s *, int, int, char * const []);
};
typedef struct cmd_tbl_s cmd_tbl_t;

enum {
	SMEM_BOOT_NO_FLASH = 0, SMEM_BOOT_NOR_FLASH = 1,
	SMEM_BOOT_NAND_FLASH = 2, SMEM_BOOT_MMC_FLASH = 5,
	SMEM_BOOT_SPI_FLASH = 6, SMEM_BOOT_NORPLUSNAND = 7,
	SMEM_BOOT_NORPLUSEMMC = 8, SMEM_BOOT_QSPI_NAND_FLASH = 11,
};

enum firmware_type_enum {
	FW_TYPE_FIT = 0, FW_TYPE_GPT = 1, FW_TYPE_QSDK = 2, FW_TYPE_UBI = 3,
	FW_TYPE_CDT = 4, FW_TYPE_ELF = 5, FW_TYPE_MIBIB = 6,
	FW_TYPE_SYSUPGRADE = 7,
};

/* ---------------- command log ---------------- */

#define MAXLOG 256
static char cmdlog[MAXLOG][200];
static int ncmd;

static void log_cmd(int argc, char * const argv[])
{
	char *p;
	int i;

	if (ncmd >= MAXLOG)
		return;
	p = cmdlog[ncmd++];
	p[0] = 0;
	for (i = 0; i < argc; i++) {
		if (i)
			strcat(p, " ");
		strncat(p, argv[i], 190 - strlen(p));
	}
}

/* number of executed commands starting with prefix */
static int ran(const char *prefix)
{
	int i, n = 0;

	for (i = 0; i < ncmd; i++)
		if (!strncmp(cmdlog[i], prefix, strlen(prefix)))
			n++;
	return n;
}

/* number of executed commands equal to cmd */
static int ran_eq(const char *cmd)
{
	int i, n = 0;

	for (i = 0; i < ncmd; i++)
		if (!strcmp(cmdlog[i], cmd))
			n++;
	return n;
}

static int index_of(const char *prefix)
{
	int i;

	for (i = 0; i < ncmd; i++)
		if (!strncmp(cmdlog[i], prefix, strlen(prefix)))
			return i;
	return -1;
}

/* ---------------- environment ---------------- */

static char env_filesize[32], env_fileaddr[32], env_mtdids[64];

#define getenv fx_getenv
static char *getenv(const char *name)
{
	if (!strcmp(name, "filesize"))
		return env_filesize[0] ? env_filesize : NULL;
	if (!strcmp(name, "fileaddr"))
		return env_fileaddr[0] ? env_fileaddr : NULL;
	if (!strcmp(name, "mtdids"))
		return env_mtdids[0] ? env_mtdids : NULL;
	return NULL;
}

static int setenv_hex(const char *name, ulong v)
{
	char *d = !strcmp(name, "filesize") ? env_filesize :
		  !strcmp(name, "fileaddr") ? env_fileaddr : NULL;

	if (d)
		snprintf(d, 32, "%lx", v);
	return 0;
}

static ulong get_timer(ulong base) { return 100000 - base; }

/* ---------------- SMEM partitions ---------------- */

#define SMEM_PTN_NAME_MAX 16
#define ES	0x20000u		/* erase block */
#define PS	0x800u			/* page */
#define NAND_SIZE 0x04000000u		/* 64 MiB, 512 blocks */
#define NBLK	(NAND_SIZE / ES)
#define IPQ_NAND_ROOTFS_SIZE (64 * ES)

typedef struct {
	uint32_t flash_type, flash_block_size, flash_secondary_type;
	struct { uint32_t offset, size; } rootfs;
} qca_smem_flash_info_t;
qca_smem_flash_info_t qca_smem_flash_info;
uint32_t flash_type_new = -1;

struct smem_ptn { char name[SMEM_PTN_NAME_MAX]; uint32_t start, size; };
static struct {
	int len;
	struct smem_ptn parts[12];
} smem_ptable;

/* block numbers of the simulated layout */
#define APPSBL_BLK	8
#define ROOTFS_BLK	64
#define ROOTFS_NB	64
#define ROOTFS_1_BLK	128
#define ROOTFS1_BLK	192
#define ROOTFS2_BLK	256
#define NORP_BLK	320

static void smem_add(const char *name, uint32_t start, uint32_t size)
{
	struct smem_ptn *p = &smem_ptable.parts[smem_ptable.len++];

	snprintf(p->name, sizeof(p->name), "%s", name);
	p->start = start;
	p->size = size;
}

static uint32_t get_part_block_size(struct smem_ptn *p,
				    qca_smem_flash_info_t *sfi)
{
	return sfi->flash_block_size;
}

static int get_device_id_by_part(struct smem_ptn *p) { return 0; }
static int which_flash_param;
static int get_which_flash_param(char *name) { return which_flash_param; }
static int smem_bootconfig_info(void) { return 0; }
static unsigned int get_rootfs_active_partition(void) { return 0; }

static int smem_getpart(char *name, uint32_t *start, uint32_t *size)
{
	int i;

	for (i = 0; i < smem_ptable.len; i++)
		if (!strcmp(smem_ptable.parts[i].name, name)) {
			*start = smem_ptable.parts[i].start;
			*size = smem_ptable.parts[i].size;
			return 0;
		}
	return -ENOENT;
}

/* ---------------- simulated NAND ---------------- */

struct mtd_info {
	const char *name;
	uint64_t size;
	uint32_t erasesize, writesize;
};
typedef struct mtd_info nand_info_t;

int nand_curr_device = -1;
nand_info_t nand_info[CONFIG_SYS_MAX_NAND_DEVICE];

static int bad[NBLK], qerr[NBLK];
static int nquery, nquery_unselected, nselect, select_rc;
static int nerase, nwrite;
static uint32_t erase_off[32], write_off[32];
static size_t write_len[32];
static uint32_t fail_erase_at = ~0u, fail_write_at = ~0u;

int mtd_block_isbad(struct mtd_info *m, loff_t ofs)
{
	if (ofs < 0 || (uint64_t)ofs >= m->size)
		return -EINVAL;
	nquery++;
	if (!nselect)
		nquery_unselected++;
	if (qerr[ofs / ES])
		return -EIO;
	return bad[ofs / ES];
}

/* @@TYPES@@ */

static struct selected_dev ubi_dev;

static int nand_erase_opts(nand_info_t *nand, const nand_erase_options_t *o)
{
	if (nerase < 32)
		erase_off[nerase] = o->offset;
	nerase++;
	return (uint32_t)o->offset == fail_erase_at ? -EIO : 0;
}

static int nand_write_skip_bad(nand_info_t *nand, loff_t offset,
			       size_t *length, size_t *actual, loff_t lim,
			       u_char *buffer, int flags)
{
	if (nwrite < 32) {
		write_off[nwrite] = offset;
		write_len[nwrite] = *length;
	}
	nwrite++;
	if ((uint64_t)*length > (uint64_t)lim || !(flags & WITH_WR_VERIFY))
		return -EINVAL;
	if ((uint32_t)offset == fail_write_at) {
		*length = 0;
		return -EIO;
	}
	return 0;
}

static int nwrites_at(uint32_t blk)
{
	int i, n = 0;

	for (i = 0; i < nwrite && i < 32; i++)
		if (write_off[i] == blk * ES)
			n++;
	return n;
}

/* ---------------- simulated MMC ---------------- */

typedef struct { ulong start, size, blksz; } disk_partition_t;
typedef struct { uint64_t lba; uint32_t blksz; } block_dev_desc_t;
static struct { int dev_num; } mmc_host;
static block_dev_desc_t sim_mmc = { 0x100000, 512 };
static int mmc_present = 1, mmc_part_found = 1;

#define GPT_PART_NAME "0:GPT"
#define GPT_BACKUP_PART_NAME "0:GPTBACKUP"

static block_dev_desc_t *mmc_get_dev(int dev)
{
	return mmc_present ? &sim_mmc : NULL;
}

static int get_partition_info_efi_by_name(block_dev_desc_t *d,
					  const char *name,
					  disk_partition_t *info)
{
	if (!mmc_part_found)
		return -1;
	info->start = 0x100;
	info->size = 0x200;
	info->blksz = 512;
	return 0;
}

static int gpt_convert_and_write(block_dev_desc_t *d, const char *name,
				 uint32_t addr, uint32_t size)
{
	return 0;
}

/* ---------------- simulated UBI ---------------- */

#define UBI_VOL_NAME_MAX 127
#define UBI_MAX_VOLUME_NAME 127
#define UBI_INT_VOL_COUNT 1
#define UBI_VOL_NUM_AUTO (-1)
#define UBI_DYNAMIC_VOLUME 3
#define UBI_STATIC_VOLUME 4
#define VTBL 8
#define LEB 0x1f000

struct ubi_device;
struct ubi_volume {
	struct ubi_device *ubi;
	int vol_id, reserved_pebs, data_pad, usable_leb_size, alignment;
	int vol_type, name_len, updating, upd_marker, corrupted, checked;
	long long used_bytes;
	int *eba_tbl;
	char name[UBI_VOL_NAME_MAX + 1];
};

struct ubi_device {
	int ubi_num, vtbl_slots, leb_size, min_io_size;
	int avail_pebs, rsvd_pebs, beb_rsvd_level, beb_rsvd_pebs;
	int vol_count, ro_mode, corr_peb_count;
	struct ubi_volume *volumes[VTBL + 1];
};

struct ubi_mkvol_req {
	int32_t vol_id, alignment;
	int64_t bytes;
	int8_t vol_type, padding1;
	int16_t name_len;
	int8_t padding2[4];
	char name[UBI_MAX_VOLUME_NAME + 1];
};

#define ubi_msg(fmt, ...) printf("UBI: " fmt "\n", ##__VA_ARGS__)
#define ubi_err(u, fmt, ...) printf("UBI error: " fmt "\n", ##__VA_ARGS__)
#define ubi_warn(u, fmt, ...) printf("UBI warning: " fmt "\n", ##__VA_ARGS__)

static struct ubi_device sim_ubi;
static struct ubi_device *ubi;
struct ubi_device *ubi_devices[1];
static char buffer[80];
static int ubi_initialized;

/* counters of UBI side effects */
static int n_attach, n_rootfs_attach, n_remove, n_create, n_upd, n_wrdata;
static int attach_rc, rootfs_attach_rc;
static const char *fail_remove, *fail_create, *fail_write;
static int fail_create_err;
static char wrdata_name[16][32];

static int ubi_ops(void)
{
	return n_attach + n_rootfs_attach + n_remove + n_create + n_upd +
	       n_wrdata;
}

static void sim_vol(const char *name, int id, int pebs)
{
	struct ubi_volume *v = calloc(1, sizeof(*v));

	snprintf(v->name, sizeof(v->name), "%s", name);
	v->name_len = strlen(name);
	v->vol_id = id;
	v->reserved_pebs = pebs;
	v->usable_leb_size = LEB;
	v->alignment = 1;
	v->ubi = &sim_ubi;
	sim_ubi.volumes[id] = v;
	sim_ubi.avail_pebs -= pebs;
	sim_ubi.rsvd_pebs += pebs;
	sim_ubi.vol_count++;
}

/* volume id by name from the simulated table, -1 when absent */
static int vol_id(const char *name)
{
	int i;

	for (i = 0; i < VTBL; i++)
		if (sim_ubi.volumes[i] &&
		    !strcmp(sim_ubi.volumes[i]->name, name))
			return i;
	return -1;
}

static int ubi_change_vtbl_record(struct ubi_device *u, int id, void *rec)
{
	n_remove++;
	if (fail_remove && !strcmp(u->volumes[id]->name, fail_remove))
		return -EIO;
	return 0;
}

static int ubi_eba_unmap_leb(struct ubi_device *u, struct ubi_volume *v,
			     int lnum)
{
	return 0;
}

/* Mirrors drivers/mtd/ubi/vmt.c: lowest free ID, unique name, PEBs. */
int ubi_create_volume(struct ubi_device *u, struct ubi_mkvol_req *req)
{
	int i, id = req->vol_id, pebs;
	struct ubi_volume *v;

	n_create++;
	if (u->ro_mode)
		return -EROFS;
	if (fail_create && !strcmp(req->name, fail_create))
		return fail_create_err;
	if (id == UBI_VOL_NUM_AUTO) {
		for (i = 0; i < u->vtbl_slots; i++)
			if (!u->volumes[i]) {
				id = i;
				break;
			}
		if (id == UBI_VOL_NUM_AUTO)
			return -ENFILE;
	}
	if (u->volumes[id])
		return -EEXIST;
	for (i = 0; i < u->vtbl_slots; i++)
		if (u->volumes[i] && !strcmp(u->volumes[i]->name, req->name))
			return -EEXIST;
	pebs = (req->bytes + LEB - 1) / LEB;
	if (pebs > u->avail_pebs)
		return -ENOSPC;
	sim_vol(req->name, id, pebs);
	req->vol_id = id;
	return 0;
}

static int ubi_start_update(struct ubi_device *u, struct ubi_volume *v,
			    long long bytes)
{
	n_upd++;
	return 0;
}

static int ubi_more_update_data(struct ubi_device *u, struct ubi_volume *v,
				const void *buf, int count)
{
	if (n_wrdata < 16)
		snprintf(wrdata_name[n_wrdata], 32, "%s", v->name);
	n_wrdata++;
	if (fail_write && !strcmp(v->name, fail_write))
		return -EIO;
	v->used_bytes = count;
	return 0;
}

static int ubi_check_volume(struct ubi_device *u, int id) { return 0; }
static void ubi_gluebi_updated(struct ubi_volume *v) { }
static int ubi_info(int layout) { return 0; }
static int ubi_volume_read(char *volume, char *buf, size_t size) { return 0; }
static int ubi_detach_mtd_dev(int n, int force) { return 0; }
static void put_mtd_device(struct mtd_info *m) { }

static void sim_attach(void)
{
	ubi = &sim_ubi;
	ubi_devices[0] = &sim_ubi;
}

/* smem.c: attaches the rootfs partition with "ubi part fs" */
int ubi_set_rootfs_part(void)
{
	n_rootfs_attach++;
	if (rootfs_attach_rc)
		return rootfs_attach_rc;
	ubi_dev.selected = 1;
	sim_attach();
	return 0;
}

/* ---------------- web state ---------------- */

static uint8_t sim_upload[0x10000], sim_ram[0x10000];
#define UPLOAD_ADDR ((unsigned long)sim_upload)
#define RAM_BOOT_ADDR ((unsigned long)sim_ram)
#define NOR_FIRMWARE_START 0x00100000ul
#define NOR_FIRMWARE_SIZE 0x00e00000ul

typedef struct {
	const void *kernel_data;
	size_t kernel_size;
	const void *rootfs_data;
	size_t rootfs_size;
} sysupgrade_fw_parts;

int webfailsafe_is_running, webfailsafe_ready_for_upgrade;
int webfailsafe_upgrade_type, webfailsafe_backup_avail_enabled;
int webfailsafe_auto_reboot_enabled = 1;
int webfailsafe_img_flash;
int upgrade_status;
ulong net_boot_file_size, load_addr;
static int httpd_progress_start_done, eth_init_attempted;
static ulong periodic_timer;
static struct { int chunked, chunk_busy; ulong total_remaining; } backup;

static uint32_t cur_flash_type;
static int fw_type, last_progress, n_reset, n_done;
static sysupgrade_fw_parts sim_parts;

static int get_current_flash_type(uint32_t *t) { *t = cur_flash_type; return 0; }
static int check_fw_type(void *p) { return fw_type; }
static sysupgrade_fw_parts parse_sysupgrade_firmware(void *p) { return sim_parts; }
static void printChecksumMd5(ulong a, ulong s) { }
static int do_http_progress(const int state) { last_progress = state; return 0; }
static void httpd_poll_wait(int count) { }
static void HttpdDone(void) { n_done++; }
static int do_reset(cmd_tbl_t *c, int f, int argc, char * const argv[]) { n_reset++; return 0; }
static int webterm_run_pending_command(void) { return 0; }
static void *eth_get_dev(void) { return NULL; }
static int eth_is_active(void *d) { return 1; }
static void eth_halt(void) { }
static void eth_set_current(void) { }
static int eth_init(void) { return 0; }
static int eth_check_link_change(void) { return 0; }
static int eth_rx(void) { return 0; }
static void sys_check_timeouts(void) { }
static void backup_chunk_next(void) { }
static int httpd_is_running(void) { return 0; }
static void httpd_poll(void) { }

/* ---------------- hush glue ---------------- */

typedef struct {
	char *data;
	int length, maxlen, quote, nonnull;
} o_string;
#define NULL_O_STRING { NULL, 0, 0, 0, 0 }

static int flag_repeat, last_return_code, syntax_errors;
static struct pipe *pending_chain;

#define syntax() (syntax_errors++)
static int ctrlc(void) { return 0; }
static int had_ctrlc(void) { return 0; }
static char **make_list_in(char **inp, char *name) { abort(); }
static int static_peek(struct in_str *i) { return 0; }
static int file_peek(struct in_str *i) { return 0; }
#define b_peek(input) ((input)->peek(input))
static void initialize_context(struct p_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
static void update_ifs_map(void) { }
static void mapset(const uchar *set, int code) { }
static int parse_stream(o_string *dest, struct p_context *ctx,
			struct in_str *input, int end_trigger)
{
	ctx->list_head = pending_chain;
	pending_chain = NULL;
	return 0;
}
static int done_word(o_string *dest, struct p_context *ctx) { return 0; }
static int done_pipe(struct p_context *ctx, pipe_style type) { return 0; }
static void b_free(o_string *o) { }
static void b_reset(o_string *o) { }

static int free_pipe_list(struct pipe *head, int indent)
{
	struct pipe *pi, *next;
	int i;

	for (pi = head; pi; pi = next) {
		next = pi->next;
		for (i = 0; i < pi->progs[0].argc; i++)
			free(pi->progs[0].argv[i]);
		free(pi->progs[0].argv);
		free(pi->progs);
		free(pi);
	}
	return 0;
}

enum command_ret_t cmd_process(int flag, int argc, char * const argv[],
			       int *repeatable, ulong *ticks);

/* As in the U-Boot branch of the real run_pipe_real(). */
static int run_pipe_real(struct pipe *pi)
{
	struct child_prog *child = &pi->progs[0];

	return cmd_process(0, child->argc, child->argv, &flag_repeat, NULL);
}

static char *expand(const char *w, int n)
{
	char tmp[64], *v;

	if (w[0] == '$') {
		snprintf(tmp, sizeof(tmp), "%.*s", n - 1, w + 1);
		v = getenv(tmp);
		return strdup(v ? v : "");
	}
	return strndup(w, n);
}

/*
 * Minimal tokenizer: "a b && c ; d" -> pipes of one command each, with
 * PIPE_AND / PIPE_SEQ follow-ups, "$name" from the environment.
 */
static struct pipe *tokenize(const char *s)
{
	struct pipe *head = NULL, **tail = &head, *pi;
	struct child_prog *c;
	const char *p = s, *w;

	while (*p) {
		char *argv[16];
		int argc = 0, followup = PIPE_SEQ;

		for (;;) {
			while (*p == ' ' || *p == '\t' || *p == '\n')
				p++;
			if (!*p)
				break;
			if (!strncmp(p, "&&", 2)) {
				followup = PIPE_AND;
				p += 2;
				break;
			}
			if (*p == ';') {
				p++;
				break;
			}
			w = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
			       *p != ';' && strncmp(p, "&&", 2))
				p++;
			argv[argc++] = expand(w, p - w);
		}
		if (!argc)
			continue;
		pi = calloc(1, sizeof(*pi));
		c = calloc(1, sizeof(*c));
		c->argv = calloc(argc + 1, sizeof(char *));
		memcpy(c->argv, argv, argc * sizeof(char *));
		c->argc = argc;
		pi->num_progs = 1;
		pi->progs = c;
		pi->followup = followup;
		pi->r_mode = RES_NONE;
		*tail = pi;
		tail = &pi->next;
	}
	return head;
}

static int parse_stream_outer(struct in_str *inp, int flag);

int parse_string_outer(const char *s, int flag)
{
	struct in_str in;

	memset(&in, 0, sizeof(in));
	in.peek = static_peek;
	pending_chain = tokenize(s);
	return parse_stream_outer(&in, flag);
}

/* ---------------- simulated commands ---------------- */

static int mmc_rc, sf_rc, bootm_rc;

static int sim_nand(cmd_tbl_t *c, int f, int argc, char * const argv[])
{
	if (argc == 3 && !strcmp(argv[1], "device")) {
		nselect++;
		if (select_rc)
			return select_rc;
		nand_curr_device = strtoul(argv[2], NULL, 10);
		return 0;
	}
	return 1;
}

static int sim_mmc_cmd(cmd_tbl_t *c, int f, int argc, char * const argv[])
{
	return (argc > 1 && !strcmp(argv[1], "write")) ? mmc_rc : 0;
}

static int sim_sf(cmd_tbl_t *c, int f, int argc, char * const argv[])
{
	return (argc > 1 && !strcmp(argv[1], "write")) ? sf_rc : 0;
}

static int sim_bootm(cmd_tbl_t *c, int f, int argc, char * const argv[]) { return bootm_rc; }
static int sim_ok(cmd_tbl_t *c, int f, int argc, char * const argv[]) { return 0; }

static int do_flash(cmd_tbl_t *, int, int, char * const []);
static int do_ubi(cmd_tbl_t *, int, int, char * const []);

static int logged_flash(cmd_tbl_t *c, int f, int argc, char * const argv[])
{
	log_cmd(argc, argv);
	return do_flash(c, f, argc, argv);
}

static int logged_ubi(cmd_tbl_t *c, int f, int argc, char * const argv[])
{
	log_cmd(argc, argv);
	return do_ubi(c, f, argc, argv);
}

#define LOGGED(fn) \
	static int l_##fn(cmd_tbl_t *c, int f, int argc, char * const argv[]) \
	{ log_cmd(argc, argv); return fn(c, f, argc, argv); }
LOGGED(sim_nand) LOGGED(sim_mmc_cmd) LOGGED(sim_sf) LOGGED(sim_bootm)
LOGGED(sim_ok)

static cmd_tbl_t cmd_table[] = {
	{ "flash", 4, 0, logged_flash },
	{ "flasherase", 4, 0, logged_flash },
	{ "ubi", 6, 1, logged_ubi },
	{ "nand", 6, 1, l_sim_nand },
	{ "mmc", 6, 1, l_sim_mmc_cmd },
	{ "sf", 6, 1, l_sim_sf },
	{ "bootm", 6, 1, l_sim_bootm },
	{ "mw", 6, 1, l_sim_ok },
	{ "setenv", 6, 1, l_sim_ok },
	{ "source", 6, 1, l_sim_ok },
};

static cmd_tbl_t *find_cmd(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cmd_table); i++)
		if (!strcmp(cmd_table[i].name, name))
			return &cmd_table[i];
	return NULL;
}

static int cmd_usage(const cmd_tbl_t *c)
{
	printf("Usage: %s ...\n", c->name);
	return 1;
}

int run_command(const char *cmd, int flag);
int ubi_part(char *part_name, const char *vid_header_offset);

/* @@EXTRACTED@@ */

/*
 * "ubi part" back end: attach succeeds unless attach_rc is set.  A failure
 * leaves the previous selection, as the early error returns of ubi_part()
 * (mtdparts/partition lookup) do.
 */
int ubi_part(char *part_name, const char *vid_header_offset)
{
	n_attach++;
	if (attach_rc)
		return attach_rc;
	ubi_dev.selected = 1;
	sim_attach();
	return 0;
}

/* ---------------- scenario helpers ---------------- */

static int cur_failed;
#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "  check failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
	cur_failed = 1; return; } } while (0)

#define IMG	(24 * ES)		/* raw UBI image */

static void reset_all(void)
{
	int i;

	ncmd = 0;
	memset(env_filesize, 0, sizeof(env_filesize));
	memset(env_fileaddr, 0, sizeof(env_fileaddr));
	strcpy(env_mtdids, "nand0=nand0");

	memset(&qca_smem_flash_info, 0, sizeof(qca_smem_flash_info));
	qca_smem_flash_info.flash_type = SMEM_BOOT_NAND_FLASH;
	qca_smem_flash_info.flash_block_size = ES;
	qca_smem_flash_info.rootfs.offset = ROOTFS_BLK * ES;
	qca_smem_flash_info.rootfs.size = ROOTFS_NB * ES;
	flash_type_new = -1;
	which_flash_param = 0;
	memset(&smem_ptable, 0, sizeof(smem_ptable));
	smem_add("0:APPSBL", APPSBL_BLK, 8);
	smem_add("0:APPSBL_1", APPSBL_BLK + 8, 8);
	smem_add("0:MIBIB", 2, 1);
	smem_add("rootfs", ROOTFS_BLK, ROOTFS_NB);

	memset(nand_info, 0, sizeof(nand_info));
	nand_info[0].name = "sim-nand";
	nand_info[0].size = NAND_SIZE;
	nand_info[0].erasesize = ES;
	nand_info[0].writesize = PS;
	nand_curr_device = -1;
	memset(bad, 0, sizeof(bad));
	memset(qerr, 0, sizeof(qerr));
	nquery = nquery_unselected = nselect = select_rc = 0;
	nerase = nwrite = 0;
	fail_erase_at = fail_write_at = ~0u;

	mmc_present = mmc_part_found = 1;
	mmc_rc = sf_rc = bootm_rc = 0;

	for (i = 0; i <= VTBL; i++) {
		if (sim_ubi.volumes[i])
			free(sim_ubi.volumes[i]);
		sim_ubi.volumes[i] = NULL;
	}
	memset(&sim_ubi, 0, sizeof(sim_ubi));
	sim_ubi.vtbl_slots = VTBL;
	sim_ubi.leb_size = LEB;
	sim_ubi.min_io_size = PS;
	sim_ubi.avail_pebs = 400;
	ubi = NULL;
	ubi_devices[0] = NULL;
	memset(&ubi_dev, 0, sizeof(ubi_dev));
	n_attach = n_rootfs_attach = n_remove = n_create = n_upd = 0;
	n_wrdata = 0;
	attach_rc = rootfs_attach_rc = 0;
	fail_remove = fail_create = fail_write = NULL;
	fail_create_err = 0;

	webfailsafe_backup_avail_enabled = 0;
	webfailsafe_auto_reboot_enabled = 1;
	webfailsafe_img_flash = 0;
	cur_flash_type = SMEM_BOOT_NAND_FLASH;
	fw_type = FW_TYPE_UBI;
	last_progress = -1;
	n_reset = n_done = 0;
	upgrade_status = 0;
	memset(&sim_parts, 0, sizeof(sim_parts));
	syntax_errors = 0;
}

/* stock RC01 UBI volumes */
static void stock_volumes(void)
{
	sim_vol("kernel", 0, 40);
	sim_vol("rootfs", 1, 120);
	sim_vol("rootfs_data", 2, 200);
}

static void tar_parts(void)
{
	sim_parts.kernel_data = sim_upload + 0x200;
	sim_parts.kernel_size = 0x400000;
	sim_parts.rootfs_data = sim_upload + 0x8000;
	sim_parts.rootfs_size = 0x800000;
}

/* Run one web upgrade through the real outer dispatcher. */
static void web_upgrade(int type, ulong size)
{
	webfailsafe_is_running = 1;
	webfailsafe_ready_for_upgrade = 1;
	webfailsafe_upgrade_type = type;
	net_boot_file_size = size;
	load_addr = UPLOAD_ADDR;
	failsafe_httpd_poll();
}

static int web_failed(void)
{
	return upgrade_status == 3 && !n_reset && !n_done &&
	       last_progress == WEBFAILSAFE_PROGRESS_UPGRADE_FAILED;
}

static int web_succeeded(void)
{
	return upgrade_status == 4 && n_reset == 1;
}

static void flash_cmd(char *buf, const char *part, ulong size)
{
	sprintf(buf, "flash %s 0x%lx 0x%lx", part, UPLOAD_ADDR, size);
}

/* ---------------- command boundary ---------------- */

static void t_flash_missing_part_fails_in_hush(void)
{
	char buf[128];
	char *argv[] = { "flash", "nosuch", "0x1000", "0x1000", NULL };

	flash_type_new = SMEM_BOOT_QSPI_NAND_FLASH;
	CHECK(do_flash(NULL, 0, 4, argv) == CMD_RET_FAILURE);
	CHECK(flash_type_new == (uint32_t)-1);

	reset_all();
	flash_cmd(buf, "nosuch", 0x1000);
	strcat(buf, " && nand device 1");
	CHECK(run_command(buf, 0) == 1);
	CHECK(ran("nand device") == 0);
	CHECK(nerase == 0 && nwrite == 0);
}

static void t_flash_invalid_part_fails_in_hush(void)
{
	char buf[128];

	smem_add("zero", 400, 0);	/* getpart_offset_size: -EINVAL */
	flash_cmd(buf, "zero", 0x1000);
	strcat(buf, " && nand device 1");
	CHECK(run_command(buf, 0) == 1);
	CHECK(ran("nand device") == 0 && nerase == 0);
}

static void t_flash_usage_kept(void)
{
	char *argv[] = { "flash", "rootfs", "0x1000", NULL };

	flash_type_new = SMEM_BOOT_QSPI_NAND_FLASH;
	CHECK(do_flash(NULL, 0, 3, argv) == CMD_RET_USAGE);
	CHECK(flash_type_new == (uint32_t)-1);
	CHECK(run_command("flash rootfs 0x1000 && nand device 1", 0) == 1);
	CHECK(ran("nand device") == 0);
}

static void t_flash_success_and_erase_failure(void)
{
	char buf[128];

	flash_cmd(buf, "rootfs", IMG);
	CHECK(run_command(buf, 0) == 0);
	CHECK(nerase == 1 && nwrite == 1 && nwrites_at(ROOTFS_BLK) == 1);

	reset_all();
	fail_erase_at = ROOTFS_BLK * ES;
	flash_cmd(buf, "rootfs", IMG);
	strcat(buf, " && nand device 1");
	CHECK(run_command(buf, 0) == 1);
	CHECK(nerase == 1 && nwrite == 0 && ran("nand device 1") == 0);
}

static void t_ubi_create_errno_stops_chain(void)
{
	static const int errs[] = { -ENOSPC, -EROFS, -ENOMEM, -EIO };
	char *argv[] = { "ubi", "create", "kernel", "0x1000", NULL };
	char buf[160];
	int i;

	for (i = 0; i < 4; i++) {
		reset_all();
		ubi_dev.selected = 1;
		sim_attach();
		fail_create = "kernel";
		fail_create_err = errs[i];
		CHECK(do_ubi(NULL, 0, 4, argv) == CMD_RET_FAILURE);
		n_wrdata = 0;
		sprintf(buf, "ubi create kernel 0x1000 && ubi write 0x%lx kernel 0x1000",
			UPLOAD_ADDR);
		CHECK(run_command(buf, 0) == 1);
		CHECK(ran("ubi write") == 0 && n_wrdata == 0);
		CHECK(vol_id("kernel") < 0);
	}

	/* the same errors from the simulated vmt.c conditions */
	reset_all();
	ubi_dev.selected = 1;
	sim_attach();
	sim_ubi.avail_pebs = 1;
	CHECK(run_command("ubi create kernel 0x100000 && ubi write 0x1 kernel 0x1", 0) == 1);
	CHECK(n_create == 1 && n_wrdata == 0);
	sim_ubi.avail_pebs = 100;
	sim_ubi.ro_mode = 1;
	CHECK(run_command("ubi create kernel 0x100000 && ubi write 0x1 kernel 0x1", 0) == 1);
	CHECK(n_wrdata == 0);
}

static void t_ubi_create_success_and_usage(void)
{
	char *create[] = { "ubi", "create", "kernel", "0x1000", NULL };
	char *bad_type[] = { "ubi", "create", "kernel", "0x1000", "x", NULL };
	char *none[] = { "ubi", NULL };

	ubi_dev.selected = 1;
	sim_attach();
	CHECK(do_ubi(NULL, 0, 4, create) == CMD_RET_SUCCESS);
	CHECK(vol_id("kernel") == 0);
	CHECK(do_ubi(NULL, 0, 5, bad_type) == 1);
	CHECK(do_ubi(NULL, 0, 1, none) == CMD_RET_USAGE);
	CHECK(run_command("ubi create rootfs 0x1000 && ubi write 0x1 rootfs 0x1000", 0) == 0);
	CHECK(n_wrdata == 1 && vol_id("rootfs") == 1);
}

/* ---------------- web dispatcher ---------------- */

static void t_web_positive_failure_no_reset(void)
{
	/* "flash 0:APPSBL" fails with an ordinary +1 command status */
	fw_type = FW_TYPE_ELF;
	fail_erase_at = APPSBL_BLK * ES;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_UBOOT, 0x1000);
	CHECK(nerase == 1);
	CHECK(web_failed());
}

static void t_web_success_resets(void)
{
	fw_type = FW_TYPE_ELF;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_UBOOT, 0x1000);
	CHECK(nwrite == 1 && web_succeeded());

	reset_all();
	fw_type = FW_TYPE_ELF;
	webfailsafe_auto_reboot_enabled = 0;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_UBOOT, 0x1000);
	CHECK(upgrade_status == 5 && !n_reset);
}

static void t_web_mibib_initramfs_failures(void)
{
	fw_type = FW_TYPE_MIBIB;
	fail_erase_at = 2 * ES;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_MIBIB, 0x1000);
	CHECK(web_failed());

	reset_all();
	fw_type = FW_TYPE_MIBIB;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_MIBIB, 0x1000);
	CHECK(web_succeeded());

	reset_all();
	fw_type = FW_TYPE_FIT;
	bootm_rc = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_INITRAMFS, 0x1000);
	CHECK(ran("bootm") == 1 && web_failed());
}

/* ---------------- SYSUPGRADE (all boards) ---------------- */

static void t_sysupgrade_create_errno_fails(void)
{
	static const int errs[] = { -ENOSPC, -EROFS, -ENOMEM, -EIO };
	static const char *const vols[] = { "kernel", "rootfs", "rootfs_data" };
	int i, v;

	for (v = 0; v < 3; v++) {
		for (i = 0; i < 4; i++) {
			reset_all();
			stock_volumes();
			tar_parts();
			fw_type = FW_TYPE_SYSUPGRADE;
			fail_create = vols[v];
			fail_create_err = errs[i];
			web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, 0xc00000);
			CHECK(web_failed());
			/* no write into the volume that was not created */
			CHECK(vol_id(vols[v]) < 0);
			CHECK(n_wrdata == v);
			if (v == 0)
				CHECK(vol_id("rootfs") == 1 && ran_eq("ubi remove rootfs") == 0);
		}
	}
}

static void t_sysupgrade_attach_failure(void)
{
	stock_volumes();
	tar_parts();
	attach_rc = 1;
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 0) != 0);
	CHECK(n_attach == 1 && n_remove == 0 && n_create == 0 && n_wrdata == 0);
	CHECK(vol_id("rootfs_data") == 2);
}

static void t_sysupgrade_remove_failure(void)
{
	stock_volumes();
	tar_parts();
	fail_remove = "kernel";
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 0) != 0);
	CHECK(n_create == 0 && n_wrdata == 0);
}

static void t_sysupgrade_data_remove_failure(void)
{
	stock_volumes();
	tar_parts();
	fail_remove = "rootfs_data";
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 0) != 0);
	CHECK(n_create == 0 && n_wrdata == 0 && vol_id("kernel") == 0);
}

static void t_sysupgrade_missing_volume_created(void)
{
	/* no kernel volume and no rootfs_data: nothing to remove, create */
	sim_vol("rootfs", 1, 120);
	tar_parts();
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 0) == 0);
	CHECK(vol_id("kernel") == 0 && vol_id("rootfs") == 1 &&
	      vol_id("rootfs_data") == 2);
	CHECK(ran_eq("ubi remove kernel") == 0 &&
	      ran_eq("ubi remove rootfs_data") == 0);
	CHECK(n_wrdata == 2);
}

static void t_sysupgrade_write_failure(void)
{
	stock_volumes();
	tar_parts();
	fail_write = "rootfs";
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 0) != 0);
	CHECK(vol_id("rootfs_data") < 0 && ran("ubi create rootfs_data") == 0);
}

static void t_sysupgrade_data_create_failure(void)
{
	stock_volumes();
	tar_parts();
	fw_type = FW_TYPE_SYSUPGRADE;
	fail_create = "rootfs_data";
	fail_create_err = -ENOSPC;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, 0xc00000);
	CHECK(n_wrdata == 2 && vol_id("rootfs_data") < 0);
	CHECK(web_failed());
}

static void t_sysupgrade_success_fixed_ids(void)
{
	stock_volumes();
	tar_parts();
	fw_type = FW_TYPE_SYSUPGRADE;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, 0xc00000);
	CHECK(web_succeeded());
	CHECK(vol_id("kernel") == 0 && vol_id("rootfs") == 1 &&
	      vol_id("rootfs_data") == 2);
	CHECK(n_wrdata == 2 && !strcmp(wrdata_name[0], "kernel") &&
	      !strcmp(wrdata_name[1], "rootfs"));
	CHECK(nerase == 0 && nwrite == 0);
}

#ifdef CONFIG_IPQ9574_XIAOMI_BE10000
/* ---------------- RC01 raw UBI ---------------- */

static void t_raw_rc01_single_rootfs_once(void)
{
	/* even if legacy names existed they are not RC01 targets */
	smem_add("rootfs1", ROOTFS1_BLK, ROOTFS_NB);
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_succeeded());
	CHECK(nerase == 1 && nwrite == 1 && nwrites_at(ROOTFS_BLK) == 1);
	CHECK(write_len[0] == IMG);
	CHECK(ran("flash rootfs ") == 1 && ran("flash rootfs1") == 0 &&
	      ran("flash rootfs2") == 0 && ran("flash rootfs_1") == 0);
	CHECK(ubi_ops() == 0);
	/* device selection before the capacity queries */
	CHECK(nquery_unselected == 0 && index_of("nand device") == 0);
}

static void t_raw_rc01_backup_missing_rejected(void)
{
	webfailsafe_backup_avail_enabled = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_failed());
	CHECK(nerase == 0 && nwrite == 0 && ran("flash") == 0);
	CHECK(ubi_ops() == 0);
}

static void t_raw_rc01_backup_present_written(void)
{
	smem_add("rootfs_1", ROOTFS_1_BLK, ROOTFS_NB);
	webfailsafe_backup_avail_enabled = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_succeeded());
	CHECK(nwrites_at(ROOTFS_BLK) == 1 && nwrites_at(ROOTFS_1_BLK) == 1);
	CHECK(nwrite == 2 && ubi_ops() == 0);
}

static void t_raw_rc01_capacity_rejected(void)
{
	int i;

	/* primary: 41 of 64 blocks bad, 23 good < 24 needed */
	for (i = 0; i < 41; i++)
		bad[ROOTFS_BLK + i] = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_failed() && nerase == 0 && nwrite == 0);

	/* exactly enough is accepted */
	reset_all();
	for (i = 0; i < 40; i++)
		bad[ROOTFS_BLK + i] = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_succeeded() && nwrite == 1);

	/* backup target too small: checked before the primary erase */
	reset_all();
	smem_add("rootfs_1", ROOTFS_1_BLK, 16);
	webfailsafe_backup_avail_enabled = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_failed() && nerase == 0 && nwrite == 0);

	/* failing bad block query in the backup range */
	reset_all();
	smem_add("rootfs_1", ROOTFS_1_BLK, ROOTFS_NB);
	qerr[ROOTFS_1_BLK + 63] = 1;
	webfailsafe_backup_avail_enabled = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_failed() && nerase == 0 && nwrite == 0);

	/* image larger than the partition */
	reset_all();
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, (ROOTFS_NB + 1) * ES);
	CHECK(web_failed() && nerase == 0 && nwrite == 0);

	/* device selection fails */
	reset_all();
	select_rc = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_failed() && nquery == 0 && nerase == 0 && nwrite == 0);
}

static void t_raw_rc01_second_target_failure(void)
{
	smem_add("rootfs_1", ROOTFS_1_BLK, ROOTFS_NB);
	webfailsafe_backup_avail_enabled = 1;
	fail_write_at = ROOTFS_1_BLK * ES;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(nwrites_at(ROOTFS_BLK) == 1 && nwrites_at(ROOTFS_1_BLK) == 1);
	CHECK(web_failed());

	/* first target failure stops before the second */
	reset_all();
	smem_add("rootfs_1", ROOTFS_1_BLK, ROOTFS_NB);
	webfailsafe_backup_avail_enabled = 1;
	fail_erase_at = ROOTFS_BLK * ES;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_failed() && nerase == 1 && nwrite == 0);
}

static void t_sysupgrade_rc01_backup_rejected(void)
{
	stock_volumes();
	tar_parts();
	fw_type = FW_TYPE_SYSUPGRADE;
	webfailsafe_backup_avail_enabled = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, 0xc00000);
	CHECK(web_failed());
	CHECK(ubi_ops() == 0 && ran("ubi") == 0);
	CHECK(vol_id("kernel") == 0 && vol_id("rootfs") == 1 &&
	      vol_id("rootfs_data") == 2 && vol_id("kernel_1") < 0);
}

static void t_sysupgrade_rc01_volume_ids_checked(void)
{
	/* kernel at an unexpected ID: automatic re-creation takes ID 2 */
	sim_vol("foo", 0, 10);
	sim_vol("rootfs", 1, 120);
	sim_vol("rootfs_data", 2, 200);
	sim_vol("kernel", 3, 40);
	tar_parts();
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 0) != 0);
	CHECK(vol_id("kernel") == 2);
}
#else
/* ---------------- other pure NAND boards ---------------- */

static void t_raw_other_single_rootfs(void)
{
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_succeeded());
	CHECK(nwrite == 1 && nwrites_at(ROOTFS_BLK) == 1);
	CHECK(ran("flash rootfs1") == 0 && ubi_ops() == 0);
}

static void t_raw_other_legacy_copies(void)
{
	smem_add("rootfs1", ROOTFS1_BLK, ROOTFS_NB);
	smem_add("rootfs2", ROOTFS2_BLK, ROOTFS_NB);
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_succeeded());
	CHECK(nwrite == 3 && write_off[0] == ROOTFS_BLK * ES &&
	      write_off[1] == ROOTFS1_BLK * ES &&
	      write_off[2] == ROOTFS2_BLK * ES);

	reset_all();
	smem_add("rootfs1", ROOTFS1_BLK, ROOTFS_NB);
	smem_add("rootfs2", ROOTFS2_BLK, ROOTFS_NB);
	fail_write_at = ROOTFS2_BLK * ES;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(nwrite == 3 && web_failed());
}

static void t_raw_other_backup_missing_rejected(void)
{
	webfailsafe_backup_avail_enabled = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_failed() && nerase == 0 && nwrite == 0 && ubi_ops() == 0);
}

static void t_raw_other_backup_written(void)
{
	smem_add("rootfs_1", ROOTFS_1_BLK, ROOTFS_NB);
	webfailsafe_backup_avail_enabled = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_succeeded() && nwrite == 2 && nwrites_at(ROOTFS_1_BLK) == 1);
}

static void t_raw_norplusnand_route_unchanged(void)
{
	/* NOR+NAND: prefix fallback to the secondary NAND, old chain */
	char exp[128];

	cur_flash_type = SMEM_BOOT_NORPLUSNAND;
	qca_smem_flash_info.flash_type = SMEM_BOOT_SPI_FLASH;
	qca_smem_flash_info.flash_secondary_type = SMEM_BOOT_NAND_FLASH;
	qca_smem_flash_info.rootfs.offset = 0xBAD0FF5E;
	smem_ptable.len = 0;
	setenv_hex("filesize", IMG);
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, IMG);
	CHECK(web_succeeded());
	CHECK(ncmd >= 3 && nwrite == 3);
	sprintf(exp, "flash rootfs 0x%lx %lx", UPLOAD_ADDR, (ulong)IMG);
	CHECK(!strcmp(cmdlog[0], exp));
	sprintf(exp, "flash rootfs1 0x%lx %lx", UPLOAD_ADDR, (ulong)IMG);
	CHECK(!strcmp(cmdlog[2], exp));
}

static void t_sysupgrade_backup_written(void)
{
	stock_volumes();
	tar_parts();
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 1) == 0);
	CHECK(vol_id("kernel_1") >= 0 && vol_id("rootfs_1") >= 0);
	CHECK(vol_id("rootfs_data") >= 0 && n_wrdata == 4);
}

static void t_sysupgrade_backup_failure_fails(void)
{
	static const int errs[] = { -ENOSPC, -EROFS, -ENOMEM, -EIO };
	int i;

	for (i = 0; i < 4; i++) {
		reset_all();
		stock_volumes();
		tar_parts();
		fw_type = FW_TYPE_SYSUPGRADE;
		webfailsafe_backup_avail_enabled = 1;
		fail_create = i & 1 ? "rootfs_1" : "kernel_1";
		fail_create_err = errs[i];
		web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, 0xc00000);
		CHECK(web_failed());
		CHECK(vol_id(fail_create) < 0);
	}

	reset_all();
	stock_volumes();
	tar_parts();
	fail_write = "rootfs_1";
	CHECK(sysupgrade_write_ubi_volumes(&sim_parts, 1) != 0);
}

/* ---------------- MMC / NOR routes ---------------- */

static void t_flash_mmc_route(void)
{
	char buf[128];

	flash_type_new = SMEM_BOOT_MMC_FLASH;
	flash_cmd(buf, "0:HLOS", 513);
	CHECK(run_command(buf, 0) == 0);
	CHECK(ran("mmc erase 0x100 0x200") == 1 && ran("mmc write") == 1);
	sprintf(buf, "mmc write 0x%lx 0x100 0x2", UPLOAD_ADDR & 0xffffffff);
	CHECK(ran(buf) == 1);

	reset_all();
	flash_type_new = SMEM_BOOT_MMC_FLASH;
	mmc_rc = 1;
	flash_cmd(buf, "0:HLOS", 513);
	CHECK(run_command(buf, 0) == 1);

	/* missing eMMC partition keeps the web "skipped" success */
	reset_all();
	flash_type_new = SMEM_BOOT_MMC_FLASH;
	mmc_part_found = 0;
	flash_cmd(buf, "0:HLOS_1", 513);
	CHECK(run_command(buf, 0) == 0 && ran("mmc") == 0);
}

static void t_flash_nor_route(void)
{
	char buf[128];

	qca_smem_flash_info.flash_type = SMEM_BOOT_SPI_FLASH;
	qca_smem_flash_info.flash_block_size = 0x10000;
	smem_add("0:HLOS", 0x20, 0x40);
	flash_cmd(buf, "0:HLOS", 513);
	CHECK(run_command(buf, 0) == 0);
	CHECK(ran("sf probe") == 1 && ran("sf erase 0x200000 0x400000") == 1);
	sprintf(buf, "sf write 0x%lx 0x200000 0x201", UPLOAD_ADDR & 0xffffffff);
	CHECK(ran(buf) == 1);

	reset_all();
	qca_smem_flash_info.flash_type = SMEM_BOOT_SPI_FLASH;
	qca_smem_flash_info.flash_block_size = 0x10000;
	smem_add("0:HLOS", 0x20, 0x40);
	sf_rc = 1;
	flash_cmd(buf, "0:HLOS", 513);
	CHECK(run_command(buf, 0) == 1);
}

static void t_flash_nor_missing_part_fails(void)
{
	char buf[128];

	qca_smem_flash_info.flash_type = SMEM_BOOT_SPI_FLASH;
	qca_smem_flash_info.flash_block_size = 0x10000;
	flash_cmd(buf, "0:NOSUCH", 513);
	strcat(buf, " && sf probe");
	CHECK(run_command(buf, 0) == 1 && ran("sf") == 0);
}

static void t_web_nor_failure_no_reset(void)
{
	char exp[128];

	cur_flash_type = SMEM_BOOT_NOR_FLASH;
	fw_type = FW_TYPE_FIT;
	sf_rc = 1;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, 0x1000);
	sprintf(exp, "sf write 0x%lx 0x%lx 0x%lx", UPLOAD_ADDR,
		NOR_FIRMWARE_START, 0x1000ul);
	CHECK(ran("sf erase 0x100000 0xe00000") == 1 && ran(exp) == 1);
	CHECK(web_failed());

	reset_all();
	cur_flash_type = SMEM_BOOT_NOR_FLASH;
	fw_type = FW_TYPE_FIT;
	web_upgrade(WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE, 0x1000);
	CHECK(web_succeeded());
}
#endif

#define RUN(t) do { reset_all(); cur_failed = 0; t(); \
	fprintf(stderr, "%s %s\n", cur_failed ? "FAIL" : "PASS", #t); } while (0)

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	RUN(t_flash_missing_part_fails_in_hush);
	RUN(t_flash_invalid_part_fails_in_hush);
	RUN(t_flash_usage_kept);
	RUN(t_flash_success_and_erase_failure);
	RUN(t_ubi_create_errno_stops_chain);
	RUN(t_ubi_create_success_and_usage);
	RUN(t_web_positive_failure_no_reset);
	RUN(t_web_success_resets);
	RUN(t_web_mibib_initramfs_failures);
	RUN(t_sysupgrade_create_errno_fails);
	RUN(t_sysupgrade_attach_failure);
	RUN(t_sysupgrade_remove_failure);
	RUN(t_sysupgrade_data_remove_failure);
	RUN(t_sysupgrade_missing_volume_created);
	RUN(t_sysupgrade_write_failure);
	RUN(t_sysupgrade_data_create_failure);
	RUN(t_sysupgrade_success_fixed_ids);
#ifdef CONFIG_IPQ9574_XIAOMI_BE10000
	RUN(t_raw_rc01_single_rootfs_once);
	RUN(t_raw_rc01_backup_missing_rejected);
	RUN(t_raw_rc01_backup_present_written);
	RUN(t_raw_rc01_capacity_rejected);
	RUN(t_raw_rc01_second_target_failure);
	RUN(t_sysupgrade_rc01_backup_rejected);
	RUN(t_sysupgrade_rc01_volume_ids_checked);
#else
	RUN(t_raw_other_single_rootfs);
	RUN(t_raw_other_legacy_copies);
	RUN(t_raw_other_backup_missing_rejected);
	RUN(t_raw_other_backup_written);
	RUN(t_raw_norplusnand_route_unchanged);
	RUN(t_sysupgrade_backup_written);
	RUN(t_sysupgrade_backup_failure_fails);
	RUN(t_flash_mmc_route);
	RUN(t_flash_nor_route);
	RUN(t_flash_nor_missing_part_fails);
	RUN(t_web_nor_failure_no_reset);
#endif
	return 0;
}
