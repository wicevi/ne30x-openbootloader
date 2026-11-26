/* Test double for the parts of the OpenBootloader the USB module depends on.
   Only what openbl_usb_cmd.c / openbl_util.c actually call is provided. */
#ifndef STUBS_H
#define STUBS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define UNUSED(x) ((void)(x))
typedef enum { ERROR = 0, SUCCESS = !ERROR } ErrorStatus;

/* --- configuration values, same as openbootloader_conf.h --- */
#define FLASH_LOADER_START_ADDRESS        0x38000000U
#define FLASH_LOADER_SIZE                 (32U * 1024U)
#define FLASH_LOADER_END_ADDRESS          (FLASH_LOADER_START_ADDRESS + FLASH_LOADER_SIZE - 1U)
#define FLASH_LOADER_WRITE_ADDRESS        FLASH_LOADER_START_ADDRESS
#define FLASHLAYOUT_ADDRESS               0x341FFC00U
#define RAM_WRITE_ADDRESS                 0x341B9000U
#define RAM_WRITE_SIZE                    0x47000U
#define EXT_MEMORY_START_ADDRESS          0x70000000U
#define EXT_MEMORY_END_ADDRESS            0x78000000U
#define EXT_MEMORY_SIZE                   0x08000000U
#define EXT_MEMORY_SECTOR_SIZE            0x10000U
#define UNDEF_ADDRESS                     0xFFFFFFFFU
#define OTP_PART_SIZE                     (2U * 368U)
#define OTP_ERROR                         1U

typedef struct { uint32_t Version; uint32_t GlobalState; uint32_t OtpPart[OTP_PART_SIZE]; } Otp_Partition_t;

/* --- test API --- */
void  SIM_Reset(void);
void  SIM_SetAbortHook(void (*hook)(const char *what));
void  SIM_SetVerifyFail(uint32_t address);   /* 0: compare the flash for real */
void  SIM_SetInitResult(uint32_t r);
uint32_t SIM_InitCalls(void);
uint32_t SIM_EraseCalls(void);
uint32_t SIM_WriteCalls(void);
uint32_t SIM_LostWrites(void);               /* writes that did not change the flash */
uint32_t SIM_OtpWrites(void);                /* OTP programming calls */
uint8_t SIM_NOR_Read(uint32_t addr);
void  SIM_NOR_Preset(uint32_t addr, const uint8_t *data, uint32_t len);
/* The erase calls the module made, in order: [i] = start address of call i, end address
   of call i is at i + 1. A real loader erases every 64 KB sector from the one holding
   the start address to the one holding the end address, inclusive. */
uint32_t SIM_EraseLog(uint32_t *log, uint32_t count);
void  OPENBL_TestLayoutReset(void);            /* .bss of the parser back to its power-on value */

/* --- remaining declarations of the real headers, needed to compile the untouched parts --- */
#define OPENBL_DEFAULT_MEM               0xFFFFFFFFU
#define FunctionalState                  uint32_t
#define ENABLE                           1U
#define DISABLE                          0U
typedef struct { int dummy; } USBD_HandleTypeDef;
uint32_t OPENBL_USB_GetPage(uint32_t Address);
ErrorStatus OPENBL_MEM_Erase(uint32_t Address, const uint8_t *p_Data, uint32_t DataLength);
ErrorStatus OPENBL_MEM_SetWriteProtection(FunctionalState State, uint32_t Address, const uint8_t *Buffer, uint32_t Length);
void OPENBL_MEM_SetReadOutProtection(uint32_t Address, FunctionalState State);
void Common_StartPostProcessing(void);
uint16_t OPENBL_USB_SendAddressNack(USBD_HandleTypeDef *pDev);
uint8_t USBD_DFU_GetAltPartId(uint8_t alt, uint32_t *part_id);

/* --- memory layer doubles --- */
uint32_t OPENBL_MEM_Init(uint32_t Address);
void     OPENBL_MEM_Write(uint32_t Address, uint8_t *Data, uint32_t DataLength);
void     OPENBL_MEM_SectorErase(uint32_t Address, uint32_t EraseStartAddress, uint32_t EraseEndAddress);
uint64_t OPENBL_MEM_Verify(uint32_t Address, uint32_t DataAddr, uint32_t DataLength, uint32_t CrcInit);
Otp_Partition_t OPENBL_OTP_Read(void);
int      OPENBL_OTP_Write(Otp_Partition_t Otp);
#endif
