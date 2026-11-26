#include "stubs.h"
#include "openbl_util.h"
#include <errno.h>

/* Test doubles for the parts of the OpenBootloader the USB module depends on: the
   external memory (through the memory layer the module calls), the OTP storage and the
   heap. Only what openbl_usb_cmd.c and openbl_util.c call is provided. */

/* --------------------------------------------------------------------------------------------
   Model of the NOR flash: 128 MB in 64 KB sectors.
   A bit can only go from 1 to 0; back to 1 needs an erase of the whole sector, exactly
   like a real flash. A write that would need a "0 -> 1" on a sector not erased since the
   last power cycle does not change the bit, and is counted (SIM_OutOfRangeWrites()).
   Sparse: only the sectors actually touched get a buffer, from a static pool, so that
   the model never depends on the heap the tests use for their images.
   -------------------------------------------------------------------------------------------- */
#define NOR_SECTORS (EXT_MEMORY_SIZE / EXT_MEMORY_SECTOR_SIZE)
/* Enough for the complete NE301 download (FSBL, OTA info, 3.8 MB App, 3.1 MB model, web
   UI, 1.7 MB WiFi firmware: 148 sectors), so that a layout built by the host tool from the
   real build/ can be played (--layout) */
#define SECTOR_POOL_COUNT 160

static uint8_t  *sector_buf[NOR_SECTORS];
static uint8_t   sector_erased[NOR_SECTORS];
static uint8_t   sector_pool[SECTOR_POOL_COUNT][EXT_MEMORY_SECTOR_SIZE];
static uint32_t  sector_pool_used;

static uint64_t  verify_fail;          /* address of the mismatch the Verify call reports */
static uint32_t  init_result = 1U;     /* what the loader Init returns */
static uint32_t  init_calls, erase_calls, write_calls, lost_writes;
static uint32_t  erase_log[256];         /* the erase calls: start, end, start, end ... */
static uint32_t  erase_log_used;
static uint32_t  otp_writes;

/* Called when the model cannot continue: the test stops instead of failing oddly */
static void (*sim_abort_hook)(const char *what);
void SIM_SetAbortHook(void (*hook)(const char *what)) { sim_abort_hook = hook; }

static uint8_t *sector_memory(uint32_t s)
{
  if (sector_buf[s] == NULL)
  {
    if (sector_pool_used >= SECTOR_POOL_COUNT)
    {
      if (sim_abort_hook != NULL)
      {
        sim_abort_hook("NOR model: the pool of sector buffers is full");
      }
      return sector_pool[0];            /* not reached when the hook stops the run */
    }
    sector_buf[s] = sector_pool[sector_pool_used++];
    memset(sector_buf[s], 0x5A, EXT_MEMORY_SECTOR_SIZE);     /* stale content */
  }
  return sector_buf[s];
}

void SIM_Reset(void)
{
  uint32_t s;

  /* A power cycle from a known flash: every sector holds stale data and none is erased */
  for (s = 0U; s < NOR_SECTORS; s++)
  {
    sector_buf[s] = NULL;
    sector_erased[s] = 0U;
  }
  sector_pool_used = 0U;
  verify_fail = 0U;
  init_result = 1U;
  init_calls = erase_calls = write_calls = lost_writes = 0U;
  erase_log_used = 0U;
  otp_writes = 0U;

  /* RAM does not hold a power cycle either: no external memory loader is left in place */
  (void)memset((void *)FLASH_LOADER_START_ADDRESS, 0, FLASH_LOADER_SIZE);
  (void)memset((void *)RAM_WRITE_ADDRESS, 0, RAM_WRITE_SIZE);
}

void SIM_SetVerifyFail(uint32_t address) { verify_fail = address; }
void SIM_SetInitResult(uint32_t r)       { init_result = r; }
uint32_t SIM_InitCalls(void)             { return init_calls; }
uint32_t SIM_EraseCalls(void)            { return erase_calls; }
uint32_t SIM_WriteCalls(void)            { return write_calls; }
uint32_t SIM_LostWrites(void)            { return lost_writes; }

uint8_t SIM_NOR_Read(uint32_t addr)
{
  uint32_t off, s;

  if ((addr < EXT_MEMORY_START_ADDRESS) || (addr >= EXT_MEMORY_END_ADDRESS))
  {
    return 0xEEU;
  }
  off = addr - EXT_MEMORY_START_ADDRESS;
  s = off / EXT_MEMORY_SECTOR_SIZE;
  if (sector_buf[s] == NULL)
  {
    return 0xFFU;                        /* never touched: erased flash */
  }
  return sector_buf[s][off % EXT_MEMORY_SECTOR_SIZE];
}

void SIM_NOR_Preset(uint32_t addr, const uint8_t *data, uint32_t len)
{
  uint32_t off = addr - EXT_MEMORY_START_ADDRESS;
  uint32_t s = off / EXT_MEMORY_SECTOR_SIZE;

  memcpy(&sector_memory(s)[off % EXT_MEMORY_SECTOR_SIZE], data, len);
  sector_erased[s] = 0U;
}

/* --------------------------------------------------------------------------------------------
   The memory layer, as the external memory interface of the target calls it
   -------------------------------------------------------------------------------------------- */
uint32_t OPENBL_MEM_Init(uint32_t Address)
{
  uint32_t loader_init;

  UNUSED(Address);
  init_calls++;

  /* As OPENBL_ExtMem_Init() of the target does: the function table of the external
     memory loader is read where the loader is expected, FLASH_LOADER_WRITE_ADDRESS, and
     its Init entry must point into the loader area. A loader written anywhere else, or
     not written at all, fails here as it fails on the device. */
  if (init_result == 0U)
  {
    return 0U;
  }
  loader_init = *(const volatile uint32_t *)FLASH_LOADER_WRITE_ADDRESS;

  return ((loader_init > FLASH_LOADER_START_ADDRESS) && (loader_init < FLASH_LOADER_END_ADDRESS)) ? 1U : 0U;
}

uint32_t SIM_EraseLog(uint32_t *log, uint32_t count)
{
  uint32_t k;
  uint32_t n = (erase_log_used < count) ? erase_log_used : count;

  for (k = 0U; k < n; k++)
  {
    log[k] = erase_log[k];
  }
  return n;
}

void OPENBL_MEM_SectorErase(uint32_t Address, uint32_t EraseStartAddress, uint32_t EraseEndAddress)
{
  uint32_t s = (EraseStartAddress - EXT_MEMORY_START_ADDRESS) / EXT_MEMORY_SECTOR_SIZE;
  uint32_t e = (EraseEndAddress - EXT_MEMORY_START_ADDRESS) / EXT_MEMORY_SECTOR_SIZE;
  UNUSED(Address);

  if ((erase_log_used + 2U) <= (sizeof(erase_log) / sizeof(erase_log[0])))
  {
    erase_log[erase_log_used++] = EraseStartAddress;
    erase_log[erase_log_used++] = EraseEndAddress;
  }

  /* Like the ST external loader: every 64 KB block the range touches is erased, the one
     holding the end address included */
  for (; s <= e; s++)
  {
    (void)memset(sector_memory(s), 0xFF, EXT_MEMORY_SECTOR_SIZE);
    sector_erased[s] = 1U;
    erase_calls++;
  }
}

void OPENBL_MEM_Write(uint32_t Address, uint8_t *Data, uint32_t DataLength)
{
  uint32_t k;

  if ((Address >= EXT_MEMORY_START_ADDRESS) && (Address < EXT_MEMORY_END_ADDRESS))
  {
    uint32_t off = Address - EXT_MEMORY_START_ADDRESS;

    for (k = 0U; k < DataLength; k++)
    {
      uint32_t s = (off + k) / EXT_MEMORY_SECTOR_SIZE;
      uint8_t *cell = &sector_memory(s)[(off + k) % EXT_MEMORY_SECTOR_SIZE];

      /* programming a bit 1 -> 0 is always possible, 0 -> 1 needs an erase */
      if ((sector_erased[s] == 0U) && ((*cell | Data[k]) != *cell))
      {
        lost_writes++;                   /* the write silently does NOT change the bit */
        continue;
      }
      *cell &= Data[k];
    }
  }
  else if ((Address >= FLASH_LOADER_START_ADDRESS) && (DataLength <= FLASH_LOADER_SIZE)
           && (Address <= (FLASH_LOADER_START_ADDRESS + FLASH_LOADER_SIZE - DataLength)))
  {
    memcpy((uint8_t *)Address, Data, DataLength);      /* the external memory loader area */
  }
  else if ((Address >= RAM_WRITE_ADDRESS) && (DataLength <= RAM_WRITE_SIZE)
           && (Address <= (RAM_WRITE_ADDRESS + RAM_WRITE_SIZE - DataLength)))
  {
    memcpy((uint8_t *)Address, Data, DataLength);      /* the RAM download area */
  }
  else
  {
    lost_writes++;                       /* outside every area the target registers */
  }
  write_calls++;
}

/* What the ST external loader returns: the low word is the address where the comparison
   stopped. On a mismatch the test may force the reported address (SIM_SetVerifyFail);
   otherwise the flash content is really compared with what was just written. */
uint64_t OPENBL_MEM_Verify(uint32_t Address, uint32_t DataAddr, uint32_t DataLength, uint32_t CrcInit)
{
  UNUSED(CrcInit);
  if (verify_fail != 0U)
  {
    return verify_fail;
  }
  {
    const uint8_t *data = (const uint8_t *)DataAddr;
    uint32_t k;

    for (k = 0U; k < DataLength; k++)
    {
      if (SIM_NOR_Read(Address + k) != data[k])
      {
        return (uint64_t)(Address + k);
      }
    }
  }
  return (uint64_t)(Address + DataLength);   /* end of the compared range: no mismatch */
}

/* --------------------------------------------------------------------------------------------
   OTP storage
   -------------------------------------------------------------------------------------------- */
static Otp_Partition_t otp_storage;
Otp_Partition_t OPENBL_OTP_Read(void) { return otp_storage; }
int OPENBL_OTP_Write(Otp_Partition_t Otp) { otp_storage = Otp; otp_writes++; return 0; }
uint32_t SIM_OtpWrites(void) { return otp_writes; }

/* --------------------------------------------------------------------------------------------
   The flash layout structure of the parser (.bss of the target: zero after a power cycle)
   -------------------------------------------------------------------------------------------- */
void OPENBL_TestLayoutReset(void)
{
#ifdef OPENBL_REFERENCE
  /* the untouched ST sources have no function to free the strings: one session only */
  (void)memset(&FlashlayoutStruct, 0, sizeof(FlashlayoutStruct));
#else
  OPENBL_FlashLayout_Free();
  (void)memset(&FlashlayoutStruct, 0, sizeof(FlashlayoutStruct));
#endif
}

#ifdef OPENBL_REFERENCE
/* The untouched ST sources have no test hook, and the reference run does one session */
void OPENBL_USB_TestReset(void) { }
#endif

/* --------------------------------------------------------------------------------------------
   Heap of the C library (newlib _sbrk_r calls it)
   -------------------------------------------------------------------------------------------- */
#define TEST_HEAP_BASE   0x10000000U
#define TEST_HEAP_SIZE   0x00800000U
void *_sbrk(ptrdiff_t incr)
{
  static uint8_t *heap_end = NULL;
  uint8_t *prev;

  if (heap_end == NULL)
  {
    heap_end = (uint8_t *)TEST_HEAP_BASE;
  }
  if ((heap_end + incr) > (uint8_t *)(TEST_HEAP_BASE + TEST_HEAP_SIZE))
  {
    errno = ENOMEM;
    return (void *)-1;
  }
  prev = heap_end;
  heap_end += incr;
  return prev;
}
