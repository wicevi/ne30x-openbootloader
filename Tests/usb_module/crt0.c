/* Reset handler of the test firmware: initialise .data/.bss, run the tests, signal the end. */
#include <stdint.h>
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern int run_tests(void);

/* Landmark for the emulator: execution reaching this function means the tests are done.
   It reads an address outside the emulated memory, which stops the emulator (cheaper
   than a per-instruction hook, and it is the only unmapped access in the run). */
#define TEST_DONE_ADDRESS   0xE0000000U
__attribute__((noinline, used))
void test_finished(void)
{
  for (;;) {
    (void)*(volatile uint32_t *)TEST_DONE_ADDRESS;
  }
}

void Reset_Handler(void)
{
  uint32_t *s = &_sidata, *d = &_sdata, *e = &_edata;
  while (d < e) { *d++ = *s++; }
  for (d = &_sbss, e = &_ebss; d < e; ) { *d++ = 0U; }
  (void)run_tests();
  test_finished();
  for (;;) { }
}
