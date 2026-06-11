/*
 * ap2p — config.c
 *
 * Runtime config parser.  Source is an MQTT case-81 retained payload (NOT a
 * disk file): the broker auto-delivers the bytes on subscribe.  The MQTT
 * thread calls apply_ap2p_conf_payload(state, bytes, len).
 *
 * Format is flat KEY=value, one per line, # for comments.  17 keys:
 *   NODE_ID, CTRL_HOST, CTRL_PORT, CTRL_SCHEME, CTRL_TOKEN,
 *   EDGE_HOST, EDGE_PORT,
 *   RELAY_HOST, RELAY_PORT, RELAY_USER, RELAY_PASS,
 *   SRC_HOST, SRC_PORT, SRC_PATH, SRC_AUTH,
 *   LATENCY_MS, VERBOSE.
 * Unknown keys: silently skipped (forward-compat for broker-pushed extras).
 *
 * On success, signals state->state_ready_cv (so the SRT thread can start
 * the first time) and sets state->srt_reload_pending (so subsequent payloads
 * cause a graceful tear-down + rebuild).  Returns 0 on success, -1 if the
 * payload was malformed (no CTRL_HOST after parsing).
 *
 * Bootstrap loader (config.json) is a separate function used by main().
 */
#include "state.h"
#include "boot_timing.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <pthread.h>

/* key → field offset/type table.  STR fields carry a buffer size; INT
 * fields use sz = 0 as a sentinel. */
typedef enum { F_STR, F_INT } field_kind_t;

typedef struct {
    const char  *key;
    field_kind_t kind;
    size_t       offset;   /* offsetof in ap2p_state_t */
    size_t       sz;       /* buffer size for STR */
} field_t;

#define F_STR_ENT(K, FIELD) { K, F_STR, offsetof(ap2p_state_t, FIELD), sizeof(((ap2p_state_t*)0)->FIELD) }
#define F_INT_ENT(K, FIELD) { K, F_INT, offsetof(ap2p_state_t, FIELD), 0 }

static const field_t kFields[] = {
    F_STR_ENT("NODE_ID",     node_id),
    F_STR_ENT("CTRL_HOST",   ctrl_host),
    F_INT_ENT("CTRL_PORT",   ctrl_port),
    F_STR_ENT("CTRL_SCHEME", ctrl_scheme),
    F_STR_ENT("CTRL_TOKEN",  ctrl_token),
    F_STR_ENT("EDGE_HOST",   edge_host),
    F_INT_ENT("EDGE_PORT",   edge_port),
    F_STR_ENT("RELAY_HOST",  relay_host),
    F_INT_ENT("RELAY_PORT",  relay_port),
    F_STR_ENT("RELAY_USER",  relay_user),
    F_STR_ENT("RELAY_PASS",  relay_pass),
    F_STR_ENT("SRC_HOST",    src_host),
    F_INT_ENT("SRC_PORT",    src_port),
    F_STR_ENT("SRC_PATH",    src_path),
    F_STR_ENT("SRC_AUTH",    src_auth),
    F_INT_ENT("LATENCY_MS",  latency_ms),
    F_INT_ENT("VERBOSE",     verbose),
};
#define N_FIELDS (sizeof(kFields)/sizeof(kFields[0]))

/* Trim ASCII whitespace in-place; returns pointer to first non-space char
 * (the original buffer's interior).  Cuts a trailing '#' comment off. */
static char *trim(char *s)
{
    if (!s) return s;
    char *hash = strchr(s, '#');
    if (hash) *hash = '\0';
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static void apply_field(ap2p_state_t *state, const field_t *f, const char *val)
{
    char *base = (char *)state + f->offset;
    if (f->kind == F_STR) {
        strncpy(base, val, f->sz - 1);
        base[f->sz - 1] = '\0';
    } else {
        *(int *)base = atoi(val);
    }
}

int apply_ap2p_conf_payload(ap2p_state_t *state, const char *payload, size_t len)
{
    if (!state || !payload || len == 0) return -1;

    /* Idempotent — only the FIRST retained-payload landing matters for
     * boot-timing.  Subsequent updates are config rotations, not boots. */
    bt_mark(BT_CASE81_RX);

    pthread_mutex_lock(&state->lock);

    /* Walk the payload one line at a time.  Copy each line to a local
     * buffer so trim() can mutate it without touching the caller's memory. */
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && payload[j] != '\n' && payload[j] != '\r') j++;
        size_t linelen = j - i;
        if (linelen >= sizeof(((char[1024]){0}))) linelen = 1023;
        char line[1024];
        memcpy(line, payload + i, linelen);
        line[linelen] = '\0';

        /* advance past terminator(s) */
        i = j;
        while (i < len && (payload[i] == '\n' || payload[i] == '\r')) i++;

        char *p = trim(line);
        if (!*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        for (size_t k = 0; k < N_FIELDS; k++) {
            if (strcmp(key, kFields[k].key) == 0) {
                apply_field(state, &kFields[k], val);
                break;
            }
            /* unknown keys silently skipped */
        }
    }

    /* Sanity: CTRL_HOST is the minimum signal that the payload is real.
     * If still empty after parsing, reject without flipping any flags. */
    if (state->ctrl_host[0] == '\0') {
        pthread_mutex_unlock(&state->lock);
        return -1;
    }

    int was_first = !state->state_ready;
    state->state_ready = 1;
    if (!was_first) {
        /* Subsequent payload = config update; tell SRT thread to rebuild. */
        state->srt_reload_pending = 1;
    }
    pthread_cond_broadcast(&state->state_ready_cv);
    pthread_mutex_unlock(&state->lock);
    if (was_first) bt_mark(BT_STATE_READY);
    return 0;
}

/* Bootstrap loader — parses config.json (tiny JSON: just mqttUrl, username,
 * password, productType, plus optionally a top-level SERVICE_ID-like key).
 *
 * We don't link a full JSON library here — a 50-line ad-hoc parser is
 * sufficient for the four flat string fields we need.  When Phase A wires
 * cJSON in, this implementation may be swapped for a real parse. */
static int extract_json_string(const char *src, const char *key,
                                char *out, size_t out_sz)
{
    /* Search for "key" : "value" — tolerant of whitespace. */
    size_t klen = strlen(key);
    /* DEFENSIVE: empty key would make strstr return src on every call AND
     * p += klen would be a no-op → infinite loop.  This happens if a brand
     * scrubber zero-fills the literal we passed in, which is exactly how
     * v2.0.0-rc1 hung.  Fail fast instead. */
    if (klen == 0) return -1;
    const char *p = src;
    while ((p = strstr(p, key)) != NULL) {
        /* Check it's a real key — preceded by '"' and followed by '"' */
        if (p > src && *(p - 1) == '"' && p[klen] == '"') break;
        p += klen;
    }
    if (!p) return -1;
    p += klen;
    /* skip "..": */
    while (*p && *p != ':') p++;
    if (!*p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    /* copy until closing quote */
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

#define BSTRACE(...) do { \
    fprintf(stderr, "[ap2p:bootstrap] " __VA_ARGS__); \
    fputc('\n', stderr); \
    fflush(stderr); \
} while (0)

int ap2p_state_load_bootstrap(ap2p_state_t *state, const char *config_json_path)
{
    BSTRACE("entered path=%s", config_json_path);
    if (!state || !config_json_path) { BSTRACE("bad args"); return -1; }
    BSTRACE("fopen");
    FILE *fp = fopen(config_json_path, "r");
    if (!fp) { BSTRACE("fopen failed errno=%d", errno); return -1; }
    BSTRACE("fread");
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    BSTRACE("fread returned %zu", n);
    fclose(fp);
    if (n == 0) { BSTRACE("empty file"); return -1; }
    buf[n] = '\0';
    BSTRACE("buf null-terminated; len=%zu first_byte=0x%02x last_byte=0x%02x",
            n, (unsigned char)buf[0], (unsigned char)buf[n - 1]);

    BSTRACE("extracting mqttUrl");
    char url[256] = {0};
    if (extract_json_string(buf, "mqttUrl", url, sizeof(url)) != 0) {
        BSTRACE("mqttUrl extraction FAILED");
        return -1;
    }
    BSTRACE("mqttUrl = '%s'", url);
    strncpy(state->bootstrap_mqtt_url, url, sizeof(state->bootstrap_mqtt_url) - 1);

    /* Optional backup broker URL.  Missing → empty string, no failover. */
    BSTRACE("extracting mqttUrl_BKP (optional)");
    if (extract_json_string(buf, "mqttUrl_BKP",
                             state->bootstrap_mqtt_url_bkp,
                             sizeof(state->bootstrap_mqtt_url_bkp)) == 0) {
        BSTRACE("mqttUrl_BKP = '%s'", state->bootstrap_mqtt_url_bkp);
    } else {
        state->bootstrap_mqtt_url_bkp[0] = '\0';
        BSTRACE("mqttUrl_BKP not present (no failover broker)");
    }

    BSTRACE("extracting username");
    extract_json_string(buf, "username", state->bootstrap_mqtt_user,
                        sizeof(state->bootstrap_mqtt_user));
    BSTRACE("username = '%s'", state->bootstrap_mqtt_user);

    BSTRACE("extracting password");
    extract_json_string(buf, "password", state->bootstrap_mqtt_pass,
                        sizeof(state->bootstrap_mqtt_pass));
    BSTRACE("password length = %zu", strlen(state->bootstrap_mqtt_pass));

    /* IMPORTANT: do NOT look up "SERVICE_ID" / "serviceId" here.  The brand
     * scrubber zero-fills any "SERVICE_ID" byte sequence in .rodata to keep
     * `strings ap2p` clean, including the legitimate JSON-key literal we'd
     * pass to extract_json_string().  That leaves us calling
     * strstr(buf, "") → loops forever with klen=0.
     *
     * Main.c falls back to reading /mny/mtd/ipc/BurnUID for the node_id —
     * always present on Augentix flash — so we don't actually need this
     * lookup path.  Leaving state->node_id empty here is correct. */
    BSTRACE("returning 0 (node_id will come from BurnUID fallback in main)");
    bt_mark(BT_CONFIG_LOADED);
    return 0;
}
