/*
 * Host fixture for the RC01 UB03 /env_set web handler.
 *
 * test_env_save.py extracts the real httpd_handle_env_set() from
 * failsafe/failsafe_httpd.c (by symbol) and inserts it at @@EXTRACTED@@.
 * setenv/saveenv/env_reset_gpio_invalidate and the HTTP send are simulated:
 * a two-variable RAM environment, a "flash" copy updated only by a
 * successful saveenv, and the reset_key GPIO cache of lib/ipq_api.c
 * (valid until invalidated, then re-read from RAM).
 *
 * "<exe> --list" lists the scenarios, "<exe> <name>" runs one; the result
 * is "PASS <name>" or "FAIL <name>" on stderr.
 *
 * SPDX-License-Identifier:	GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* the U-Boot setenv(name, value), not the libc one */
#define setenv uboot_setenv

struct failsafe_httpd_state { int dummy; };

static char sent[512];
static int sent_len, send_calls;
static void httpd_send_static(struct failsafe_httpd_state *hs, const char *buf,
			      int len)
{
	send_calls++;
	sent_len = len;
	if (len < 0 || len > (int)sizeof(sent) - 1)
		len = sizeof(sent) - 1;
	memcpy(sent, buf, len);
	sent[len] = '\0';
}

/* RAM and persisted environment */
static char ram_reset_key[256], ram_config_name[256];
static char nv_reset_key[256], nv_config_name[256];
static int setenv_calls, setenv_fail, saveenv_calls, saveenv_fail;
static int invalidate_calls;
static const char *last_set_value;
static int last_set_was_delete;

static char *var(char *ram_rk, char *ram_cn, const char *name)
{
	if (!strcmp(name, "reset_key"))
		return ram_rk;
	if (!strcmp(name, "config_name"))
		return ram_cn;
	return NULL;
}

int setenv(const char *name, const char *value)
{
	char *v;

	setenv_calls++;
	last_set_was_delete = value == NULL;
	last_set_value = value;
	if (setenv_fail)
		return 1;
	v = var(ram_reset_key, ram_config_name, name);
	if (!v)
		return 1;
	snprintf(v, 256, "%s", value ? value : "");
	return 0;
}

int saveenv(void)
{
	saveenv_calls++;
	if (saveenv_fail)
		return 1;
	strcpy(nv_reset_key, ram_reset_key);
	strcpy(nv_config_name, ram_config_name);
	return 0;
}

/* lib/ipq_api.c: cached reset GPIO, -2 = re-read from RAM */
static int gpio_cache = -2;
void env_reset_gpio_invalidate(void)
{
	invalidate_calls++;
	gpio_cache = -2;
}
static int env_reset_gpio(void)
{
	if (gpio_cache == -2)
		gpio_cache = ram_reset_key[0] ? atoi(ram_reset_key) : -1;
	return gpio_cache;
}

/* @@EXTRACTED@@ */

#define CHECK(c) do { if (!(c)) { \
	fprintf(stdout, "  check failed (%s:%d): %s\n", __FILE__, __LINE__, #c); \
	return false; } } while (0)

static void setup(void)
{
	strcpy(ram_reset_key, "5");
	strcpy(nv_reset_key, "5");
	strcpy(ram_config_name, "config@old");
	strcpy(nv_config_name, "config@old");
	setenv_calls = setenv_fail = saveenv_calls = saveenv_fail = 0;
	invalidate_calls = send_calls = sent_len = 0;
	last_set_value = NULL;
	gpio_cache = -2;
	env_reset_gpio();		/* cache holds 5 */
	sent[0] = '\0';
}

static const char *reply(void)
{
	const char *b = strstr(sent, "\r\n\r\n");

	return b ? b + 4 : "";
}

static bool post(const char *body)
{
	static char req[4096];
	char *data;
	int n;

	n = snprintf(req, sizeof(req), "POST /env_set HTTP/1.1\r\nHost: x\r\n"
		     "Content-Type: text/plain\r\n\r\n%s", body);
	/* exact size heap copy, as httpd_recv() hands it over */
	data = malloc(n + 1);
	memcpy(data, req, n + 1);
	httpd_handle_env_set(NULL, data, n);
	free(data);
	return send_calls == 1;
}

static bool t_set_config_name_ok(void)
{
	setup();
	CHECK(post("config_name=config@new"));
	CHECK(!strcmp(reply(), "ok"));
	CHECK(setenv_calls == 1 && saveenv_calls == 1);
	CHECK(!strcmp(nv_config_name, "config@new"));
	CHECK(!invalidate_calls && env_reset_gpio() == 5);
	return true;
}

static bool t_set_reset_key_ok(void)
{
	setup();
	CHECK(post("reset_key=7"));
	CHECK(!strcmp(reply(), "ok"));
	CHECK(!strcmp(nv_reset_key, "7") && env_reset_gpio() == 7);
	return true;
}

static bool t_restore_config_name_ok(void)
{
	setup();
	CHECK(post("config_name="));
	CHECK(!strcmp(reply(), "ok"));
	CHECK(last_set_was_delete && saveenv_calls == 1);
	CHECK(!nv_config_name[0]);
	return true;
}

static bool t_restore_reset_key_ok(void)
{
	setup();
	CHECK(post("reset_key="));
	CHECK(!strcmp(reply(), "ok"));
	CHECK(last_set_was_delete && !nv_reset_key[0]);
	CHECK(env_reset_gpio() == -1);
	return true;
}

static bool t_amp_terminated_value(void)
{
	setup();
	CHECK(post("reset_key=9&x=1"));
	CHECK(!strcmp(reply(), "ok") && !strcmp(nv_reset_key, "9"));
	return true;
}

static bool t_setenv_fail_no_save(void)
{
	setup();
	setenv_fail = 1;
	CHECK(post("config_name=config@new"));
	CHECK(strcmp(reply(), "ok") != 0);
	CHECK(setenv_calls == 1 && saveenv_calls == 0);
	CHECK(!strcmp(nv_config_name, "config@old"));
	return true;
}

static bool t_setenv_fail_reset_key(void)
{
	setup();
	setenv_fail = 1;
	CHECK(post("reset_key=7"));
	CHECK(strcmp(reply(), "ok") != 0 && saveenv_calls == 0);
	/* RAM unchanged, cache still matches it */
	CHECK(env_reset_gpio() == 5);
	return true;
}

static bool t_save_fail_not_ok(void)
{
	setup();
	saveenv_fail = 1;
	CHECK(post("config_name=config@new"));
	CHECK(strcmp(reply(), "ok") != 0);
	CHECK(saveenv_calls == 1);
	/* RAM changed, flash not: the reply must not claim persistence */
	CHECK(!strcmp(nv_config_name, "config@old"));
	return true;
}

static bool t_reset_key_save_fail_cache(void)
{
	setup();
	saveenv_fail = 1;
	CHECK(post("reset_key=7"));
	CHECK(strcmp(reply(), "ok") != 0);
	CHECK(!strcmp(ram_reset_key, "7") && !strcmp(nv_reset_key, "5"));
	/* the cache follows RAM, not the stale value */
	CHECK(invalidate_calls >= 1 && env_reset_gpio() == 7);
	return true;
}

static bool t_unknown_name_rejected(void)
{
	setup();
	CHECK(post("bootcmd=reset"));
	CHECK(!strcmp(reply(), "error") && !setenv_calls && !saveenv_calls);
	return true;
}

static bool t_no_body_rejected(void)
{
	static char req[] = "POST /env_set HTTP/1.1\r\nHost: x\r\n";
	char *data = malloc(sizeof(req));

	setup();
	memcpy(data, req, sizeof(req));
	httpd_handle_env_set(NULL, data, sizeof(req) - 1);
	free(data);
	CHECK(send_calls == 1 && !strcmp(reply(), "error"));
	CHECK(!setenv_calls && !saveenv_calls);
	return true;
}

static bool t_long_name_rejected(void)
{
	char body[400];

	setup();
	memset(body, 'n', 200);
	strcpy(body + 200, "=1");
	CHECK(post(body));
	CHECK(!strcmp(reply(), "error") && !setenv_calls && !saveenv_calls);
	return true;
}

static bool t_long_value_rejected(void)
{
	char body[600];

	setup();
	strcpy(body, "config_name=");
	memset(body + 12, 'v', 400);
	body[412] = '\0';
	CHECK(post(body));
	CHECK(!strcmp(reply(), "error") && !setenv_calls && !saveenv_calls);

	/* exactly 128 value bytes: one more than the buffer holds */
	setup();
	memset(body + 12, 'v', 128);
	body[140] = '\0';
	CHECK(post(body));
	CHECK(!strcmp(reply(), "error") && !setenv_calls);

	/* 127 bytes still fit */
	setup();
	memset(body + 12, 'v', 127);
	body[139] = '\0';
	CHECK(post(body));
	CHECK(!strcmp(reply(), "ok") && strlen(nv_config_name) == 127);
	return true;
}

static bool t_long_name_with_amp_rejected(void)
{
	char body[400];

	setup();
	memset(body, 'n', 100);
	strcpy(body + 100, "=1&reset_key=3");
	CHECK(post(body));
	CHECK(!strcmp(reply(), "error") && !setenv_calls);
	return true;
}

#define SCENARIOS(X) \
	X(t_set_config_name_ok) X(t_set_reset_key_ok) \
	X(t_restore_config_name_ok) X(t_restore_reset_key_ok) \
	X(t_amp_terminated_value) X(t_setenv_fail_no_save) \
	X(t_setenv_fail_reset_key) X(t_save_fail_not_ok) \
	X(t_reset_key_save_fail_cache) X(t_unknown_name_rejected) \
	X(t_no_body_rejected) X(t_long_name_rejected) \
	X(t_long_value_rejected) X(t_long_name_with_amp_rejected)

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
