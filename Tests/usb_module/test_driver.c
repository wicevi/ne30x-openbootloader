/* Drives the REAL OpenBootloader USB module (openbl_usb_cmd.c + openbl_util.c) the way
   STM32CubeProgrammer does, against a simulated NOR, and checks the result. */
#include <stdio.h>
#include "stubs.h"
#include "openbl_usb_cmd.h"
#include "openbl_util.h"
/* The alternate setting table of the device (count, indices of the reserved settings) */
#include "usbd_dfu_alt_conf.h"

void OPENBL_USB_TestReset(void);
int desc_tests(void);       /* test_dfu_desc.c */

#define ALT_FLASHLAYOUT USBD_DFU_ALT_FLASHLAYOUT
/* the real one, from usbd_dfu.c (0: ok, 1: no such alternate setting) */
uint8_t USBD_DFU_GetAltPartId(uint8_t alt, uint32_t *part_id);
#define ALT_VIRTUAL     USBD_DFU_ALT_VIRTUAL

/* Partition id of every alternate setting, asked to the DEVICE (USBD_DFU_GetAltPartId, the
   real one: usbd_dfu.c is linked in). The test therefore never keeps a table of its own
   that could disagree with the device. */
uint32_t g_alt_part_id[USBD_DFU_MAX_ITF_NUM];

static void build_alt_table(void)
{
  static int built = 0;
  if (built == 0)
  {
    uint32_t alt;
    for (alt = 0U; alt < USBD_DFU_MAX_ITF_NUM; alt++)
    {
      if (USBD_DFU_GetAltPartId((uint8_t)alt, &g_alt_part_id[alt]) != 0U)
      {
        g_alt_part_id[alt] = 0xFFFFFFFFU;
      }
    }
    built = 1;
  }
}

/* Alternate setting whose partition id is "id" (what STM32CubeProgrammer looks up in the
   interface strings); USBD_DFU_MAX_ITF_NUM if there is none */
static uint32_t alt_of_id(uint32_t id)
{
  uint32_t alt;
  build_alt_table();
  for (alt = 0U; alt < USBD_DFU_MAX_ITF_NUM; alt++)
  {
    if (g_alt_part_id[alt] == id)
    {
      return alt;
    }
  }
  return USBD_DFU_MAX_ITF_NUM;
}

/* ---- log + result ---- */
char test_log[16384];                 /* shared with test_dfu_desc.c */
int test_failures_count;              /* shared with test_dfu_desc.c */
#define log        test_log            /* the name the tests write to */
#define failures   test_failures_count
#define LOG_SIZE   (sizeof(test_log))   /* an array here: sizeof is its size, not a pointer's */
static void logcat(const char *s)
{
  size_t used = strlen(log);
  if (used < (LOG_SIZE - 1U))
  {
    strncat(log, s, LOG_SIZE - 1U - used);
  }
}
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
  { char _n[48]; snprintf(_n, sizeof _n, "  FAIL (line %d): ", __LINE__); logcat(_n); } \
  { char _b[256]; snprintf(_b, sizeof _b, __VA_ARGS__); logcat(_b); } logcat("\n"); } } while (0)

static uint32_t g_last_ret;    /* Result of the last download packet */

/* A flash layout line as the test declares it; the 6 column text is generated from it */
typedef struct
{
  uint32_t id;             /* "Id" column */
  const char *ip;          /* "IP" column: "none" (RAM) or "nor0" (external memory) */
  uint32_t offset;         /* "Offset" column */
  const uint8_t *data;     /* Binary of the line */
  uint32_t len;            /* Length of the binary (0: no download for this line) */
} line_t;

/* A line without binary: the boot loader image, already running (never programmed) */
#define LINE_NONE(id)        (line_t){ (id), "none", 0x0U, NULL, 0U }

/* One UPLOAD on the virtual alternate setting: returns the phase id, gives the address */
static uint32_t host_get_phase(uint32_t *addr)
{
  uint8_t buf[9] = {0};
  (void)OPENBL_USB_ReadMemory(ALT_VIRTUAL, buf, sizeof(buf), 0U);
  *addr = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
  return buf[0];
}

/* DNLOAD of a whole file to an alternate setting: wTransferSize blocks, block numbers from 0
   (what the DFU class does in USBD_DFU_EP0_TxReady -> Write) */
static uint32_t host_download(uint32_t alt, const uint8_t *data, uint32_t len)
{
  uint32_t block, off = 0U, ret = 0U;
  for (block = 0U; off < len; block++)
  {
    uint32_t n = (len - off > 1024U) ? 1024U : (len - off);
    g_last_ret = OPENBL_USB_Download((uint8_t *)&data[off], alt, n, block);
    if (g_last_ret != 0U)
    {
      ret = g_last_ret;
    }
    off += n;
  }
  return ret;
}

/* A power cycle: the simulated NOR, the flash layout of the session before, and the
   static state of the module under test */
/* The flash model cannot continue: record it and end the run (the read of the landmark
   below is what stops the emulator, the same way the end of the tests does) */
#define TEST_DONE_ADDRESS   0xE0000000U
static void sim_abort(const char *what)
{
  logcat("\n  ABORTED by the flash model: ");
  logcat(what);
  logcat("\n");
  failures++;
  (void)*(volatile uint32_t *)TEST_DONE_ADDRESS;
}

static void power_cycle(void)
{
  SIM_Reset();
  SIM_SetAbortHook(sim_abort);
  OPENBL_TestLayoutReset();
  OPENBL_USB_TestReset();
}

static uint8_t tsv_buf[4096];

/* Build the text STM32CubeProgrammer sends to the flashlayout alternate setting: the six
   first columns of every line, TAB separated, LF terminated, behind 256 reserved bytes.
   "header" adds the usual comment line, "final_newline" the line end of the last line. */
static uint32_t build_tsv(const line_t *lines, uint32_t n, int header, int final_newline, const char *extra_ip_column)
{
  uint32_t k, len = 256U;
  (void)memset(tsv_buf, 0, sizeof(tsv_buf));
  if (header != 0)
  {
    len += (uint32_t)snprintf((char *)&tsv_buf[len], sizeof(tsv_buf) - len, "#Opt\tId\tName\tType\tIP\tOffset\n");
  }
  for (k = 0U; k < n; k++)
  {
    len += (uint32_t)snprintf((char *)&tsv_buf[len], sizeof(tsv_buf) - len, "P\t0x%X\tline%lu\tBinary\t%s\t0x%X",
                              (unsigned)lines[k].id, (unsigned long)k, lines[k].ip, (unsigned)lines[k].offset);
    if ((extra_ip_column != NULL) && (k == 0U))
    {
      len += (uint32_t)snprintf((char *)&tsv_buf[len], sizeof(tsv_buf) - len, "\t%s", extra_ip_column);
    }
    if ((final_newline != 0) || ((k + 1U) < n))
    {
      tsv_buf[len++] = '\n';
      tsv_buf[len] = 0U;
    }
  }
  return len;
}

/* What the device must report as the destination of a flash layout line, following the
   rules of the ST original: the external memory loader (id 0x03) goes where
   OPENBL_ExtMem_Init() reads its function table; a "none" line to RAM; a "nor..." line to
   the external memory when the offset stays inside of it; anything else: no address */
static int tsv_header = 1;      /* the usual comment line first; the ST original cannot parse it */

static uint32_t expected_address_of(uint32_t id, const char *ip, uint32_t offset)
{
  if (id == 0x03U)
  {
    return FLASH_LOADER_WRITE_ADDRESS;
  }
  if (ip == NULL)
  {
    return UNDEF_ADDRESS;
  }
  if (strcmp(ip, "none") == 0)
  {
    return RAM_WRITE_ADDRESS;
  }
  if ((strncmp(ip, "nor", 3U) == 0) && (offset < EXT_MEMORY_SIZE))
  {
    return EXT_MEMORY_START_ADDRESS + offset;
  }
  return UNDEF_ADDRESS;
}

static uint32_t expected_address(const line_t *line)
{
  return expected_address_of(line->id, line->ip, line->offset);
}

/* One complete session, as STM32CubeProgrammer runs it with the ST OpenBootloader
   (the sequence the device must reproduce, whichever its source):
     1. ask (virtual alternate setting): the device announces the flash layout (0x00)
        -> the host downloads the TSV text to the flashlayout alternate setting
     2. ask: the device announces the flash layout again ("start")
     3. for every line the device announces: ask -> the device announces the line by its
        partition id and its destination -> the host downloads the binary of the line
        carrying that id to the alternate setting of that id -> ask -> the device answers
        the same id ("start")
     4. ask: the device answers the end of operation (0xFE)
   The host does not know which line comes next: it takes the lines as the device
   announces them, and checks that they are those of the layout, in its order. A line
   whose id has no alternate setting (the boot loader image) can never be announced:
   STM32CubeProgrammer stops on it ("unable to get the associate alternate setting"). */
static void run_session(const line_t *lines, uint32_t n, const char *label)
{
  uint32_t len, phase, start_phase, addr = 0U, k = 0U, guard = 0U;
  g_last_ret = 0U;
  (void)memset(tsv_buf, 0, 256U);

  len = build_tsv(lines, n, tsv_header, 1, NULL);
  phase = host_get_phase(&addr);
  CHECK(phase == 0x00U, "%s: the first phase should be the flash layout (0x00), got 0x%02lX", label, (unsigned long)phase);
  CHECK(addr == FLASHLAYOUT_ADDRESS, "%s: flash layout address 0x%08lX", label, (unsigned long)addr);
  g_last_ret = OPENBL_USB_Download(tsv_buf, ALT_FLASHLAYOUT, len, 0U);
  CHECK(g_last_ret == 0U, "%s: flash layout download returned %lu", label, (unsigned long)g_last_ret);

  /* the "start" of the flash layout */
  phase = host_get_phase(&addr);
  CHECK(phase == 0x00U, "%s: the request after the flash layout download should be its start (0x00), got 0x%02lX",
        label, (unsigned long)phase);

  /* the lines, as the device announces them */
  for (;;)
  {
    phase = host_get_phase(&addr);
    if (phase == 0xFEU)
    {
      break;
    }
    CHECK(++guard < 64U, "%s: the session does not terminate", label);
    if (guard >= 64U)
    {
      break;
    }
    CHECK(alt_of_id(phase) != USBD_DFU_MAX_ITF_NUM, "%s: id 0x%02lX has no alternate setting, the host cannot take it",
          label, (unsigned long)phase);
    /* the next line of the layout the host can program */
    while ((k < n) && (alt_of_id(lines[k].id) == USBD_DFU_MAX_ITF_NUM))
    {
      k++;
    }
    CHECK(k < n, "%s: the device announces id 0x%02lX after the last line of the layout", label, (unsigned long)phase);
    if (k >= n)
    {
      break;
    }
    CHECK(phase == lines[k].id, "%s: line %lu: the device announces id 0x%02lX, the layout says 0x%02lX", label,
          (unsigned long)k, (unsigned long)phase, (unsigned long)lines[k].id);
    CHECK(addr == expected_address(&lines[k]), "%s: line %lu (id 0x%02lX) announced at 0x%08lX, expected 0x%08lX", label,
          (unsigned long)k, (unsigned long)phase, (unsigned long)addr, (unsigned long)expected_address(&lines[k]));
    if (lines[k].len != 0U)
    {
      (void)host_download(alt_of_id(phase), lines[k].data, lines[k].len);
    }
    /* the "start" of the line: the device answers its id again */
    start_phase = host_get_phase(&addr);
    CHECK(start_phase == phase, "%s: start of line %lu (id 0x%02lX) answered 0x%02lX", label, (unsigned long)k,
          (unsigned long)phase, (unsigned long)start_phase);
    k++;
  }
  /* every line the host could program was announced */
  while ((k < n) && (alt_of_id(lines[k].id) == USBD_DFU_MAX_ITF_NUM))
  {
    k++;
  }
  CHECK(k == n, "%s: the device announced the lines up to %lu, the layout has %lu", label, (unsigned long)k,
        (unsigned long)n);
}

static uint32_t nor_cmp(uint32_t addr, const uint8_t *data, uint32_t len)
{
  uint32_t k;
  for (k = 0U; k < len; k++)
  {
    if (SIM_NOR_Read(addr + k) != data[k])
    {
      return k;
    }
  }
  return len;
}

/* Images whose every byte depends on its address: a misplacement is detectable */
static uint8_t *mk(uint32_t addr, uint32_t len, uint8_t seed)
{
  uint8_t *d = (uint8_t *)malloc(len);
  uint32_t k;
  for (k = 0U; k < len; k++)
  {
    d[k] = (uint8_t)((((addr + k) * 2654435761U) >> 13) ^ seed);
  }
  return d;
}

/* The image of the external memory loader, as the ST one is laid out: a function pointer
   table (Init, Write, SectorErase, MassErase, -, Verify, Read) whose entries point into
   the loader area, followed by code. Only the table matters to the OpenBootloader. */
static uint8_t *mk_loader(void)
{
  static const uint32_t table[8] =
  {
    0x38000001U, 0x38000003U, 0x38000005U, 0x38000007U, 0U, 0x3800000BU, 0x3800000DU, 0U
  };
  uint8_t *d = (uint8_t *)malloc(256U);

  (void)memcpy(d, table, sizeof(table));
  (void)memset(&d[sizeof(table)], 0xA5, 256U - sizeof(table));
  return d;
}

#define LOADER_LEN 256U

#define SIZE_OK(addr, d) (nor_cmp((addr), (d), sizeof(d)) == sizeof(d))

/* ---- T11: a flash layout built by a host tool ----------------------------------------------
   run.py --layout <dir> stores a layout directory (FlashLayout.tsv and the binaries it
   refers to) in the emulated memory, behind a small directory:
     base: magic, file count, then per file {offset from base, length}
     file 0 is the TSV, the others the binaries in the order of the TSV lines.
   The TSV text goes to the REAL parser and the binaries to the REAL download path,
   exactly as STM32CubeProgrammer would hand them over. What must come out is derived
   from the TSV itself: every "nor0" line must leave its binary at 0x70000000 + offset. */
#define EXT_LAYOUT_BASE      0x30000000U
#define EXT_LAYOUT_MAGIC     0x4C41594FU

static uint32_t ext_file_count(void)
{
  return *(const volatile uint32_t *)(EXT_LAYOUT_BASE + 4U);
}

/* Length of file "index", and (if data != NULL) where it is stored */
static uint32_t ext_file(uint32_t index, const uint8_t **data)
{
  uint32_t offset = *(const volatile uint32_t *)(EXT_LAYOUT_BASE + 8U + (8U * index));
  uint32_t length = *(const volatile uint32_t *)(EXT_LAYOUT_BASE + 12U + (8U * index));
  if (data != NULL)
  {
    *data = (const uint8_t *)(EXT_LAYOUT_BASE + offset);
  }
  return length;
}

static void play_external_layout(void)
{
  const uint8_t *tsv, *bin;
  uint32_t tsv_len, n_lines, k, phase, addr = 0U, start_phase, guard = 0U, written = 0U;
  /* the layout text behind the 256 reserved bytes the DFU class leaves for a signature */
  static uint8_t tsv_packet[256U + 4096U];
  uint32_t before;
  const char *label = "T11";

  if (*(const volatile uint32_t *)EXT_LAYOUT_BASE != EXT_LAYOUT_MAGIC)
  {
    return;                 /* no external layout given: nothing to do (not a failure) */
  }
  n_lines = ext_file_count() - 1U;                   /* one binary per layout line */
  tsv_len = ext_file(0, &tsv);
  before = test_failures_count;
  {
    char b[96];
    snprintf(b, sizeof b, "T11 layout built by the host tool, %lu binaries:\n", (unsigned long)n_lines);
    logcat(b);
  }

  CHECK(tsv_len <= sizeof(tsv_packet) - 256U, "%s: the layout text is %lu bytes, more than the test carries\n",
        label, (unsigned long)tsv_len);
  if (tsv_len > (sizeof(tsv_packet) - 256U)) { return; }

  power_cycle();

  /* 1. the device announces the flash layout, the host sends the TSV text behind the 256
        reserved bytes, as the DFU class hands a packet over */
  phase = host_get_phase(&addr);
  CHECK((phase == 0x00U) && (addr == FLASHLAYOUT_ADDRESS), "%s: first phase is 0x%02lX @0x%08lX\n",
        label, (unsigned long)phase, (unsigned long)addr);
  (void)memset(tsv_packet, 0, 256U);
  memcpy(&tsv_packet[256], tsv, tsv_len);
  g_last_ret = OPENBL_USB_Download(tsv_packet, ALT_FLASHLAYOUT, 256U + tsv_len, 0U);
  CHECK(g_last_ret == 0U, "%s: the device rejects the layout text (returned %lu)\n", label, (unsigned long)g_last_ret);

  /* what the device made of it */
  {
    char b[160];
    snprintf(b, sizeof b, "  parsed: %lu lines\n", (unsigned long)FlashlayoutStruct.partsize);
    logcat(b);
  }

  /* 2. the "start" of the flash layout: announced once more */
  phase = host_get_phase(&addr);
  CHECK((phase == 0x00U) && (addr == FLASHLAYOUT_ADDRESS), "%s: the request after the flash layout download should be its start (0x00), got 0x%02lX @0x%08lX\n",
        label, (unsigned long)phase, (unsigned long)addr);

  /* 3. walk the lines as the host does: a line announced by the device is taken in the
        order of the layout, and its binary (file k + 1) goes to the alternate setting of
        its id. Line 0 of the real layouts is the boot loader image: it has no alternate
        setting, the host sends nothing for it, and the device must skip it. */
  k = 0U;
  for (;;)
  {
    phase = host_get_phase(&addr);
    if (phase == 0xFEU) { break; }
    CHECK(++guard < 64U, "%s: the session does not terminate\n", label);
    if (guard >= 64U) { break; }
    /* the next line of the layout the host can program */
    while ((k < n_lines) && (k < FlashlayoutStruct.partsize) && (alt_of_id(FlashlayoutStruct.id[k]) == USBD_DFU_MAX_ITF_NUM))
    {
      k++;
    }
    CHECK(k < n_lines, "%s: the device announces id 0x%02lX after the last line of the layout (%lu)\n", label,
          (unsigned long)phase, (unsigned long)n_lines);
    if (k >= n_lines) { break; }

    if (k < FlashlayoutStruct.partsize)
    {
      uint32_t expect = expected_address_of(FlashlayoutStruct.id[k], FlashlayoutStruct.ip[k], FlashlayoutStruct.offset[k]);
      CHECK((uint32_t)phase == FlashlayoutStruct.id[k], "%s: line %lu announced as id 0x%02lX, the layout says 0x%02lX\n",
            label, (unsigned long)k, (unsigned long)phase, (unsigned long)FlashlayoutStruct.id[k]);
      CHECK(addr == expect, "%s: line %lu (id 0x%02lX) announced at 0x%08lX, expected 0x%08lX\n", label,
            (unsigned long)k, (unsigned long)phase, (unsigned long)addr, (unsigned long)expect);
    }

    {
      uint32_t len = ext_file(k + 1U, &bin);
      char b[96];

      snprintf(b, sizeof b, "  line %lu id 0x%02lX -> 0x%08lX, %lu bytes\n", (unsigned long)k, (unsigned long)phase,
               (unsigned long)addr, (unsigned long)len);
      logcat(b);
      /* straight from the directory, in wTransferSize packets (host_download) */
      g_last_ret = 0U;
      if (len != 0U)
      {
        (void)host_download(alt_of_id(phase), bin, len);
        CHECK(g_last_ret == 0U, "%s: line %lu (id 0x%02lX): download failed\n", label, (unsigned long)k,
              (unsigned long)phase);
        written++;
      }
    }
    start_phase = host_get_phase(&addr);
    CHECK(start_phase == phase, "%s: start of phase 0x%02lX answered 0x%02lX\n", label, (unsigned long)phase,
          (unsigned long)start_phase);
    k++;
  }
  while ((k < n_lines) && (k < FlashlayoutStruct.partsize) && (alt_of_id(FlashlayoutStruct.id[k]) == USBD_DFU_MAX_ITF_NUM))
  {
    k++;
  }
  CHECK(k == n_lines, "%s: the device announced the lines up to %lu, the layout has %lu\n", label, (unsigned long)k,
        (unsigned long)n_lines);

  /* 4. every "nor..." line left its binary at 0x70000000 + offset */
  for (k = 0U; k < FlashlayoutStruct.partsize; k++)
  {
    const uint8_t *data;
    uint32_t len = ext_file(k + 1U, &data);
    uint32_t target = EXT_MEMORY_START_ADDRESS + FlashlayoutStruct.offset[k];
    char what[96];
    snprintf(what, sizeof what, "line %lu (id 0x%02lX) at 0x%08lX", (unsigned long)k, (unsigned long)FlashlayoutStruct.id[k],
             (unsigned long)target);
    if (FlashlayoutStruct.ip[k] == NULL) { CHECK(1 == 0, "%s: %s: no ip\n", label, what); continue; }
    if (strncmp(FlashlayoutStruct.ip[k], "nor", 3U) != 0)
    {
      continue;                                        /* RAM line: the loader, not in the NOR */
    }
    CHECK(nor_cmp(target, data, len) == len, "%s: %s is not the binary of the layout (first difference at +%lu)\n",
          label, what, (unsigned long)nor_cmp(target, data, len));
  }
  {
    char b[120];
    snprintf(b, sizeof b, "  %lu lines programmed, every 'nor' line matches its binary; %lu sectors erased, %lu lost writes\n",
             (unsigned long)written, (unsigned long)SIM_EraseCalls(), (unsigned long)SIM_LostWrites());
    logcat(b);
  }
  CHECK(SIM_LostWrites() == 0U, "%s: %lu bytes did not land (written on memory that was not erased)\n",
        label, (unsigned long)SIM_LostWrites());
  if (test_failures_count == before) { logcat("T11 ok\n"); }
}

int run_tests(void)
{
  static const uint8_t keep[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

  int before_t10 = 0;
  log[0] = 0;
  test_failures_count = 0;

  /* ---------- T0: the flash layout of the ST OpenBootloader package ----------------------------------
     What `FlashLayout.tsv` of the ST package holds (Binaries/NOR_Binary): the boot loader
     image line, the external memory loader, one partition at offset 0. No comment line: the
     parser of the ST original cannot take one.
     This is the reference case: the same test is run against the UNTOUCHED ST sources
     (test.sh --reference), so that what the tests expect from the device is known to be
     what the ST OpenBootloader does, and what STM32CubeProgrammer was validated with. */
  {
    static line_t L[3];
    static uint8_t *loader, *image;
    loader = mk_loader();
    image  = mk(0x70000000U, 32768U, 8U);
    L[0] = (line_t){0x01U, "none", 0x0U, NULL,   0U};
    L[1] = (line_t){0x03U, "none", 0x0U, loader, LOADER_LEN};
    L[2] = (line_t){0x04U, "nor0", 0x0U, image,  32768U};
    logcat("T0 the flash layout of the ST package, one partition: ");
    tsv_header = 0;
    power_cycle();
    run_session(L, 3U, "T0");
    CHECK(g_last_ret == 0U, "T0: a download packet failed (%lu)", (unsigned long)g_last_ret);
    CHECK(nor_cmp(0x70000000U, image, 32768U) == 32768U, "T0: the partition is not at 0x70000000");
    /* the loader is where OPENBL_ExtMem_Init() reads its function table */
    CHECK(memcmp((const void *)FLASH_LOADER_WRITE_ADDRESS, loader, LOADER_LEN) == 0, "T0: the loader is not at its address");
    CHECK(SIM_EraseCalls() == 1U, "T0: %lu sectors erased (expected 1)", (unsigned long)SIM_EraseCalls());
    CHECK(SIM_InitCalls() == 1U, "T0: the external memory was initialised %lu times", (unsigned long)SIM_InitCalls());
    {
      char b[96];
      snprintf(b, sizeof b, "erases=%lu writes=%lu\n", (unsigned long)SIM_EraseCalls(), (unsigned long)SIM_WriteCalls());
      logcat(b);
    }
  }

#ifdef OPENBL_REFERENCE
  /* the untouched ST sources program one external memory partition per session: T0 is
     the only case they can pass */
  return failures;
#endif

  /* ---------- T1: the NE301 layout: loader + 6 NOR partitions, header comment ---------- */
  {
    static uint8_t *loader, *fsbl, *ota, *app, *model, *web, *wifi;
    static line_t L[8];
    loader = mk_loader();
    fsbl   = mk(0x70000000U, 50176U, 2U);   /* 49 blocks */
    ota    = mk(0x70090000U, 1024U, 3U);
    app    = mk(0x70100000U, 71680U, 4U);   /* 70 blocks: crosses a sector boundary */
    model  = mk(0x70900000U, 1536U, 5U);
    web    = mk(0x71900000U, 2048U, 6U);
    wifi   = mk(0x71A00000U, 3072U, 7U);
    L[0] = (line_t){0x01U, "none", 0x0U,        NULL,   0U};        /* boot loader image line */
    L[1] = (line_t){0x03U, "none", 0x0U,        loader, LOADER_LEN};     /* external loader -> RAM */
    L[2] = (line_t){0x04U, "nor0", 0x0000000U,  fsbl,   50176U};
    L[3] = (line_t){0x05U, "nor0", 0x0090000U,  ota,    1024U};
    L[4] = (line_t){0x06U, "nor0", 0x0100000U,  app,    71680U};
    L[5] = (line_t){0x07U, "nor0", 0x0900000U,  model,  1536U};
    L[6] = (line_t){0x08U, "nor0", 0x1900000U,  web,    2048U};
    L[7] = (line_t){0x09U, "nor0", 0x1A00000U,  wifi,   3072U};
    logcat("T1 NE301 layout, 6 NOR partitions: ");
    tsv_header = 1;
    power_cycle();
    run_session(L, 8U, "T1");
    CHECK(g_last_ret == 0U, "T1: a download packet failed (%lu)", (unsigned long)g_last_ret);
    CHECK(nor_cmp(0x70000000U, fsbl, 50176U) == 50176U, "T1: FSBL misplaced at +%lu", (unsigned long)nor_cmp(0x70000000U, fsbl, 50176U));
    CHECK(SIZE_OK(0x70090000U, ota), "T1: OTA misplaced");
    CHECK(nor_cmp(0x70100000U, app, 71680U) == 71680U, "T1: APP misplaced at +%lu", (unsigned long)nor_cmp(0x70100000U, app, 71680U));
    CHECK(nor_cmp(0x70900000U, model, 1536U) == 1536U, "T1: model misplaced");
    CHECK(SIZE_OK(0x71900000U, web), "T1: WEB misplaced");
    CHECK(nor_cmp(0x71A00000U, wifi, 3072U) == 3072U, "T1: WIFI misplaced");
    CHECK(SIM_LostWrites() == 0U, "T1: %lu writes did not land (flash not erased)", (unsigned long)SIM_LostWrites());
    CHECK(SIM_InitCalls() == 1U, "T1: external memory initialised %lu times (expected 1)", (unsigned long)SIM_InitCalls());
    CHECK(FlashlayoutStruct.partsize == 8U, "T1: partsize %lu", (unsigned long)FlashlayoutStruct.partsize);
    /* sectors 0 (FSBL), 9 (OTA), 16+17 (APP), 144 (model), 400 (WEB), 416 (WiFi) */
    CHECK(SIM_EraseCalls() == 7U, "T1: %lu sector erases (expected 7)", (unsigned long)SIM_EraseCalls());
    {
      char b[96];
      snprintf(b, sizeof b, "erases=%lu writes=%lu\n", (unsigned long)SIM_EraseCalls(), (unsigned long)SIM_WriteCalls());
      logcat(b);
    }
  }

  /* ---------- T2: a partition that is not in the layout is never touched (NVS) ---------- */
  {
    static line_t L[3];
    static uint8_t *loader, *fsbl, *app;
    loader = mk_loader();
    fsbl   = mk(0x70000000U, 1024U, 2U);
    app    = mk(0x70100000U, 1024U, 4U);
    L[0] = (line_t){0x03U, "none", 0x0U,       loader, LOADER_LEN};
    L[1] = (line_t){0x04U, "nor0", 0x0000000U, fsbl,   1024U};
    L[2] = (line_t){0x06U, "nor0", 0x0100000U, app,    1024U};
    logcat("T2 NVS partition not in the layout stays intact:\n");
    power_cycle();
    SIM_NOR_Preset(0x70080000U, keep, sizeof(keep));
    run_session(L, 3U, "T2");
    CHECK(SIZE_OK(0x70080000U, keep), "T2: the NVS content was modified");
    CHECK(SIM_EraseCalls() == 2U, "T2: %lu sector erases (expected 2)", (unsigned long)SIM_EraseCalls());
  }

  /* ---------- T3: descending addresses, sector 0 written LAST ---------- */
  {
    static line_t L[5];
    static uint8_t *loader, *hi, *mid, *low, *sec0;
    loader = mk_loader();
    hi     = mk(0x71A00000U, 1024U, 2U);
    mid    = mk(0x70900000U, 1024U, 3U);
    low    = mk(0x70100000U, 1024U, 4U);
    sec0   = mk(0x70000000U, 1024U, 5U);
    L[0] = (line_t){0x03U, "none", 0x0U,        loader, LOADER_LEN};
    L[1] = (line_t){0x04U, "nor0", 0x1A00000U,  hi,     1024U};
    L[2] = (line_t){0x05U, "nor0", 0x0900000U,  mid,    1024U};
    L[3] = (line_t){0x06U, "nor0", 0x0100000U,  low,    1024U};
    L[4] = (line_t){0x07U, "nor0", 0x0000000U,  sec0,   1024U};
    logcat("T3 descending addresses, sector 0 last:\n");
    power_cycle();
    run_session(L, 5U, "T3");
    CHECK(SIZE_OK(0x71A00000U, hi), "T3: partition at 0x71A00000 misplaced");
    CHECK(SIZE_OK(0x70900000U, mid), "T3: partition at 0x70900000 misplaced");
    CHECK(SIZE_OK(0x70100000U, low), "T3: partition at 0x70100000 misplaced");
    CHECK(SIZE_OK(0x70000000U, sec0), "T3: sector 0 misplaced");
    CHECK(SIM_LostWrites() == 0U, "T3: %lu writes did not land (flash not erased)", (unsigned long)SIM_LostWrites());
    CHECK(SIM_EraseCalls() == 4U, "T3: %lu sector erases (expected 4)", (unsigned long)SIM_EraseCalls());
  }

  /* ---------- T4: the largest layout: all 12 user partitions (14 lines) ---------- */
  {
    static line_t L[15];
    static uint8_t *img[13];
    static uint32_t n[12];
    uint32_t k;
    img[0] = mk_loader();
    L[0] = LINE_NONE(0x01U);
    L[1] = (line_t){0x03U, "none", 0x0U, img[0], LOADER_LEN};
    for (k = 0U; k < 12U; k++)
    {
      /* descending offsets, the last partition on sector 0; the sizes cross sector ends */
      uint32_t off = (k == 11U) ? 0U : (0x40U + (11U - k) * 0x300U) * 0x1000U;
      n[k] = 1024U * (1U + k);
      img[k + 1U] = mk(EXT_MEMORY_START_ADDRESS + off, n[k], (uint8_t)k);
      L[2U + k] = (line_t){0x04U + k, "nor0", off, img[k + 1U], n[k]};
    }
    logcat("T4 all 12 partitions (boot loader + loader + 12 = 14 lines):\n");
    power_cycle();
    run_session(L, 14U, "T4");
    for (k = 0U; k < 12U; k++)
    {
      CHECK(nor_cmp(EXT_MEMORY_START_ADDRESS + L[2U + k].offset, img[k + 1U], n[k]) == n[k],
            "T4: partition id 0x%02lX misplaced", (unsigned long)(0x04U + k));
    }
    CHECK(FlashlayoutStruct.partsize == 14U, "T4: partsize %lu", (unsigned long)FlashlayoutStruct.partsize);
    CHECK(SIM_LostWrites() == 0U, "T4: %lu writes did not land", (unsigned long)SIM_LostWrites());
    CHECK(SIM_InitCalls() == 1U, "T4: external memory initialised %lu times (expected 1)", (unsigned long)SIM_InitCalls());
  }

  /* ---------- T5: two sessions in a row WITHOUT a reset ---------- */
  {
    static line_t L[3];
    static uint8_t *loader, *a, *b, *c, *d;
    uint32_t n_loop;
    loader = mk_loader();
    a = mk(0x70100000U, 1024U, 2U);
    b = mk(0x70900000U, 1024U, 3U);
    c = mk(0x70500000U, 2048U, 4U);
    d = mk(0x71100000U, 4096U, 5U);
    L[0] = (line_t){0x03U, "none", 0x0U,       loader, LOADER_LEN};
    L[1] = (line_t){0x04U, "nor0", 0x0100000U, a,      1024U};
    L[2] = (line_t){0x05U, "nor0", 0x0900000U, b,      1024U};
    logcat("T5 two sessions without reset:\n");
    power_cycle();
    run_session(L, 3U, "T5 first session");
    CHECK(SIZE_OK(0x70100000U, a) && SIZE_OK(0x70900000U, b), "T5: first session misplaced");
    /* the same module instance, a new flash layout, other lines and addresses */
    L[1] = (line_t){0x04U, "nor0", 0x0500000U, c, 2048U};
    L[2] = (line_t){0x05U, "nor0", 0x1100000U, d, 4096U};
    run_session(L, 3U, "T5 second session");
    CHECK(SIZE_OK(0x70500000U, c) && SIZE_OK(0x71100000U, d), "T5: second session misplaced");
    /* the strings of the previous layout are reused, not leaked: 40 more downloads */
    L[1] = (line_t){0x04U, "nor0", 0x0100000U, a, 1024U};
    L[2] = (line_t){0x05U, "nor0", 0x0900000U, b, 1024U};
    for (n_loop = 0U; n_loop < 40U; n_loop++)
    {
      run_session(L, 3U, "T5 loop");
      if (failures != 0)
      {
        break;
      }
    }
    CHECK(failures == 0, "T5: session %lu without reset failed", (unsigned long)n_loop + 1U);
    CHECK(SIZE_OK(0x70100000U, a) && SIZE_OK(0x70900000U, b), "T5: last of the repeated sessions misplaced");
  }

  /* ---------- T6: more than 15 lines is rejected and leaves no usable layout ---------- */
  {
    static line_t L[16];
    static uint8_t *img, *loader;
    uint32_t k, len, ret;
    img = mk(0x70100000U, 1024U, 2U);
    loader = mk_loader();
    for (k = 0U; k < 16U; k++)
    {
      L[k] = (line_t){0x04U + k, "nor0", k * 0x100000U, img, 1024U};
    }
    logcat("T6 16 lines rejected:\n");
    power_cycle();
    len = build_tsv(L, 16U, 1, 1, NULL);
    ret = (uint32_t)OPENBL_FlashLayout_Parse_Layout((uint32_t)&tsv_buf[256], len - 256U);
    CHECK(ret == (uint32_t)PARSE_ERROR, "T6: a 16 line layout was accepted");
    CHECK(FlashlayoutStruct.partsize == 0U, "T6: partsize %lu after a rejected layout", (unsigned long)FlashlayoutStruct.partsize);
    /* a valid layout still works afterwards (with its loader: RAM is empty after the
       power cycle) */
    L[0] = (line_t){0x03U, "none", 0x0U,       loader, LOADER_LEN};
    L[1] = (line_t){0x04U, "nor0", 0x0100000U, img,    1024U};
    run_session(L, 2U, "T6 after rejection");
    CHECK(SIZE_OK(0x70100000U, img), "T6: layout after the rejected one misplaced");

    /* a layout without any line, and a malformed last line, are errors too (the ST
       original accepts a layout without line, and ignores a malformed last line) */
    (void)memset(tsv_buf, 0, 256U);
    strcpy((char *)&tsv_buf[256], "#Opt\tId\tName\tType\tIP\tOffset\n\n");
    ret = (uint32_t)OPENBL_FlashLayout_Parse_Layout((uint32_t)&tsv_buf[256], (uint32_t)strlen("#Opt\tId\tName\tType\tIP\tOffset\n\n") + 1U);
    CHECK(ret == (uint32_t)PARSE_ERROR, "T6: a layout without line accepted");
    strcpy((char *)&tsv_buf[256], "P\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0");
    ret = (uint32_t)OPENBL_FlashLayout_Parse_Layout((uint32_t)&tsv_buf[256], (uint32_t)strlen("P\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0") + 1U);
    CHECK(ret == (uint32_t)PARSE_ERROR, "T6: a malformed last line (no offset) accepted");
    /* a line without its offset in the middle of a layout does not take the offset of the
       line before (what the ST original does: its column buffers are never cleared) */
    strcpy((char *)&tsv_buf[256], "P\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0\nP\t0x05\tb\tBinary\tnor0\t0x200000\n");
    ret = (uint32_t)OPENBL_FlashLayout_Parse_Layout((uint32_t)&tsv_buf[256],
        (uint32_t)strlen("P\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0\nP\t0x05\tb\tBinary\tnor0\t0x200000\n") + 1U);
    CHECK(ret == (uint32_t)PARSE_ERROR, "T6: a line without offset accepted (with the offset of the line before)");
    CHECK(FlashlayoutStruct.partsize == 0U, "T6: %lu lines left by a rejected layout", (unsigned long)FlashlayoutStruct.partsize);
  }

  /* ---------- T7: comment lines, empty lines, CRLF, no final newline, 7 columns ---------- */
  {
    static const char *layouts[] = {
      "P\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0\t0x100000\n",
      "#Opt\tId\tName\tType\tIP\tOffset\nP\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0\t0x100000\n",
      "#Opt\tId\n# a second comment line\nP\t0x03\text\tBinary\tnone\t0x0\n# in the middle\nP\t0x04\tapp\tBinary\tnor0\t0x100000\n",
      "P\t0x03\text\tBinary\tnone\t0x0\r\nP\t0x04\tapp\tBinary\tnor0\t0x100000\r\n",
      "P\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0\t0x100000",
      "# only a comment\nP\t0x03\text\tBinary\tnone\t0x0\nP\t0x04\tapp\tBinary\tnor0\t0x100000\n",
      "\n\nP\t0x03\text\tBinary\tnone\t0x0\n\nP\t0x04\tapp\tBinary\tnor0\t0x100000\n\n",
      "P\t0x03\text\tBinary\tnone\t0x0\tloader.bin\nP\t0x04\tapp\tBinary\tnor0\t0x100000\tapp.bin\n",
    };
    uint32_t k, ret;
    logcat("T7 comment / line ending variants:\n");
    for (k = 0U; k < (sizeof(layouts) / sizeof(layouts[0])); k++)
    {
      (void)memset(tsv_buf, 0, 256U);
      strcpy((char *)&tsv_buf[256], layouts[k]);
      /* The parser gets the text itself (as OPENBL_USB_Download hands it over: behind the
         256 reserved bytes), not the reserved bytes */
      ret = (uint32_t)OPENBL_FlashLayout_Parse_Layout((uint32_t)&tsv_buf[256], (uint32_t)strlen(layouts[k]) + 1U);
      CHECK(ret == (uint32_t)PARSE_OK, "T7 variant %lu rejected", (unsigned long)k);
      CHECK(FlashlayoutStruct.partsize == 2U, "T7 variant %lu: partsize %lu (expected 2)", (unsigned long)k,
            (unsigned long)FlashlayoutStruct.partsize);
      CHECK(FlashlayoutStruct.id[0] == 0x03U && FlashlayoutStruct.id[1] == 0x04U, "T7 variant %lu: ids %lu,%lu", (unsigned long)k,
            (unsigned long)FlashlayoutStruct.id[0], (unsigned long)FlashlayoutStruct.id[1]);
      CHECK(FlashlayoutStruct.offset[1] == 0x100000U, "T7 variant %lu: offset 0x%lX", (unsigned long)k,
            (unsigned long)FlashlayoutStruct.offset[1]);
      CHECK((FlashlayoutStruct.ip[1] != NULL) && (strcmp(FlashlayoutStruct.ip[1], "nor0") == 0), "T7 variant %lu: ip '%s'",
            (unsigned long)k, FlashlayoutStruct.ip[1] ? FlashlayoutStruct.ip[1] : "(null)");
      CHECK((FlashlayoutStruct.opt[1] != NULL) && (strcmp(FlashlayoutStruct.opt[1], "P") == 0), "T7 variant %lu: opt '%s'",
            (unsigned long)k, FlashlayoutStruct.opt[1] ? FlashlayoutStruct.opt[1] : "(null)");
      CHECK((FlashlayoutStruct.name[1] != NULL) && (strcmp(FlashlayoutStruct.name[1], "app") == 0), "T7 variant %lu: name '%s'",
            (unsigned long)k, FlashlayoutStruct.name[1] ? FlashlayoutStruct.name[1] : "(null)");
      CHECK((FlashlayoutStruct.type[1] != NULL) && (strcmp(FlashlayoutStruct.type[1], "Binary") == 0), "T7 variant %lu: type '%s'",
            (unsigned long)k, FlashlayoutStruct.type[1] ? FlashlayoutStruct.type[1] : "(null)");
    }
  }

  /* ---------- T8: failures are reported (the original always returned 0) ---------- */
  {
    static line_t L[2];
    static uint8_t *loader, *app, *big;
    uint32_t addr_t8 = 0U, phase;
    loader = mk_loader();
    app = mk(0x70100000U, 1024U, 2U);
    big = mk(0x77FFFC00U, 2048U, 3U);   /* starts 1 KB before the end of the memory, 2 KB long */
    L[0] = (line_t){0x03U, "none", 0x0U,       loader, LOADER_LEN};
    L[1] = (line_t){0x04U, "nor0", 0x0100000U, app,    1024U};
    logcat("T8 errors are reported:\n");

    /* the external memory loader does not start */
    power_cycle();
    SIM_SetInitResult(0U);
    run_session(L, 2U, "T8a");
    CHECK(g_last_ret != 0U, "T8a: external memory init failure not reported");
    CHECK(SIM_WriteCalls() == 1U, "T8a: %lu writes happened after the init failure (expected 1: the loader)", (unsigned long)SIM_WriteCalls());

    /* the loader reports a mismatch inside the written range */
    power_cycle();
    SIM_SetVerifyFail(0x70100004U);
    run_session(L, 2U, "T8b");
    CHECK(g_last_ret != 0U, "T8b: verify mismatch not reported");

    /* The flash model compares the flash content itself: a correct download passes. The
       device accepts "0" and "end of the compared range" as "no mismatch", the two values
       loaders give; both are exercised (T8c with the real comparison, which returns the
       end of the range, T8d with a forced 0). */
    power_cycle();
    run_session(L, 2U, "T8c");
    CHECK(g_last_ret == 0U, "T8c: a correct download wrongly reported as an error");
    CHECK(SIZE_OK(0x70100000U, app), "T8c: data misplaced");
    power_cycle();
    SIM_SetVerifyFail(0x70100000U + 1024U);          /* returned as is: the end of the range */
    run_session(L, 2U, "T8d");
    CHECK(g_last_ret == 0U, "T8d: verify == end of range wrongly reported as an error");
    CHECK(SIZE_OK(0x70100000U, app), "T8d: data misplaced");

    /* an unsupported "ip": nothing is written to the memory and the error is reported */
    {
      static line_t B[1];
      B[0] = (line_t){0x04U, "sdmmc", 0x100000U, app, 1024U};
      power_cycle();
      run_session(B, 1U, "T8e");
      CHECK(g_last_ret != 0U, "T8e: unsupported ip not reported");
      CHECK(SIM_WriteCalls() == 0U, "T8e: %lu writes happened", (unsigned long)SIM_WriteCalls());
      CHECK(SIM_EraseCalls() == 0U, "T8e: %lu erases happened", (unsigned long)SIM_EraseCalls());
    }

    /* an image running past the end of the external memory */
    {
      static line_t B[2];
      B[0] = (line_t){0x03U, "none", 0x0U,       loader, LOADER_LEN};
      B[1] = (line_t){0x04U, "nor0", 0x7FFFFC00U, big,  2048U};
      power_cycle();
      run_session(B, 2U, "T8f");
      CHECK(g_last_ret != 0U, "T8f: write past the end of the memory not reported");
      CHECK(SIM_WriteCalls() == 1U, "T8f: %lu writes happened (expected 1: the loader)", (unsigned long)SIM_WriteCalls());
    }

    /* a line with a negative offset, i.e. below the external memory base */
    {
      static line_t B[2];
      B[0] = (line_t){0x03U, "none", 0x0U,        loader, LOADER_LEN};
      B[1] = (line_t){0x04U, "nor0", 0xF0000000U, app,    1024U};
      power_cycle();
      run_session(B, 2U, "T8g");
      CHECK(g_last_ret != 0U, "T8g: offset below the external memory base not reported");
      CHECK(SIM_WriteCalls() == 1U, "T8g: %lu writes happened (expected 1: the loader)", (unsigned long)SIM_WriteCalls());
    }

    /* The external memory loader must be in place where OPENBL_ExtMem_Init() reads its
       function table (0x38000000) when the first external memory packet arrives. Without
       the loader line, or with a loader line that carries no binary, the external memory
       cannot start: the error is reported and nothing reaches the memory. */
    {
      static line_t B[2];
      B[0] = (line_t){0x01U, "none", 0x0U, NULL,   0U};
      B[1] = (line_t){0x04U, "nor0", 0x0100000U, app,    1024U};
      power_cycle();
      run_session(B, 2U, "T8h");
      CHECK(g_last_ret != 0U, "T8h: no loader in the layout, the external memory init failure not reported");
      CHECK((SIM_EraseCalls() == 0U) && (SIM_WriteCalls() == 0U), "T8h: %lu writes, %lu erases happened",
            (unsigned long)SIM_WriteCalls(), (unsigned long)SIM_EraseCalls());

      B[0] = (line_t){0x03U, "none", 0x0U, NULL,   0U};
      power_cycle();
      run_session(B, 2U, "T8i");
      CHECK(g_last_ret != 0U, "T8i: loader line without binary, the external memory init failure not reported");
      CHECK((SIM_EraseCalls() == 0U) && (SIM_WriteCalls() == 0U), "T8i: %lu writes, %lu erases happened",
            (unsigned long)SIM_WriteCalls(), (unsigned long)SIM_EraseCalls());
    }

    /* A flash layout download with nothing behind the 256 reserved bytes is refused. The
       session before (T8i) is over: the device announces the flash layout again, and
       with the download refused it has no layout at all, not the one of before: after the
       "start" of the flash layout comes the end of operation, never a line of the old
       layout. */
    CHECK(OPENBL_USB_Download(loader, ALT_FLASHLAYOUT, 256U, 0U) != 0U, "T8j: a 256 byte flash layout download accepted");
    phase = host_get_phase(&addr_t8);
    CHECK(phase == 0x00U, "T8j: the device announces 0x%02lX, expected the flash layout (0x00)", (unsigned long)phase);
    phase = host_get_phase(&addr_t8);
    CHECK(phase == 0x00U, "T8j: the start of the flash layout answered 0x%02lX", (unsigned long)phase);
    phase = host_get_phase(&addr_t8);
    CHECK(phase == 0xFEU, "T8j: without a flash layout the device announces 0x%02lX, expected the end of operation",
          (unsigned long)phase);
    CHECK(FlashlayoutStruct.partsize == 0U, "T8j: %lu lines of the layout before are still there",
          (unsigned long)FlashlayoutStruct.partsize);

    /* the same after a power cycle: no layout at all in the device */
    power_cycle();
    phase = host_get_phase(&addr_t8);
    CHECK(phase == 0x00U, "T8k: a device without flash layout announces 0x%02lX", (unsigned long)phase);
    phase = host_get_phase(&addr_t8);
    CHECK(phase == 0x00U, "T8k: the start of the flash layout answered 0x%02lX", (unsigned long)phase);
    phase = host_get_phase(&addr_t8);
    CHECK(phase == 0xFEU, "T8k: after the start of a flash layout that never came the device announces 0x%02lX, "
          "expected the end of operation", (unsigned long)phase);

    /* A flash layout line whose id is the one of the OTP (0xF2) or of the flash layout
       (0x00) is announced on their alternate settings: its binary must be refused. The
       ST original takes it for the OTP content (programming the OTP cannot be undone) or
       for a flash layout. */
    {
      static line_t B[2];
      static uint8_t *otp_image;

      otp_image = mk(0x10000000U, 3072U, 9U);   /* 3 packets: what fills the OTP part and programs it */
      B[0] = (line_t){0x03U, "none", 0x0U, loader, LOADER_LEN};
      B[1] = (line_t){0xF2U, "none", 0x0U, otp_image, 3072U};
      power_cycle();
      run_session(B, 2U, "T8l");
      CHECK(g_last_ret != 0U, "T8l: a flash layout line with the id of the OTP accepted");
      CHECK(SIM_OtpWrites() == 0U, "T8l: the OTP was programmed (%lu times)", (unsigned long)SIM_OtpWrites());

      B[1] = (line_t){0x00U, "none", 0x0U, otp_image, 3072U};
      power_cycle();
      run_session(B, 2U, "T8m");
      CHECK(g_last_ret != 0U, "T8m: a flash layout line with the id of the flash layout accepted");
      CHECK(FlashlayoutStruct.partsize == 2U, "T8m: the flash layout was replaced by the line (%lu lines)",
            (unsigned long)FlashlayoutStruct.partsize);

      /* The OTP itself is still programmed when no flash layout line is announced */
      power_cycle();
      {
        uint8_t buf[9] = {0};

        (void)OPENBL_USB_ReadMemory(USBD_DFU_ALT_OTP, buf, sizeof(buf), 0U);   /* the host reads it first */
        g_last_ret = 0U;
        (void)host_download(USBD_DFU_ALT_OTP, otp_image, 3072U);
        CHECK(g_last_ret == 0U, "T8n: the download of the OTP itself refused (%lu)", (unsigned long)g_last_ret);
        CHECK(SIM_OtpWrites() == 1U, "T8n: the OTP was programmed %lu times (expected 1)", (unsigned long)SIM_OtpWrites());
      }
    }
  }

  /* ---------- T9: the alternate settings ---------- */
  {
    static line_t L[2];
    static uint8_t *loader, *app;
    uint32_t phase, addr = 0U, part = 0U;
    loader = mk_loader();
    app = mk(0x70100000U, 1024U, 2U);
    L[0] = (line_t){0x03U, "none", 0x0U,       loader, LOADER_LEN};
    L[1] = (line_t){0x04U, "nor0", 0x0100000U, app,    1024U};
    logcat("T9 alternate settings:\n");
    power_cycle();

    /* an unknown alternate setting has no phase */
    CHECK(USBD_DFU_GetAltPartId(USBD_DFU_MAX_ITF_NUM, &part) != 0U, "T9: an unknown alternate setting has a partition id");
    /* data on an alternate setting that does not exist is refused */
    CHECK(OPENBL_USB_Download(app, USBD_DFU_MAX_ITF_NUM, 8U, 0U) != 0U, "T9: download on an unknown alternate setting accepted");
    /* the reserved alternate settings keep their ids */
    CHECK((USBD_DFU_GetAltPartId(ALT_FLASHLAYOUT, &part) == 0U) && (part == DFU_PART_ID_FLASHLAYOUT), "T9: flashlayout alternate setting id");
    CHECK((USBD_DFU_GetAltPartId(USBD_DFU_ALT_VIRTUAL, &part) == 0U) && (part == DFU_PART_ID_VIRTUAL), "T9: virtual alternate setting id");
    CHECK((USBD_DFU_GetAltPartId(USBD_DFU_ALT_OTP, &part) == 0U) && (part == DFU_PART_ID_OTP), "T9: OTP alternate setting id");
    /* a request on the OTP alternate setting does not disturb the walk */
    {
      uint8_t buf[9] = {0};
      (void)OPENBL_USB_ReadMemory(USBD_DFU_ALT_OTP, buf, sizeof(buf), 0U);
    }
    phase = host_get_phase(&addr);
    CHECK((phase == 0x00U) && (addr == FLASHLAYOUT_ADDRESS), "T9: first virtual answer is 0x%02lX @0x%08lX",
          (unsigned long)phase, (unsigned long)addr);

    /* data sent on the WRONG alternate setting (another partition) is refused and writes nothing */
    power_cycle();
    {
      uint32_t len;
      len = build_tsv(L, 2U, 1, 1, NULL);
      (void)host_get_phase(&addr);                            /* the flash layout, announced */
      (void)OPENBL_USB_Download(tsv_buf, ALT_FLASHLAYOUT, len, 0U);
      (void)host_get_phase(&addr);                            /* its "start" */
      phase = host_get_phase(&addr);                          /* the loader line, phase 0x03 */
      CHECK(phase == 0x03U, "T9: the loader line announced as 0x%02lX (expected 0x03)", (unsigned long)phase);
      CHECK(addr == FLASH_LOADER_WRITE_ADDRESS, "T9: the loader line is announced at 0x%08lX, expected 0x%08lX",
            (unsigned long)addr, (unsigned long)FLASH_LOADER_WRITE_ADDRESS);
      CHECK(OPENBL_USB_Download(app, alt_of_id(0x04U), 1024U, 0U) != 0U, "T9: data on another alternate setting than the announced phase accepted");
      CHECK(SIM_WriteCalls() == 0U, "T9: %lu writes happened on a wrong alternate setting", (unsigned long)SIM_WriteCalls());
      /* the flash layout sent to a partition alternate setting is refused too */
      CHECK(OPENBL_USB_Download(tsv_buf, alt_of_id(0x04U), len, 0U) != 0U, "T9: flash layout on a partition alternate setting accepted");
    }
  }

  /* ---------- T12: what a line erases -----------------------------------------------------------------
     A loader may take the sector holding the END address of an erase request as well. An
     image ending exactly on a 64 KB sector boundary must not erase the first sector of
     what follows it; and a packet straddling a sector boundary must find the new sector
     erased before any of its bytes is written. */
  {
    static line_t L[3];
    static uint8_t *loader, *full, *straddle;
    static const uint8_t sentinel[16] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xF0,0x11};
    uint32_t log[8];
    uint32_t n_log;
    const uint32_t base = 0x70900000U;         /* sectors 0x70900000, 0x70910000, 0x70920000 ... */
    loader   = mk_loader();
    full     = mk(base, 0x10000U, 5U);        /* exactly one sector */
    straddle = mk(base + 0xFFF00U, 4000U, 6U); /* starts 256 bytes before a sector end: packets not aligned on it */
    logcat("T12 erase requests of a line:\n");
    L[0] = (line_t){0x01U, "none", 0x0U, NULL,   0U};            /* the boot loader image line */
    L[1] = (line_t){0x03U, "none", 0x0U, loader, LOADER_LEN};     /* the loader */

    /* a) an image of exactly one sector: the sector after it stays as it was */
    L[2] = (line_t){0x04U, "nor0", base - EXT_MEMORY_START_ADDRESS, full, 0x10000U};
    power_cycle();
    SIM_NOR_Preset(base + 0x10000U, sentinel, sizeof(sentinel));
    run_session(L, 3U, "T12a");
    CHECK(g_last_ret == 0U, "T12a: download failed");
    CHECK(nor_cmp(base, full, 0x10000U) == 0x10000U, "T12a: the image is not at its address");
    CHECK(SIZE_OK(base + 0x10000U, sentinel), "T12a: the sector after an image ending on a sector boundary was erased");
    CHECK(SIM_EraseCalls() == 1U, "T12a: %lu sectors erased (expected 1)", (unsigned long)SIM_EraseCalls());
    n_log = SIM_EraseLog(log, 8U);
    CHECK((n_log == 2U) && (log[0] == base) && (log[1] <= (base + 0x10000U)),
          "T12a: erase request from 0x%08lX to 0x%08lX", (unsigned long)log[0], (unsigned long)log[1]);

    /* b) an image straddling a sector boundary, from a start that is not packet aligned */
    L[2] = (line_t){0x04U, "nor0", (base + 0xFFF00U) - EXT_MEMORY_START_ADDRESS, straddle, 4000U};
    power_cycle();
    SIM_NOR_Preset(base + 0x11000U, sentinel, sizeof(sentinel));
    run_session(L, 3U, "T12b");
    CHECK(g_last_ret == 0U, "T12b: download failed");
    CHECK(nor_cmp(base + 0xFFF00U, straddle, 4000U) == 4000U,
          "T12b: the straddling image is not at its address (first difference at +%lu)",
          (unsigned long)nor_cmp(base + 0xFFF00U, straddle, 4000U));
    CHECK(SIM_EraseCalls() == 2U, "T12b: %lu sectors erased (expected 2)", (unsigned long)SIM_EraseCalls());
    CHECK(SIM_LostWrites() == 0U, "T12b: %lu bytes did not land (written on memory that was not erased)",
          (unsigned long)SIM_LostWrites());
    CHECK(SIZE_OK(base + 0x11000U, sentinel), "T12b: a sector beyond the straddling image was erased");
  }

  /* ---------- T10: the DFU descriptors and interface strings the host enumerates ---------- */
  before_t10 = test_failures_count;
  logcat("T10 DFU descriptors and alternate setting strings:\n");
  build_alt_table();
  (void)desc_tests();
  if (test_failures_count == before_t10)
  {
    logcat("T10 ok\n");
  }

  /* ---------- T11: a flash layout built by a host tool, played as the host would ---------- */
  play_external_layout();

  return failures;
}
