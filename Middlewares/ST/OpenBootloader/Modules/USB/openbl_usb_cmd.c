/**
  ******************************************************************************
  * @file    openbl_usb_cmd.c
  * @author  MCD Application Team
  * @brief   Contains USB protocol commands
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2019-2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "openbl_usb_cmd.h"
#include "openbl_mem.h"
#include "openbootloader_conf.h"
#include "usb_interface.h"
#include "common_interface.h"
#include "openbl_util.h"
#include "otp_interface.h"
#include <string.h>

/* External variables --------------------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define USB_RAM_BUFFER_SIZE             20U  /* Size of USB buffer used to store received data from the host */

/* Marker for "no sector erased yet in the current partition": 0 would be wrong as it
   is a valid sector index (the first 64 K block of the external memory) */
#define EXT_MEM_NO_SECTOR_ERASED        0xFFFFFFFFU

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

/* Walk of the flash layout through the virtual alternate setting: the sequence of the
   ST original, that STM32CubeProgrammer was validated with. The host asks for the next
   item twice, once to download it ("phase") and once to start it; the index only
   advances after the "start" request.
     0x00 (phase) -> the host downloads the flash layout
     0x00 (start)
     line 1 (phase) -> the host downloads it on the alternate setting of its partition id
     line 1 (start) ... up to the last line
     0xFE: end of operation
   Line 0 is the boot loader image, which the boot ROM loaded and started: it is never
   announced here (see OPENBL_USB_FirstLine()). */
#define LAYOUT_ITEM   0xFFFFFFFEU   /* the flash layout itself is the item reported last */
#define LAYOUT_DONE   0xFFFFFFFFU   /* end of operation announced: the next request starts over */

static uint32_t OtpIndex;
static uint32_t DestinationAddress;                      /* Reported target of the current item */
static uint8_t PhaseId = PHASE_FLASHLAYOUT;              /* Phase of the item reported last */
static Otp_Partition_t Otp;
static uint32_t NextItem = LAYOUT_ITEM;                  /* LAYOUT_ITEM: the flash layout; n: flash layout line n */
static bool IsStartOperation = false;                    /* Next virtual request is a "start" */

/* Programming of the current flash layout line */
static uint32_t ExtMemInitialized = 0U;                  /* External memory loader started */
static uint32_t ExtMemBaseAddress = 0U;                  /* Base address of the current line */
static uint32_t ExtMemWriteOffset = 0U;                  /* Bytes written to the current line */
static uint32_t ExtMemEraseSector  = EXT_MEM_NO_SECTOR_ERASED; /* Last erased sector, in this line */
static uint32_t RamWriteAddress = 0U;                    /* Next RAM write address (flash loader) */
static uint32_t RamWriteOffset = 0U;                      /* Bytes written to RAM for the current line */

#ifdef OPENBL_USB_TEST_HOOKS
/* Test only (host-side tests running this module in an emulator): put every static of
   this module back to its power-on value, as a device reset would. */
void OPENBL_USB_TestReset(void)
{
  OtpIndex           = 0U;
  DestinationAddress = 0U;
  PhaseId            = PHASE_FLASHLAYOUT;
  (void)memset(&Otp, 0, sizeof(Otp));
  NextItem           = LAYOUT_ITEM;
  IsStartOperation   = false;
  ExtMemInitialized  = 0U;
  ExtMemBaseAddress  = 0U;
  ExtMemWriteOffset  = 0U;
  ExtMemEraseSector  = EXT_MEM_NO_SECTOR_ERASED;
  RamWriteAddress    = 0U;
  RamWriteOffset     = 0U;
}
#endif

/* Private function prototypes -----------------------------------------------*/
static uint32_t OPENBL_USB_GetAddress(uint32_t layout_index);
static uint8_t OPENBL_USB_GetPhase(uint32_t alternate);
static uint32_t OPENBL_USB_IsExtMemAddress(uint32_t Address);
static uint32_t OPENBL_USB_DownloadExtMem(uint8_t *pSrc, uint32_t Length);
static uint32_t OPENBL_USB_FirstLine(void);
static uint8_t OPENBL_USB_LineAnnounced(void);

/**
  * @brief  First flash layout line to announce after the flash layout.
  *         As in the ST original, line 0 is the boot loader image, which the boot ROM
  *         loaded and started, and is never announced. Should line 0 be something else
  *         (a layout handed to an OpenBootloader that already runs, without that line),
  *         the walk starts at line 0.
  *         The flash layout in place has at least one line (the parser refuses an empty
  *         one), so the result is always the index of a line.
  * @retval Index of the first line to announce.
  */
static uint32_t OPENBL_USB_FirstLine(void)
{
  if ((FlashlayoutStruct.partsize != 0U) && (FlashlayoutStruct.id[0] == (uint32_t)PHASE_1))
  {
    return 1U;
  }

  return 0U;
}

/**
  * @brief  Tell whether a flash layout line is the item announced last: not the flash
  *         layout itself, and not after the end of operation.
  * @retval 1 if a line is announced, 0 otherwise.
  */
static uint8_t OPENBL_USB_LineAnnounced(void)
{
  if ((NextItem == LAYOUT_ITEM) || (NextItem == LAYOUT_DONE) || (NextItem >= FlashlayoutStruct.partsize))
  {
    return 0U;
  }

  return 1U;
}



/* Exported functions---------------------------------------------------------*/
/**
  * @brief  Erase sector.
  * @param  Address Address of sector to be erased.
  * @retval 0 if operation is successful, MAL_FAIL else.
  */
uint16_t OPENBL_USB_EraseMemory(uint32_t Address)
{
  ErrorStatus error_value;
  uint8_t status;
  uint32_t numpage;
  uint32_t page;
  uint8_t usb_ram_buffer[USB_RAM_BUFFER_SIZE];
  uint8_t *ramaddress;

  ramaddress = (uint8_t *) usb_ram_buffer;
  numpage = 1;

  *ramaddress = (uint8_t)(numpage & 0x00FFU);
  ramaddress++;

  *ramaddress = (uint8_t)((numpage & 0xFF00U) >> 8);
  ramaddress++;

  page = OPENBL_USB_GetPage(Address);

  *ramaddress = (uint8_t)(page & 0x00FFU);
  ramaddress++;

  *ramaddress = (uint8_t)((page & 0xFF00U) >> 8);
  ramaddress++;

  error_value = OPENBL_MEM_Erase(OPENBL_DEFAULT_MEM, (uint8_t *) usb_ram_buffer, USB_RAM_BUFFER_SIZE);

  if (error_value != SUCCESS)
  {
    status = 1U;
  }
  else
  {
    status = 0U;
  }

  return status;
}

/**
  * @brief  Memory write routine.
  * @param  pSrc Pointer to the source buffer. Address to be written to.
  * @param  Alt USB Alternate.
  * @param  Length Number of data to be written (in bytes).
  * @retval Returns 0 if success else returns 1.
  */
uint32_t OPENBL_USB_Download(uint8_t *pSrc, uint32_t Alt, uint32_t Length, uint32_t BlockNumber)
{
  uint32_t temp_address;
  uint32_t i;
  uint32_t alt_phase;

  /* The data must arrive on the alternate setting of the phase announced last: an
     alternate setting that does not exist, or another one, is an error (and would
     otherwise be written at the address of the previous phase) */
  alt_phase = OPENBL_USB_GetPhase(Alt);
  if ((uint8_t)alt_phase == (uint8_t)PHASE_END)
  {
    return 1U;
  }

  /* The flash layout and the OTP have an alternate setting of their own, which is also
     what a flash layout line with the id 0x00 or 0xF2 is announced on. Such a line must
     be refused: its binary would otherwise be taken for a flash layout, or programmed
     into the OTP, which cannot be undone. The two are only downloaded when nothing else
     is announced. */
  if ((OPENBL_USB_LineAnnounced() != 0U) && ((PhaseId == PHASE_FLASHLAYOUT) || (PhaseId == PHASE_OTP)))
  {
    return 1U;
  }

  switch (PhaseId)
  {
    case PHASE_OTP:
      /* Set OTP version */
      Otp.Version = (((uint32_t)pSrc[3] << 24U)
                     | ((uint32_t)pSrc[2] << 16U)
                     | ((uint32_t)pSrc[1] << 8U)
                     | (uint32_t)pSrc[0]);

      /* Set OTP global state */
      Otp.GlobalState = (((uint32_t)pSrc[7] << 24U)
                         | ((uint32_t)pSrc[6] << 16U)
                         | ((uint32_t)pSrc[5] << 8U)
                         | (uint32_t)pSrc[4]);

      if (BlockNumber == 0U)
      {
        OtpIndex = 0U;

        /* Set OTP values and status */
        for (i = 8U; ((i < Length) && (OtpIndex < OTP_PART_SIZE)); i += 4U)
        {
          Otp.OtpPart[OtpIndex] = (((uint32_t)pSrc[i + 3U] << 24U)
                                   | ((uint32_t)pSrc[i + 2U] << 16U)
                                   | ((uint32_t)pSrc[i + 1U] << 8U)
                                   | (uint32_t)pSrc[i]);

          OtpIndex++;
        }
      }
      else
      {
        OtpIndex = (254U + ((BlockNumber - 1U) * 256U));

        for (i = 0U; ((i < Length) && (OtpIndex < OTP_PART_SIZE)); i += 4U)
        {
          Otp.OtpPart[OtpIndex] = (((uint32_t)pSrc[i + 3U] << 24U)
                                   | ((uint32_t)pSrc[i + 2U] << 16U)
                                   | ((uint32_t)pSrc[i + 1U] << 8U)
                                   | (uint32_t)pSrc[i]);

          OtpIndex++;
        }
      }

      /* Write OTP */
      if (OtpIndex == OTP_PART_SIZE)
      {
        if (OPENBL_OTP_Write(Otp) == OTP_ERROR)
        {
          /* Returns error if OTP write fails */
          return 1U;
        }
      }

      break;

    case PHASE_FLASHLAYOUT:
      if (alt_phase != PHASE_FLASHLAYOUT)
      {
        return 1U;
      }

      /* First 256 is reserved for binary signature info */
      temp_address = (uint32_t)pSrc;

      if (Length <= 256U)
      {
        /* No flash layout behind the reserved bytes */
        return 1U;
      }

      /* Parse the flash layout */
      if (OPENBL_FlashLayout_Parse_Layout(temp_address + 256U, (Length - 256U)) == PARSE_ERROR)
      {
        /* Return error */
        return 1U;
      }

      /* The flash layout has just been downloaded: whatever was going on before is over,
         and the next request on the virtual alternate setting is its "start". A download
         that failed half-way can therefore be restarted. */
      NextItem           = LAYOUT_ITEM;
      IsStartOperation   = true;
      ExtMemWriteOffset  = 0U;
      RamWriteOffset     = 0U;

      break;

    default:
      /* Flash layout line: route by the destination address space reported through
         the virtual alternate setting ("none" -> RAM, "nor"/"mmc" -> external memory),
         whatever the phase id of the line is. The data must come on the alternate
         setting of this very phase. */
      if ((alt_phase != PhaseId) || (DestinationAddress == UNDEF_ADDRESS))
      {
        /* Data on another alternate setting, "ip" of the line not supported,
           or no partition selected */
        return 1U;
      }
      else if (OPENBL_USB_IsExtMemAddress(DestinationAddress) != 0U)
      {
        if (OPENBL_USB_DownloadExtMem(pSrc, Length) != 0U)
        {
          return 1U;
        }
      }
      else
      {
        /* RAM download (the external memory loader): consecutive packets, from the base
           address reported for this line */
        if (RamWriteOffset == 0U)
        {
          RamWriteAddress = DestinationAddress;
        }

        OPENBL_MEM_Write(RamWriteAddress, pSrc, Length);
        RamWriteOffset += Length;
        RamWriteAddress += Length;
      }

      break;
  }

  return 0U;
}

/**
  * @brief  Write one download packet of the current external memory partition.
  *         The partition (flash layout line) is the one reported last through the
  *         virtual alternate setting, see OPENBL_USB_ReadMemory().
  * @param  pSrc Pointer to the source buffer.
  * @param  Length Number of data to be written (in bytes).
  * @retval Returns 0 if success else returns 1.
  */
static uint32_t OPENBL_USB_DownloadExtMem(uint8_t *pSrc, uint32_t Length)
{
  uint32_t write_address;
  uint32_t first_sector;
  uint32_t last_sector;
  uint32_t sector_start;
  uint64_t verify;

  /* A partition must have been selected through the virtual alternate setting */
  if (DestinationAddress == UNDEF_ADDRESS)
  {
    return 1U;
  }

  if (Length == 0U)
  {
    return 0U;
  }

  /* Start the external memory loader with the first packet of the first line in the
     external memory */
  if (ExtMemInitialized == 0U)
  {
    if (OPENBL_MEM_Init(DestinationAddress) != 1U)
    {
      /* Returns error if the external memory initialisation fails */
      return 1U;
    }

    ExtMemInitialized = 1U;
  }

  /* Beginning of a new line: its base address is the one reported for it, and none of
     its sectors is erased yet. Erase tracking is per line, so that a line programmed at
     a lower address than the previous one is erased too. */
  if (ExtMemWriteOffset == 0U)
  {
    ExtMemBaseAddress = DestinationAddress;
    ExtMemEraseSector  = EXT_MEM_NO_SECTOR_ERASED;
  }

  write_address = ExtMemBaseAddress + ExtMemWriteOffset;

  /* The whole packet must stay inside the external memory area */
  if ((write_address < EXT_MEMORY_START_ADDRESS) || (write_address >= EXT_MEMORY_END_ADDRESS)
      || (Length > (EXT_MEMORY_END_ADDRESS - write_address)))
  {
    return 1U;
  }

  /* Erase every sector the packet touches that was not erased yet in this line (the
     first one normally; a packet starting off a sector boundary ends in the next one).
     Each sector is erased for its own, aligned, range: whatever the loader does with the
     end address, only that sector is erased, never the first one of the next partition
     when a line ends exactly on a sector boundary. */
  first_sector = (write_address - EXT_MEMORY_START_ADDRESS) / EXT_MEMORY_SECTOR_SIZE;
  last_sector  = (write_address + Length - 1U - EXT_MEMORY_START_ADDRESS) / EXT_MEMORY_SECTOR_SIZE;
  while ((ExtMemEraseSector == EXT_MEM_NO_SECTOR_ERASED) || (ExtMemEraseSector < last_sector))
  {
    uint32_t sector = (ExtMemEraseSector == EXT_MEM_NO_SECTOR_ERASED) ? first_sector : (ExtMemEraseSector + 1U);

    sector_start = EXT_MEMORY_START_ADDRESS + (sector * EXT_MEMORY_SECTOR_SIZE);
    OPENBL_MEM_SectorErase(sector_start, sector_start, (sector_start + Length));
    ExtMemEraseSector = sector;
  }

  /* Write memory data into memory */
  OPENBL_MEM_Write(write_address, pSrc, Length);
  ExtMemWriteOffset += Length;

  /* Verify the data written in to memory. The low word of the returned value is the
     address where the verification stopped: below the end of the written range means
     a mismatch. Same check as the original USB path and the USART path. */
  verify = OPENBL_MEM_Verify(write_address, (uint32_t)pSrc, Length, 0U);

  if (((uint32_t)verify != 0U) && ((uint32_t)verify < (write_address + Length)))
  {
    /* Error detected when verifying written data */
    return 1U;
  }

  return 0U;
}

/**
  * @brief  Memory read routine.
  * @param  USB Alternate
  * @param  pDest Pointer to the destination buffer.
  * @param  Length Number of data to be read (in bytes).
  * @retval Pointer to the physical address where data should be read.
  */
uint8_t *OPENBL_USB_ReadMemory(uint32_t Alt, uint8_t *pDest, uint32_t Length, uint32_t BlockNumber)
{
  uint32_t i;

  PhaseId = OPENBL_USB_GetPhase(Alt);

  switch (PhaseId)
  {
    case PHASE_CMD:
      if (NextItem == LAYOUT_DONE)
      {
        /* The end of operation was announced for the previous flash layout: this request
           is the first one of a new download */
        NextItem         = LAYOUT_ITEM;
        IsStartOperation = false;
      }

      if (NextItem == LAYOUT_ITEM)
      {
        /* The flash layout itself: the host then downloads it to the flashlayout
           alternate setting, and its "start" is answered the same. The layout of a
           previous session is dropped when the flash layout is announced, not when the
           new one arrives: a download that is refused leaves no old layout to walk. */
        if (IsStartOperation == false)
        {
          OPENBL_FlashLayout_Free();
          (void)memset(&FlashlayoutStruct, 0, sizeof(FlashlayoutStruct));
        }

        PhaseId = PHASE_FLASHLAYOUT;
        DestinationAddress = FLASHLAYOUT_ADDRESS;
      }
      else if (NextItem < FlashlayoutStruct.partsize)
      {
        /* Flash layout line NextItem. The line is addressed by its index, not by its
           phase id, so that every line gets its own destination address. Its id is
           reported: the host selects the alternate setting whose interface string
           carries that partition id. */
        PhaseId = (uint8_t)FlashlayoutStruct.id[NextItem];
        DestinationAddress = OPENBL_USB_GetAddress(NextItem);
      }
      else
      {
        /* Every line has been announced and "started": the end of operation, announced
           once. It is not a phase and takes no part in the phase/start alternation. */
        pDest[0] = (uint8_t)PHASE_END;
        (void)memset(&(pDest[1]), 0U, 8U);

        NextItem           = LAYOUT_DONE;
        IsStartOperation   = false;
        DestinationAddress = UNDEF_ADDRESS;

        return pDest;
      }

      /* Get phase command response */
      pDest[0] = PhaseId;
      pDest[1] = (uint8_t)(DestinationAddress >> 0U);
      pDest[2] = (uint8_t)(DestinationAddress >> 8U);
      pDest[3] = (uint8_t)(DestinationAddress >> 16U);
      pDest[4] = (uint8_t)(DestinationAddress >> 24U);
      pDest[5] = 0x00U;
      pDest[6] = 0x00U;
      pDest[7] = 0x00U;
      pDest[8] = 0x00U;

      /* Start operation call after each phase operation */
      /* If current operation is phase operation, next operation is start operation */
      if (IsStartOperation) /* Start operation */
      {
        /* Next operation is phase operation */
        IsStartOperation = false;

        /* The item just "started" is done: announce the next one */
        NextItem = (NextItem == LAYOUT_ITEM) ? OPENBL_USB_FirstLine() : (NextItem + 1U);
      }
      else /* Phase operation: the host is about to download this item */
      {
        /* Next operation is start operation */
        IsStartOperation = true;

        /* The download of a new line starts from its base address */
        ExtMemWriteOffset = 0U;
        RamWriteOffset    = 0U;
      }
      break;

    case PHASE_OTP:
      if (BlockNumber == 0U)
      {
        Otp = OPENBL_OTP_Read();

        /* Get OTP version */
        pDest[0] = (uint8_t)Otp.Version;
        pDest[1] = (uint8_t)(Otp.Version >> 8U);
        pDest[2] = (uint8_t)(Otp.Version >> 16U);
        pDest[3] = (uint8_t)(Otp.Version >> 24U);

        /* Get OTP global state */
        pDest[4] = (uint8_t)Otp.GlobalState;
        pDest[5] = (uint8_t)(Otp.GlobalState >> 8U);
        pDest[6] = (uint8_t)(Otp.GlobalState >> 16U);
        pDest[7] = (uint8_t)(Otp.GlobalState >> 24U);

        OtpIndex = 0U;

        /* Get OTP values and status */
        for (i = 8U; ((i < Length) && (OtpIndex < OTP_PART_SIZE)); i += 4U)
        {
          /* 127 OTP sent for the first 1024 bytes block */
          /* BlockNumber == 0 : OtpIndex = 0..251 - 127 OTP x 8 bytes */
          pDest[i]      = (uint8_t)(Otp.OtpPart[OtpIndex]);
          pDest[i + 1U] = (uint8_t)(Otp.OtpPart[OtpIndex] >> 8U);
          pDest[i + 2U] = (uint8_t)(Otp.OtpPart[OtpIndex] >> 16U);
          pDest[i + 3U] = (uint8_t)(Otp.OtpPart[OtpIndex] >> 24U);

          OtpIndex++;
        }

      }
      else
      {
        OtpIndex = (254U + ((BlockNumber - 1U) * 256U));

        for (i = 0U; ((i < Length) && (OtpIndex < OTP_PART_SIZE)); i += 4U)
        {
          /*
           * BlockNumber == 1 : OtpIndex = 254..505 - 128 OTP x 8 bytes = 1024 bytes - 8 bytes = 4 byes for OTP value & 4 bytes for status
           * BlockNumber == 2 : OtpIndex = 506..761 - 1024 bytes
           * BlockNumber == 3 : OtpIndex = 762..767 - 1024 bytes
           */

          pDest[i]      = (uint8_t)(Otp.OtpPart[OtpIndex]);
          pDest[i + 1U] = (uint8_t)(Otp.OtpPart[OtpIndex] >> 8U);
          pDest[i + 2U] = (uint8_t)(Otp.OtpPart[OtpIndex] >> 16U);
          pDest[i + 3U] = (uint8_t)(Otp.OtpPart[OtpIndex] >> 24U);

          OtpIndex++;
        }
      }
      break;

    default:
      break;
  }

  /* Return a valid address to avoid HardFault */
  return pDest;
}

/**
  * @brief  This function is used to jump to the user application.
  * @param  Address The jump address.
  * @retval None.
  */
void OPENBL_USB_Jump(uint32_t Address)
{
  uint8_t status;

  /* Check if received address is valid or not */
  status = OPENBL_MEM_CheckJumpAddress(Address);

  if (status == 1U)
  {
    OPENBL_MEM_JumpToAddress(Address);
  }
}

/**
  * @brief  Write protect.
  * @param  pBuffer A buffer that contains the list of sectors or pages to be protected.
  * @param  Length Contains the length of the pBuffer.
  * @retval None.
  */
void OPENBL_USB_WriteProtect(uint8_t *pBuffer, uint32_t Length)
{
  ErrorStatus error_value;

  error_value = OPENBL_MEM_SetWriteProtection(ENABLE, OPENBL_DEFAULT_MEM, pBuffer, Length);

  if (error_value == SUCCESS)
  {
    /* Start post processing task if needed */
    Common_StartPostProcessing();
  }
}

/**
  * @brief  Write unprotect.
  * @retval None.
  */
void OPENBL_USB_WriteUnprotect(void)
{
  ErrorStatus error_value;

  error_value = OPENBL_MEM_SetWriteProtection(DISABLE, OPENBL_DEFAULT_MEM, NULL, 0U);

  if (error_value == SUCCESS)
  {
    /* Start post processing task if needed */
    Common_StartPostProcessing();
  }
}

/**
  * @brief  Read protect.
  * @retval None.
  */
void OPENBL_USB_ReadProtect(void)
{
  /* Enable the read protection */
  OPENBL_MEM_SetReadOutProtection(OPENBL_DEFAULT_MEM, ENABLE);

  /* Start post processing task if needed */
  Common_StartPostProcessing();
}

/**
  * @brief  Read unprotect.
  * @retval None.
  */
void OPENBL_USB_ReadUnprotect(void)
{
  /* Disable the read protection */
  OPENBL_MEM_SetReadOutProtection(OPENBL_DEFAULT_MEM, DISABLE);

  /* Start post processing task if needed */
  Common_StartPostProcessing();
}

/**
  * @brief  Link between USB Alternate and phase ID
  *         The phase ID of an alternate setting is the partition id of its
  *         interface string ("/0x<id>/"), see usbd_dfu_alt_conf.h.
  * @param  Alternate USB alternate value sent by the host.
  * @retval Returns phase ID, PHASE_END if the alternate setting does not exist.
  */
static uint8_t OPENBL_USB_GetPhase(uint32_t Alternate)
{
  uint32_t part_id;

  if (USBD_DFU_GetAltPartId((uint8_t)Alternate, &part_id) != 0U)
  {
    return (uint8_t)PHASE_END;
  }

  return (uint8_t)part_id;
}

/**
  * @brief  Get address following a given flash layout line.
  *         - the external memory loader (phase 0x03): FLASH_LOADER_WRITE_ADDRESS, where
  *           OPENBL_ExtMem_Init() reads the function table of the loader (as the ST original)
  *         - "ip" none, another line  : RAM
  *         - "ip" nor0 / nor ...      : external memory, at the offset given in the flash layout
  *           The flash layouts of the ST OpenBootloader package use "nor0" (and CubeProgrammer
  *           numbers the devices: nor0, nor1, ...), so the device name is matched by prefix.
  * @param  layout_index Index of the flash layout line.
  * @retval Returns the destination address of the flash layout line,
  *         UNDEF_ADDRESS if the "ip" is not supported.
  */
static uint32_t OPENBL_USB_GetAddress(uint32_t layout_index)
{
  const char *ip;
  uint32_t address;

  if (layout_index >= FlashlayoutStruct.partsize)
  {
    /* End of the flash layout */
    return UNDEF_ADDRESS;
  }

  if (FlashlayoutStruct.id[layout_index] == PHASE_3)
  {
    /* The external memory loader */
    return FLASH_LOADER_WRITE_ADDRESS;
  }

  ip = FlashlayoutStruct.ip[layout_index];

  if (strcmp(ip, "none") == 0)
  {
    address = RAM_WRITE_ADDRESS;
  }
  else if ((strncmp(ip, "nor", 3U) == 0) || (strncmp(ip, "mmc", 3U) == 0))
  {
    /* Compute the destination address by using the offset provided by the user
       in the flash layout, for THIS line (the phase id is what the host uses to
       select the alternate setting; the line index is what gives the address) */
    address = FlashlayoutStruct.offset[layout_index];

    /* The offset must stay inside the external memory: an offset that wraps or
       overflows would otherwise give an address outside of it, which the download
       would route to the RAM path */
    if (address < EXT_MEMORY_SIZE)
    {
      address += EXT_MEMORY_START_ADDRESS;
    }
    else
    {
      address = UNDEF_ADDRESS;
    }
  }
  else
  {
    /* Only none, nor and mmc are supported */
    address = UNDEF_ADDRESS;
  }

  return address;
}

/**
  * @brief  Tell whether an address belongs to the external memory area.
  * @param  Address Address to check.
  * @retval Returns 1 if the address is in the external memory, 0 otherwise.
  */
static uint32_t OPENBL_USB_IsExtMemAddress(uint32_t Address)
{
  return ((Address >= EXT_MEMORY_START_ADDRESS) && (Address < EXT_MEMORY_END_ADDRESS)) ? 1U : 0U;
}
