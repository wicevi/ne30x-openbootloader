/* Tests of the DFU class descriptors (usbd_dfu.c): what the host reads at enumeration.
   The real usbd_dfu.c and the real usbd_ctlreq.c (USBD_GetString, USBD_GetNextDesc) are
   linked in unmodified; the descriptors are reached through the public USBD_DFU class
   table, i.e. the callbacks the USB core calls. Only the USB core services they use are
   doubled here. usbd_conf.h is the one of the target, so the descriptors are built with
   the production configuration.

   Checked, because STM32CubeProgrammer relies on it:
     - the configuration descriptor: one interface descriptor per alternate setting, the
       string index of each being USBD_IDX_INTERFACE_STR + alt + 1, and the DFU functional
       descriptor
     - the interface string of every alternate setting has the layout CubeProgrammer
       parses, "@<name> /0x<id>/1*<size><unit>e", and carries the partition id of the
       alternate setting table (usbd_dfu_alt_conf.h) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
/* main.h: the stand-in of dfu_inc/ (no HAL) */
#include "usbd_ioreq.h"
#include "usbd_ctlreq.h"
#include "usbd_dfu.h"
#include "usbd_dfu_alt_conf.h"

/* shared with test_driver.c */
extern char test_log[16384];
extern int test_failures_count;
extern uint32_t g_alt_part_id[USBD_DFU_MAX_ITF_NUM];   /* built from the table by test_driver.c */

/* ---- doubles of the USB core services used by usbd_dfu.c and usbd_ctlreq.c ---- */
static uint8_t core_heap[4096];
static uint32_t core_heap_used;

void *USBD_static_malloc(uint32_t size)
{
  void *p;
  if ((core_heap_used + size) > sizeof(core_heap)) { return NULL; }
  p = &core_heap[core_heap_used];
  core_heap_used += size;
  return p;
}
void USBD_static_free(void *p) { (void)p; }

uint32_t HAL_Delay(uint32_t Delay) { return Delay; }

/* USBD_GetString and USBD_CtlError come from the real usbd_ctlreq.c, linked in. The
   control transfer services (usbd_core.c / usbd_ioreq.c, which drive the USB peripheral)
   are doubled with the signatures of their headers: they are never reached on the paths
   exercised here (descriptors and strings). */
USBD_StatusTypeDef USBD_CtlSendData(USBD_HandleTypeDef *pdev, uint8_t *pbuf, uint32_t len)
{
  (void)pdev; (void)pbuf; (void)len;
  return USBD_OK;
}
USBD_StatusTypeDef USBD_CtlPrepareRx(USBD_HandleTypeDef *pdev, uint8_t *pbuf, uint32_t len)
{
  (void)pdev; (void)pbuf; (void)len;
  return USBD_OK;
}
USBD_DescHeaderTypeDef *USBD_GetNextDesc(uint8_t *pbuf, uint16_t *ptr)
{
  USBD_DescHeaderTypeDef *desc = (USBD_DescHeaderTypeDef *)(void *)pbuf;
  *ptr = (uint16_t)(*ptr + desc->bLength);
  return desc;
}
USBD_StatusTypeDef USBD_Stop(USBD_HandleTypeDef *pdev) { (void)pdev; return USBD_OK; }
USBD_StatusTypeDef USBD_Start(USBD_HandleTypeDef *pdev) { (void)pdev; return USBD_OK; }

/* services of the USB core that usbd_ctlreq.c calls (not used on this path) */
USBD_StatusTypeDef USBD_CtlSendStatus(USBD_HandleTypeDef *pdev) { (void)pdev; return USBD_OK; }
USBD_StatusTypeDef USBD_SetClassConfig(USBD_HandleTypeDef *pdev, uint8_t cfgidx) { (void)pdev; (void)cfgidx; return USBD_OK; }
USBD_StatusTypeDef USBD_ClrClassConfig(USBD_HandleTypeDef *pdev, uint8_t cfgidx) { (void)pdev; (void)cfgidx; return USBD_OK; }
uint8_t USBD_CoreFindIF(USBD_HandleTypeDef *pdev, uint8_t index) { (void)pdev; (void)index; return 0xFFU; }
uint8_t USBD_CoreFindEP(USBD_HandleTypeDef *pdev, uint8_t index) { (void)pdev; (void)index; return 0xFFU; }
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr) { (void)pdev; (void)dev_addr; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) { (void)pdev; (void)ep_addr; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) { (void)pdev; (void)ep_addr; return USBD_OK; }
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr) { (void)pdev; (void)ep_addr; return 0U; }

/* ---- log and checks, appended to the log of test_driver.c ---- */
static void logadd(const char *s)
{
  size_t used = strlen(test_log);
  if (used < (sizeof(test_log) - 1U))
  {
    strncat(test_log, s, sizeof(test_log) - 1U - used);
  }
}

static void CHECK(int cond, const char *fmt, ...)
{
  if (cond == 0)
  {
    va_list ap;
    char b[256];
    test_failures_count++;
    logadd("  FAIL: ");
    va_start(ap, fmt);
    (void)vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    logadd(b);
    logadd("\n");
  }
}

/* Does "s" have the layout STM32CubeProgrammer parses in an interface string,
   "@<name> /0x<id>/1*<digits><unit>e", with <id> the partition id of alternate setting alt? */
static int is_alt_setting_string(const char *s, uint32_t alt)
{
  const char *p = s;
  uint32_t id = 0U;
  int digits = 0;

  if (*p++ != '@') { return 0; }
  /* name: at least one character, no space, no slash */
  if ((*p == '\0') || (*p == ' ') || (*p == '/')) { return 0; }
  while ((*p != '\0') && (*p != ' ') && (*p != '/')) { p++; }
  while (*p == ' ') { p++; }
  /* "/0x<hex digits>/" */
  if ((p[0] != '/') || (p[1] != '0') || (p[2] != 'x')) { return 0; }
  p += 3;
  while (1)
  {
    int v;
    if ((*p >= '0') && (*p <= '9')) { v = *p - '0'; }
    else if (((*p >= 'a') && (*p <= 'f')) || ((*p >= 'A') && (*p <= 'F'))) { v = (*p | 0x20) - 'a' + 10; }
    else { break; }
    id = (id << 4) | (uint32_t)v;
    p++;
    digits++;
  }
  if ((digits == 0) || (digits > 8) || (*p++ != '/')) { return 0; }
  /* "1*<digits><unit>e" */
  if ((p[0] != '1') || (p[1] != '*')) { return 0; }
  p += 2;
  if (!((*p >= '1') && (*p <= '9'))) { return 0; }
  while ((*p >= '0') && (*p <= '9')) { p++; }
  if (strchr(" BKMG", *p) == NULL) { return 0; }
  if (*p != ' ') { p++; }

  return ((*p == 'e') && (p[1] == '\0') && (id == g_alt_part_id[alt])) ? 1 : 0;
}

/* The interface string of an alternate setting, as ASCII (or NULL) */
static const char *alt_string(uint32_t alt, uint16_t *len, char *ascii, size_t size)
{
  uint8_t *s = USBD_DFU.GetUsrStrDescriptor(NULL, (uint8_t)(USBD_IDX_INTERFACE_STR + alt + 1U), len);
  uint32_t k;
  if (s == NULL) { return NULL; }
  for (k = 0U; ((2U + (2U * k)) < s[0]) && ((k + 1U) < size); k++)
  {
    ascii[k] = (s[3U + (2U * k)] == 0U) ? (char)s[2U + (2U * k)] : '?';
  }
  ascii[k] = '\0';
  return ascii;
}

int desc_tests(void)
{
  uint16_t len = 0U;
  uint8_t *cfg;
  uint32_t off, alt, n_if = 0U, n_dfu = 0U;
  uint8_t seen[USBD_DFU_MAX_ITF_NUM];
  uint32_t part = 0U;
  char ascii[96];

  memset(seen, 0, sizeof(seen));

  /* ---- D1: the configuration descriptor, as the USB core hands it to the host ---- */
  logadd("D1 configuration descriptor:\n");
  cfg = USBD_DFU.GetFSConfigDescriptor(&len);
  CHECK(cfg != NULL, "no configuration descriptor\n");
  if (cfg == NULL) { return test_failures_count; }

  CHECK(len == USB_DFU_CONFIG_DESC_SIZ, "length %u, expected %u (= 18 + 9 x %u)\n", len,
        USB_DFU_CONFIG_DESC_SIZ, USBD_DFU_MAX_ITF_NUM);
  CHECK((cfg[2] | (cfg[3] << 8)) == USB_DFU_CONFIG_DESC_SIZ, "wTotalLength %u, expected %u\n",
        cfg[2] | (cfg[3] << 8), USB_DFU_CONFIG_DESC_SIZ);
  CHECK((cfg[0] == 9U) && (cfg[1] == USB_DESC_TYPE_CONFIGURATION), "bad configuration descriptor header\n");
  CHECK(cfg[4] == 1U, "bNumInterfaces %u (expected 1)\n", cfg[4]);

  off = cfg[0];
  while (off < len)
  {
    const uint8_t *d = &cfg[off];
    if (d[0] == 0U) { break; }
    if (d[1] == USB_DESC_TYPE_INTERFACE)
    {
      n_if++;
      CHECK(d[0] == 9U, "interface descriptor at %u: bLength %u\n", off, d[0]);
      CHECK((d[2] == 0U) && (d[4] == 0U), "interface descriptor at %u: bInterfaceNumber %u, bNumEndpoints %u\n", off, d[2], d[4]);
      CHECK((d[5] == 0xFEU) && (d[6] == 0x01U) && (d[7] == 0x02U),
            "interface descriptor at %u: class %02X %02X %02X (DFU is FE 01 02)\n", off, d[5], d[6], d[7]);
      CHECK(d[3] < USBD_DFU_MAX_ITF_NUM, "interface descriptor at %u: alternate setting %u, beyond the table\n", off, d[3]);
      if (d[3] < USBD_DFU_MAX_ITF_NUM)
      {
        CHECK(d[8] == (uint8_t)(USBD_IDX_INTERFACE_STR + d[3] + 1U), "alt %u: iString %u, expected %u\n", d[3], d[8],
              USBD_IDX_INTERFACE_STR + d[3] + 1U);
        CHECK(seen[d[3]] == 0U, "alternate setting %u declared twice\n", d[3]);
        seen[d[3]] = 1U;
      }
    }
    else if (d[1] == DFU_DESCRIPTOR_TYPE)
    {
      n_dfu++;
      CHECK(d[0] == 9U, "DFU functional descriptor: bLength %u\n", d[0]);
      CHECK(d[2] == 0x0FU, "DFU functional descriptor: bmAttributes 0x%02X (the ST OpenBootloader declares 0x0F)\n", d[2]);
      CHECK((d[3] == 0xFFU) && (d[4] == 0x00U), "DFU functional descriptor: wDetachTimeOut %u\n", d[3] | (d[4] << 8));
      CHECK((d[5] | (d[6] << 8)) == USBD_DFU_XFER_SIZE, "DFU functional descriptor: wTransferSize %u, expected %u\n",
            d[5] | (d[6] << 8), USBD_DFU_XFER_SIZE);
      CHECK((d[7] == 0x10U) && (d[8] == 0x01U), "DFU functional descriptor: bcdDFUVersion %02X.%02X (DFU 1.10)\n", d[8], d[7]);
    }
    else
    {
      CHECK(1 == 0, "unexpected descriptor type 0x%02X at %u\n", d[1], off);
    }
    off += d[0];
  }
  CHECK(n_if == USBD_DFU_MAX_ITF_NUM, "%u interface descriptors, expected %u\n", n_if, USBD_DFU_MAX_ITF_NUM);
  CHECK(n_dfu == 1U, "%lu DFU functional descriptors (expected 1)\n", (unsigned long)n_dfu);
  for (alt = 0U; alt < USBD_DFU_MAX_ITF_NUM; alt++)
  {
    CHECK(seen[alt] != 0U, "alternate setting %lu has no interface descriptor\n", (unsigned long)alt);
  }
  {
    char b[96];
    snprintf(b, sizeof b, "  wTotalLength %u: %lu interface descriptors + 1 DFU functional descriptor\n", len, (unsigned long)n_if);
    logadd(b);
  }
  /* the descriptor is the same through the other speed callbacks */
  {
    uint16_t l2 = 0U, l3 = 0U;
    uint8_t *c2 = USBD_DFU.GetHSConfigDescriptor(&l2);
    uint8_t *c3 = USBD_DFU.GetOtherSpeedConfigDescriptor(&l3);
    CHECK((c2 != NULL) && (l2 == len) && (memcmp(c2, cfg, len) == 0), "HS configuration descriptor differs\n");
    CHECK((c3 != NULL) && (l3 == len) && (memcmp(c3, cfg, len) == 0), "other speed configuration descriptor differs\n");
  }

  /* ---- D2: the interface string of every alternate setting ---- */
  logadd("D2 interface strings of the alternate settings:\n");
  for (alt = 0U; alt < USBD_DFU_MAX_ITF_NUM; alt++)
  {
    uint16_t slen = 0U;
    uint8_t *s = USBD_DFU.GetUsrStrDescriptor(NULL, (uint8_t)(USBD_IDX_INTERFACE_STR + alt + 1U), &slen);
    uint32_t k;
    if (s == NULL)
    {
      CHECK(1 == 0, "alt %lu: no string descriptor\n", (unsigned long)alt);
      continue;
    }
    CHECK(s[1] == USB_DESC_TYPE_STRING, "alt %lu: descriptor type %u (a string is 3)\n", (unsigned long)alt, s[1]);
    CHECK((uint16_t)s[0] == slen, "alt %lu: bLength %u, length %u\n", (unsigned long)alt, s[0], slen);
    for (k = 0U; ((2U + (2U * k)) < s[0]) && ((k + 1U) < sizeof(ascii)); k++)
    {
      CHECK(s[3U + (2U * k)] == 0U, "alt %lu: character %lu is not US-ASCII\n", (unsigned long)alt, (unsigned long)k);
      ascii[k] = (char)s[2U + (2U * k)];
    }
    ascii[k] = '\0';
    CHECK(strlen(ascii) == ((s[0] - 2U) / 2U), "alt %lu: %lu characters in a %u byte descriptor\n", (unsigned long)alt,
          (unsigned long)strlen(ascii), s[0]);
    CHECK(is_alt_setting_string(ascii, alt) != 0,
          "alt %lu: '%s' is not \"@<name> /0x<id>/1*<size><unit>e\" with id 0x%02lX\n",
          (unsigned long)alt, ascii, (unsigned long)g_alt_part_id[alt]);
    {
      char b[160];
      snprintf(b, sizeof b, "  alt %2lu: \"%.80s\"\n", (unsigned long)alt, ascii);
      logadd(b);
    }
  }

  /* the five alternate settings of the ST original keep their exact strings */
  {
    static const char *const original[5] =
    {
      "@Flashlayout /0x00/1*1Ke", "@FSBL-EXT /0x03/1*32Ke", "@FSBL-APP /0x04/1*128Me",
      "@virtual /0xF1/1*512Be", "@OTP /0xF2/1*752Be"
    };
    for (alt = 0U; alt < 5U; alt++)
    {
      uint16_t slen = 0U;
      const char *a = alt_string(alt, &slen, ascii, sizeof(ascii));
      CHECK((a != NULL) && (strcmp(a, original[alt]) == 0), "alt %lu: '%s', the ST original is '%s'\n",
            (unsigned long)alt, a ? a : "(none)", original[alt]);
    }
    logadd("  alt 0..4: identical to the strings of the ST original\n");
  }

  /* ---- D3: the alternate setting -> partition id mapping used by the USB module ---- */
  logadd("D3 partition ids of the alternate settings:\n");
  CHECK((USBD_DFU_GetAltPartId(USBD_DFU_ALT_FLASHLAYOUT, &part) == 0U) && (part == DFU_PART_ID_FLASHLAYOUT),
        "flashlayout alternate setting: id 0x%02lX\n", (unsigned long)part);
  CHECK((USBD_DFU_GetAltPartId(USBD_DFU_ALT_EXT_LOADER, &part) == 0U) && (part == DFU_PART_ID_EXT_LOADER),
        "loader alternate setting: id 0x%02lX\n", (unsigned long)part);
  CHECK((USBD_DFU_GetAltPartId(USBD_DFU_ALT_VIRTUAL, &part) == 0U) && (part == DFU_PART_ID_VIRTUAL),
        "virtual alternate setting: id 0x%02lX\n", (unsigned long)part);
  CHECK((USBD_DFU_GetAltPartId(USBD_DFU_ALT_OTP, &part) == 0U) && (part == DFU_PART_ID_OTP),
        "OTP alternate setting: id 0x%02lX\n", (unsigned long)part);
  CHECK(USBD_DFU_GetAltPartId(USBD_DFU_MAX_ITF_NUM, &part) != 0U, "an alternate setting beyond the table is not refused\n");
  CHECK(USBD_DFU_GetAltPartId(0U, NULL) != 0U, "a NULL partition id pointer is not refused\n");

  /* a string index beyond the table is refused, with the length cleared */
  len = 99U;
  CHECK(USBD_DFU.GetUsrStrDescriptor(NULL, (uint8_t)(USBD_IDX_INTERFACE_STR + USBD_DFU_MAX_ITF_NUM + 1U), &len) == NULL,
        "a string index beyond the table is not refused\n");
  CHECK(len == 0U, "a refused string index leaves the length at %u\n", len);
  logadd("  ok\n");

  return test_failures_count;
}
