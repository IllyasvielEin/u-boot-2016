/* SPDX-License-Identifier: GPL-2.0 */
/* Full production hush parser; only the ENV table and save backend are stubs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
typedef unsigned char uchar;
typedef unsigned long ulong;
#define CONFIG_SYS_HUSH_PARSER 1
#define CONFIG_SYS_CBSIZE 1024
#define CONFIG_SYS_MAXARGS 32
#define CONFIG_SYS_PROMPT "=> "
#define DECLARE_GLOBAL_DATA_PTR
#define U_BOOT_CMD(...)
#define CMD_FLAG_REPEAT 1
#define CMD_FLAG_ENV 4
#define debug(...) do {} while (0)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define simple_strtoul strtoul
#define H_FORCE 1
#define H_INTERACTIVE 2
typedef struct { char *key; char *data; } ENTRY;
enum { FIND, ENTER };
static int env_id, env_htab, saves, first_fail, save_fail, exists;
static ENTRY stored;
static char *fake_getenv(const char *s) { (void)s; return NULL; }
#define getenv fake_getenv
static int hdelete_r(const char *n, void *h, int f)
{
	(void)n; (void)h; (void)f;
	if (first_fail || !exists) return 0;
	exists = 0;
	return 1;
}
static int hsearch_r(ENTRY e, int a, ENTRY **r, void *h, int f)
{
	(void)e; (void)a; (void)h; (void)f;
	*r = first_fail ? NULL : &stored;
	exists = !first_fail;
	return !first_fail;
}
typedef struct cmd_tbl_s {
	const char *name;
	int maxargs, repeatable;
	int (*cmd)(struct cmd_tbl_s *, int, int, char *const []);
} cmd_tbl_t;
enum command_ret_t { CMD_RET_SUCCESS, CMD_RET_FAILURE, CMD_RET_USAGE = -1 };
static int cmd_usage(const cmd_tbl_t *c) { (void)c; puts("usage"); return 1; }
static int ctrlc(void) { return 0; }
static int had_ctrlc(void) { return 0; }
static void clear_ctrlc(void) {}
static void bootretry_reset_cmd_timeout(void) {}
static int cli_readline(const char *s) { (void)s; return -1; }
static char console_buffer[1024];
static ulong get_timer(ulong b) { (void)b; return 0; }
static void set_default_env(const char *msg) { puts(msg); }
static void set_default_vars(int n, char *const v[]) { (void)n; (void)v; }
static cmd_tbl_t *find_cmd(const char *s);
enum command_ret_t cmd_process(int, int, char *const [], int *, ulong *);

/* @@EXTRACTED@@ */

static int do_save(cmd_tbl_t *c, int f, int n, char *const v[])
{
	(void)c; (void)f; (void)n; (void)v;
	++saves;
	puts(save_fail ? "Saving Environment... FAILED!" : "Saving Environment... OK");
	return save_fail;
}
static int do_env(cmd_tbl_t *c, int f, int n, char *const v[])
{
	return do_env_default(c, f, n - 1, v + 1);
}
static cmd_tbl_t cmds[] = {
	{ "setenv", 32, 0, do_env_set },
	{ "saveenv", 1, 0, do_save },
	{ "env", 32, 1, do_env },
};
static cmd_tbl_t *find_cmd(const char *s)
{
	unsigned i;
	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (!strcmp(s, cmds[i].name)) return &cmds[i];
	return NULL;
}
int main(int argc, char **argv)
{
	int r;
	if (argc != 5) return 2;
	exists = atoi(argv[2]);
	first_fail = atoi(argv[3]);
	save_fail = atoi(argv[4]);
	u_boot_hush_start();
	r = run_command(argv[1], 0);
	fprintf(stderr, "RESULT rc=%d saves=%d exists=%d\n", r, saves, exists);
	return 0;
}
