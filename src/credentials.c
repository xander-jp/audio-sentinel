#include "credentials.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

//=============================================================================
// On-flash layout (see credentials.h for the schema doc).
//=============================================================================
#define CREDS_MAGIC    0xCA112024u
// Bump when on-flash layout changes — a mismatch makes creds_load() return
// "no creds" so the device falls back to BLE provisioning mode instead of
// reading stale fields at the wrong offsets.
#define CREDS_VERSION  3u

// One sector at the very top of flash.
#define CREDS_FLASH_OFFSET   (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CREDS_FLASH_XIP_ADDR ((const uint8_t *)(XIP_BASE + CREDS_FLASH_OFFSET))

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint8_t  ssid_len;
    uint8_t  pass_len;
    uint8_t  device_id_len;
    uint8_t  opus_enabled;       // 0 = off, non-zero = on
    char     ssid[CREDS_SSID_MAX];
    char     password[CREDS_PASS_MAX];
    char     device_id[CREDS_DEVICE_ID_MAX];
    uint32_t crc32;
} creds_record_t;

_Static_assert(sizeof(creds_record_t) <= FLASH_PAGE_SIZE,
               "creds_record_t must fit in a single flash page");

//=============================================================================
// CRC32 (IEEE 802.3, reflected). Small table-less implementation.
//=============================================================================
static uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int j = 0; j < 8; j++) {
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
        }
    }
    return ~c;
}

static uint32_t record_crc(const creds_record_t *r) {
    // CRC over everything up to (but excluding) the trailing crc32 field.
    return crc32((const uint8_t *)r,
                 offsetof(creds_record_t, crc32));
}

//=============================================================================
// Load (direct XIP read — no flash hardware op needed)
//=============================================================================
int creds_load(wifi_creds_t *out) {
    if (!out) return -1;
    creds_record_t r;
    memcpy(&r, CREDS_FLASH_XIP_ADDR, sizeof(r));

    if (r.magic != CREDS_MAGIC) {
        printf("[creds] no magic in flash (raw=0x%08lx)\n",
               (unsigned long)r.magic);
        return -1;
    }
    if (r.version != CREDS_VERSION) {
        printf("[creds] version mismatch (have=%lu want=%u)\n",
               (unsigned long)r.version, CREDS_VERSION);
        return -1;
    }
    if (r.ssid_len      > CREDS_SSID_MAX ||
        r.pass_len      > CREDS_PASS_MAX ||
        r.device_id_len > CREDS_DEVICE_ID_MAX) {
        printf("[creds] length out of range (ssid=%u pass=%u dev=%u)\n",
               r.ssid_len, r.pass_len, r.device_id_len);
        return -1;
    }
    uint32_t want_crc = record_crc(&r);
    if (want_crc != r.crc32) {
        printf("[creds] crc mismatch (have=0x%08lx want=0x%08lx)\n",
               (unsigned long)r.crc32, (unsigned long)want_crc);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->ssid,      r.ssid,      r.ssid_len);
    memcpy(out->password,  r.password,  r.pass_len);
    memcpy(out->device_id, r.device_id, r.device_id_len);
    out->ssid[r.ssid_len]           = '\0';
    out->password[r.pass_len]       = '\0';
    out->device_id[r.device_id_len] = '\0';
    out->opus_enabled               = (r.opus_enabled != 0);
    printf("[creds] loaded SSID='%s' (pass_len=%u) device_id='%s' opus=%d\n",
           out->ssid, r.pass_len, out->device_id, (int)out->opus_enabled);
    return 0;
}

//=============================================================================
// Save — flash_range_erase + flash_range_program inside flash_safe_execute
//=============================================================================
typedef struct {
    creds_record_t record;
    uint8_t        page_buf[FLASH_PAGE_SIZE];
    int            result;
} save_ctx_t;

static void save_flash_cb(void *param) {
    save_ctx_t *ctx = (save_ctx_t *)param;
    flash_range_erase(CREDS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CREDS_FLASH_OFFSET, ctx->page_buf, FLASH_PAGE_SIZE);
    ctx->result = 0;
}

int creds_save(const wifi_creds_t *in) {
    if (!in) return -1;

    size_t slen = strnlen(in->ssid,      CREDS_SSID_MAX);
    size_t plen = strnlen(in->password,  CREDS_PASS_MAX);
    size_t dlen = strnlen(in->device_id, CREDS_DEVICE_ID_MAX);
    if (slen == 0) {
        printf("[creds] refusing to save empty SSID\n");
        return -1;
    }

    save_ctx_t ctx = {0};
    ctx.record.magic         = CREDS_MAGIC;
    ctx.record.version       = CREDS_VERSION;
    ctx.record.ssid_len      = (uint8_t)slen;
    ctx.record.pass_len      = (uint8_t)plen;
    ctx.record.device_id_len = (uint8_t)dlen;
    ctx.record.opus_enabled  = in->opus_enabled ? 1u : 0u;
    memcpy(ctx.record.ssid,      in->ssid,      slen);
    memcpy(ctx.record.password,  in->password,  plen);
    memcpy(ctx.record.device_id, in->device_id, dlen);
    ctx.record.crc32         = record_crc(&ctx.record);
    ctx.result               = -1;

    // Page-aligned + page-sized buffer for flash_range_program.
    memset(ctx.page_buf, 0xFF, sizeof(ctx.page_buf));
    memcpy(ctx.page_buf, &ctx.record, sizeof(ctx.record));

    int rc = flash_safe_execute(save_flash_cb, &ctx, 3000);
    if (rc != PICO_OK) {
        printf("[creds] flash_safe_execute failed rc=%d\n", rc);
        return -1;
    }
    printf("[creds] saved SSID='%s' (pass_len=%u) device_id='%s' opus=%d\n",
           in->ssid, (unsigned)plen, in->device_id, (int)in->opus_enabled);
    return ctx.result;
}

//=============================================================================
// Erase
//=============================================================================
static void erase_flash_cb(void *param) {
    (void)param;
    flash_range_erase(CREDS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
}

int creds_erase(void) {
    int rc = flash_safe_execute(erase_flash_cb, NULL, 3000);
    if (rc != PICO_OK) {
        printf("[creds] erase failed rc=%d\n", rc);
        return -1;
    }
    return 0;
}
