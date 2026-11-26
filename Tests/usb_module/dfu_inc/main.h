/* Test stand-in for Core/Inc/main.h when the DFU class is compiled for the host-side test.
   The real one pulls in the whole STM32 HAL. The USB device library only needs these few
   things from it: the standard integer types and the CMSIS attributes below. */
#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

/* CMSIS core attributes (cmsis_compiler.h) */
#ifndef __PACKED
#define __PACKED          __attribute__((packed))
#endif
#ifndef __IO
#define __IO              volatile
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE   static inline
#endif
#ifndef UNUSED
#define UNUSED(X)         (void)(X)
#endif

/* Name of the USB peripheral handle, used by the target usbd_conf.h */
typedef struct { int dummy; } PCD_HandleTypeDef;

/* HAL_Delay(): usbd_conf.h maps USBD_Delay on it (detach request without "will detach") */
uint32_t HAL_Delay(uint32_t Delay);

#endif
