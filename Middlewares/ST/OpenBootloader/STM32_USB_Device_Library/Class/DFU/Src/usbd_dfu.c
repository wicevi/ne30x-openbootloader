/**
  ******************************************************************************
  * @file    usbd_dfu.c
  * @author  MCD Application Team
  * @brief   This file provides the DFU core functions.
  *
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2015 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  * @verbatim
  *
  *          ===================================================================
  *                                DFU Class Driver Description
  *          ===================================================================
  *           This driver manages the DFU class V1.1 following the "Device Class Specification for
  *           Device Firmware Upgrade Version 1.1 Aug 5, 2004".
  *           This driver implements the following aspects of the specification:
  *             - Device descriptor management
  *             - Configuration descriptor management
  *             - Enumeration as DFU device (in DFU mode only)
  *             - Requests management (supporting ST DFU sub-protocol)
  *             - Memory operations management (Download/Upload/Erase/Detach/GetState/GetStatus)
  *             - DFU state machine implementation.
  *
  *           @note
  *            ST DFU sub-protocol is compliant with DFU protocol and use sub-requests to manage
  *            memory addressing, commands processing, specific memories operations (ie. Erase) ...
  *            As required by the DFU specification, only endpoint 0 is used in this application.
  *            Other endpoints and functions may be added to the application (ie. DFU ...)
  *
  *           These aspects may be enriched or modified for a specific user application.
  *
  *           This driver doesn't implement the following aspects of the specification
  *           (but it is possible to manage these features with some modifications on this driver):
  *             - Manifestation Tolerant mode
  *
  *  @endverbatim
  *
  ******************************************************************************
  */

/* BSPDependencies
- "stm32xxxxx_{eval}{discovery}{nucleo_144}.c"
- "stm32xxxxx_{eval}{discovery}_io.c"
EndBSPDependencies */

/* Includes ------------------------------------------------------------------*/
#include "usbd_dfu.h"
#include "usbd_ctlreq.h"
#include <stdio.h>
#include <string.h>

/** @addtogroup STM32_USB_DEVICE_LIBRARY
  @{
  */

/** @defgroup USBD_DFU
  * @brief usbd core module
  * @{
  */

/** @defgroup USBD_DFU_Private_TypesDefinitions
  * @{
  */

/* One DFU alternate setting, as described by the interface string
   "@<name> /0x<partition id>/1*<size><unit>e" */
typedef struct
{
  uint32_t    PartId;     /* Partition id ("/0x<id>/" field) */
  const char *Name;       /* Name before the "/0x<id>/" field */
  uint32_t    Size;       /* Descriptive size */
  char        Unit;       /* Descriptive size unit */
} USBD_DFU_AltTypeDef;

/* Table rows are X(alt, name, id, size, unit); "size unit" is passed as a single
   argument pair (see DFU_ALT_SIZE_NOR), hence the variadic form */
#define USBD_DFU_ALT_ENTRY(alt, name, id, size_unit...) { (id), (name), size_unit },

/**
  * @}
  */


/** @defgroup USBD_DFU_Private_Defines
  * @{
  */

/**
  * @}
  */


/** @defgroup USBD_DFU_Private_Macros
  * @{
  */

/**
  * @}
  */


/** @defgroup USBD_DFU_Private_FunctionPrototypes
  * @{
  */

static uint8_t USBD_DFU_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_DFU_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_DFU_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static uint8_t USBD_DFU_EP0_RxReady(USBD_HandleTypeDef *pdev);
static uint8_t USBD_DFU_EP0_TxReady(USBD_HandleTypeDef *pdev);
static uint8_t USBD_DFU_SOF(USBD_HandleTypeDef *pdev);

#ifndef USE_USBD_COMPOSITE
static uint8_t *USBD_DFU_GetCfgDesc(uint16_t *length);
static uint8_t *USBD_DFU_GetDeviceQualifierDesc(uint16_t *length);
#endif /* USE_USBD_COMPOSITE */

#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
static uint8_t *USBD_DFU_GetUsrStringDesc(USBD_HandleTypeDef *pdev,
                                          uint8_t index, uint16_t *length);
static uint8_t *USBD_DFU_GetAltStrDesc(USBD_HandleTypeDef *pdev,
                                       uint8_t alt, uint16_t *length);
#endif /* USBD_SUPPORT_USER_STRING_DESC */

#ifndef USE_USBD_COMPOSITE
static void USBD_DFU_BuildCfgDesc(void);
#endif /* USE_USBD_COMPOSITE */

static void DFU_Detach(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static void DFU_Download(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static void DFU_Upload(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static void DFU_GetStatus(USBD_HandleTypeDef *pdev);
static void DFU_ClearStatus(USBD_HandleTypeDef *pdev);
static void DFU_GetState(USBD_HandleTypeDef *pdev);
static void DFU_Abort(USBD_HandleTypeDef *pdev);
static void DFU_Leave(USBD_HandleTypeDef *pdev);
static void *USBD_DFU_GetDfuFuncDesc(uint8_t *pConfDesc);

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Variables
  * @{
  */

USBD_ClassTypeDef USBD_DFU =
{
  USBD_DFU_Init,
  USBD_DFU_DeInit,
  USBD_DFU_Setup,
  USBD_DFU_EP0_TxReady,
  USBD_DFU_EP0_RxReady,
  NULL,
  NULL,
  USBD_DFU_SOF,
  NULL,
  NULL,
#ifdef USE_USBD_COMPOSITE
  NULL,
  NULL,
  NULL,
  NULL,
#else
  USBD_DFU_GetCfgDesc,
  USBD_DFU_GetCfgDesc,
  USBD_DFU_GetCfgDesc,
  USBD_DFU_GetDeviceQualifierDesc,
#endif /* USE_USBD_COMPOSITE */
#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
  USBD_DFU_GetUsrStringDesc
#endif /* USBD_SUPPORT_USER_STRING_DESC */
};

/* Alternate setting table, in alternate-setting order (see usbd_dfu_alt_conf.h) */
static const USBD_DFU_AltTypeDef USBD_DFU_AltTable[USBD_DFU_MAX_ITF_NUM] =
{
  USBD_DFU_ALT_TABLE(USBD_DFU_ALT_ENTRY)
};

#ifndef USE_USBD_COMPOSITE
/* USB DFU device Configuration Descriptor, built from the alternate setting table
   by USBD_DFU_BuildCfgDesc() */
__ALIGN_BEGIN static uint8_t USBD_DFU_CfgDesc[USB_DFU_CONFIG_DESC_SIZ] __ALIGN_END;

/* USB Standard Device Qualifier Descriptor */
__ALIGN_BEGIN static uint8_t USBD_DFU_DeviceQualifierDesc[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END =
{
  USB_LEN_DEV_QUALIFIER_DESC,
  USB_DESC_TYPE_DEVICE_QUALIFIER,
  0x00,
  0x02,
  0x00,
  0x00,
  0x00,
  0x40,
  0x01,
  0x00,
};
#endif /* USE_USBD_COMPOSITE */

#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
/* Alternate setting string descriptor (UTF-16, filled by USBD_DFU_GetAltStrDesc) */
static uint8_t USBD_DFU_AltStrDesc[USBD_DFU_ALT_STR_SIZ];
#endif /* USBD_SUPPORT_USER_STRING_DESC */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Functions
  * @{
  */

#ifndef USE_USBD_COMPOSITE
/**
  * @brief  USBD_DFU_BuildCfgDesc
  *         Build the DFU configuration descriptor: configuration descriptor, one
  *         interface descriptor per alternate setting, DFU functional descriptor.
  * @param  none
  * @retval none
  */
static void USBD_DFU_BuildCfgDesc(void)
{
  uint8_t *desc = USBD_DFU_CfgDesc;
  uint32_t alt;

  desc[0] = 0x09U;                                      /* bLength: Configuration Descriptor size */
  desc[1] = USB_DESC_TYPE_CONFIGURATION;                /* bDescriptorType: Configuration */
  desc[2] = (uint8_t)(USB_DFU_CONFIG_DESC_SIZ & 0xFFU); /* wTotalLength: Bytes returned */
  desc[3] = (uint8_t)(USB_DFU_CONFIG_DESC_SIZ >> 8);
  desc[4] = 0x01U;                                      /* bNumInterfaces: 1 interface */
  desc[5] = 0x01U;                                      /* bConfigurationValue: Configuration value */
  desc[6] = 0x02U;                                      /* iConfiguration */
#if (USBD_SELF_POWERED == 1U)
  desc[7] = 0xC0U;                                      /* bmAttributes */
#else
  desc[7] = 0x80U;                                      /* bmAttributes */
#endif /* USBD_SELF_POWERED == 1U */
  desc[8] = USBD_MAX_POWER;                             /* MaxPower (mA) */
  desc += 9U;

  /* One interface descriptor per alternate setting */
  for (alt = 0U; alt < USBD_DFU_MAX_ITF_NUM; alt++)
  {
    USBD_DFU_IF_DESC_INIT(alt, desc);
    desc += 9U;
  }

  /* DFU Functional Descriptor */
  desc[0] = 0x09U;                                      /* bLength = 9 Bytes */
  desc[1] = DFU_DESCRIPTOR_TYPE;                        /* DFU Functional Descriptor */
  desc[2] = 0x0FU;                                      /* bmAttributes:
                                                           bitCanDnload             = 1      (bit 0)
                                                           bitCanUpload             = 1      (bit 1)
                                                           bitManifestationTolerant = 1      (bit 2)
                                                           bitWillDetach            = 1      (bit 3) */
  desc[3] = (uint8_t)(USBD_DFU_DETACH_TIMEOUT & 0xFFU); /* wDetachTimeOut */
  desc[4] = (uint8_t)(USBD_DFU_DETACH_TIMEOUT >> 8);
  /* WARNING: In DMA mode the multiple MPS packets feature is still not supported
     ==> In this case, when using DMA USBD_DFU_XFER_SIZE should be set to 64 in usbd_conf.h */
  desc[5] = (uint8_t)(USBD_DFU_XFER_SIZE & 0xFFU);      /* wTransferSize */
  desc[6] = (uint8_t)(USBD_DFU_XFER_SIZE >> 8);
  desc[7] = 0x10U;                                      /* bcdDFUVersion */
  desc[8] = 0x01U;
}
#endif /* USE_USBD_COMPOSITE */

/**
  * @brief  USBD_DFU_Init
  *         Initialize the DFU interface
  * @param  pdev: device instance
  * @param  cfgidx: Configuration index
  * @retval status
  */
static uint8_t USBD_DFU_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);

  USBD_DFU_HandleTypeDef *hdfu;

  /* Allocate Audio structure */
  hdfu = (USBD_DFU_HandleTypeDef *)USBD_malloc(sizeof(USBD_DFU_HandleTypeDef));

  if (hdfu == NULL)
  {
    pdev->pClassDataCmsit[pdev->classId] = NULL;
    return (uint8_t)USBD_EMEM;
  }

  pdev->pClassDataCmsit[pdev->classId] = (void *)hdfu;
  pdev->pClassData = pdev->pClassDataCmsit[pdev->classId];

  hdfu->alt_setting = 0U;
  hdfu->data_ptr = USBD_DFU_APP_DEFAULT_ADD;
  hdfu->wblock_num = 0U;
  hdfu->wlength = 0U;

  hdfu->manif_state = DFU_MANIFEST_COMPLETE;
  hdfu->dev_state = DFU_STATE_IDLE;

  hdfu->dev_status[0] = DFU_ERROR_NONE;
  hdfu->dev_status[1] = 0U;
  hdfu->dev_status[2] = 0U;
  hdfu->dev_status[3] = 0U;
  hdfu->dev_status[4] = DFU_STATE_IDLE;
  hdfu->dev_status[5] = 0U;

  /* Initialize Hardware layer */
  if (((USBD_DFU_MediaTypeDef *)pdev->pUserData[pdev->classId])->Init() != USBD_OK)
  {
    return (uint8_t)USBD_FAIL;
  }

  return (uint8_t)USBD_OK;
}

/**
  * @brief  USBD_DFU_DeInit
  *         De-Initialize the DFU layer
  * @param  pdev: device instance
  * @param  cfgidx: Configuration index
  * @retval status
  */
static uint8_t USBD_DFU_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);
  USBD_DFU_HandleTypeDef *hdfu;

  if (pdev->pClassDataCmsit[pdev->classId] == NULL)
  {
    return (uint8_t)USBD_EMEM;
  }

  hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];
  hdfu->wblock_num = 0U;
  hdfu->wlength = 0U;

  hdfu->dev_state = DFU_STATE_IDLE;
  hdfu->dev_status[0] = DFU_ERROR_NONE;
  hdfu->dev_status[4] = DFU_STATE_IDLE;

  /* DeInit  physical Interface components and Hardware Layer */
  ((USBD_DFU_MediaTypeDef *)pdev->pUserData[pdev->classId])->DeInit();
  USBD_free(pdev->pClassDataCmsit[pdev->classId]);
  pdev->pClassDataCmsit[pdev->classId] = NULL;
  pdev->pClassData = NULL;

  return (uint8_t)USBD_OK;
}

/**
  * @brief  USBD_DFU_Setup
  *         Handle the DFU specific requests
  * @param  pdev: instance
  * @param  req: usb requests
  * @retval status
  */
static uint8_t USBD_DFU_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];
  USBD_StatusTypeDef ret = USBD_OK;
  uint8_t *pbuf;
  uint16_t len;
  uint16_t status_info = 0U;

  if (hdfu == NULL)
  {
    return (uint8_t)USBD_FAIL;
  }

  switch (req->bmRequest & USB_REQ_TYPE_MASK)
  {
    case USB_REQ_TYPE_CLASS:
      switch (req->bRequest)
      {
        case DFU_DNLOAD:
          DFU_Download(pdev, req);
          break;

        case DFU_UPLOAD:
          DFU_Upload(pdev, req);
          break;

        case DFU_GETSTATUS:
          DFU_GetStatus(pdev);
          break;

        case DFU_CLRSTATUS:
          DFU_ClearStatus(pdev);
          break;

        case DFU_GETSTATE:
          DFU_GetState(pdev);
          break;

        case DFU_ABORT:
          DFU_Abort(pdev);
          break;

        case DFU_DETACH:
          DFU_Detach(pdev, req);
          break;

        default:
          USBD_CtlError(pdev, req);
          ret = USBD_FAIL;
          break;
      }
      break;

    case USB_REQ_TYPE_STANDARD:
      switch (req->bRequest)
      {
        case USB_REQ_GET_STATUS:
          if (pdev->dev_state == USBD_STATE_CONFIGURED)
          {
            (void)USBD_CtlSendData(pdev, (uint8_t *)&status_info, 2U);
          }
          else
          {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;

        case USB_REQ_GET_DESCRIPTOR:
          if ((req->wValue >> 8) == DFU_DESCRIPTOR_TYPE)
          {
            pbuf = (uint8_t *)USBD_DFU_GetDfuFuncDesc(pdev->pConfDesc);

            if (pbuf != NULL)
            {
              len = MIN(USB_DFU_DESC_SIZ, req->wLength);
              (void)USBD_CtlSendData(pdev, pbuf, len);
            }
            else
            {
              USBD_CtlError(pdev, req);
              ret = USBD_FAIL;
            }
          }
          break;

        case USB_REQ_GET_INTERFACE:
          if (pdev->dev_state == USBD_STATE_CONFIGURED)
          {
            (void)USBD_CtlSendData(pdev, (uint8_t *)&hdfu->alt_setting, 1U);
          }
          else
          {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;

        case USB_REQ_SET_INTERFACE:
          if ((uint8_t)(req->wValue) < USBD_DFU_MAX_ITF_NUM)
          {
            if (pdev->dev_state == USBD_STATE_CONFIGURED)
            {
              hdfu->alt_setting = (uint8_t)(req->wValue);
            }
            else
            {
              USBD_CtlError(pdev, req);
              ret = USBD_FAIL;
            }
          }
          else
          {
            /* Call the error management function (command will be NAKed */
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;

        case USB_REQ_CLEAR_FEATURE:
          break;

        default:
          USBD_CtlError(pdev, req);
          ret = USBD_FAIL;
          break;
      }
      break;

    default:
      USBD_CtlError(pdev, req);
      ret = USBD_FAIL;
      break;
  }

  return (uint8_t)ret;
}

#ifndef USE_USBD_COMPOSITE
/**
  * @brief  USBD_DFU_GetCfgDesc
  *         return configuration descriptor
  * @param  length : pointer data length
  * @retval pointer to descriptor buffer
  */
static uint8_t *USBD_DFU_GetCfgDesc(uint16_t *length)
{
  static uint8_t cfg_desc_built = 0U;

  /* The descriptor is derived from the alternate setting table; build it once */
  if (cfg_desc_built == 0U)
  {
    USBD_DFU_BuildCfgDesc();
    cfg_desc_built = 1U;
  }

  *length = (uint16_t)sizeof(USBD_DFU_CfgDesc);

  return USBD_DFU_CfgDesc;
}
#endif /* USE_USBD_COMPOSITE */

/**
  * @brief  USBD_DFU_EP0_RxReady
  *         handle EP0 Rx Ready event
  * @param  pdev: device instance
  * @retval status
  */
static uint8_t USBD_DFU_EP0_RxReady(USBD_HandleTypeDef *pdev)
{
  UNUSED(pdev);

  return (uint8_t)USBD_OK;
}
/**
  * @brief  USBD_DFU_EP0_TxReady
  *         handle EP0 TRx Ready event
  * @param  pdev: device instance
  * @retval status
  */
static uint8_t  USBD_DFU_EP0_TxReady(USBD_HandleTypeDef *pdev)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];
  USBD_DFU_MediaTypeDef *DfuInterface = (USBD_DFU_MediaTypeDef *)pdev->pUserData[pdev->classId];

  if (hdfu == NULL)
  {
    return (uint8_t)USBD_FAIL;
  }

  if (hdfu->dev_state == DFU_STATE_DNLOAD_BUSY)
  {
    /* Perform the write operation */
    if (DfuInterface->Write(hdfu->buffer.d8, hdfu->alt_setting, hdfu->wlength, hdfu->wblock_num) != USBD_OK)
    {
      /* The status stage of the DNLOAD request is already complete and the USB core
         ignores the return value here, so the failure is reported through the DFU
         state: the next GETSTATUS returns errTarget in the dfuERROR state and the
         host aborts, instead of going on with a partition written incorrectly */
      hdfu->dev_state = DFU_STATE_ERROR;
      hdfu->dev_status[0] = DFU_ERROR_TARGET;
      hdfu->dev_status[1] = 0U;
      hdfu->dev_status[2] = 0U;
      hdfu->dev_status[3] = 0U;
      hdfu->dev_status[4] = hdfu->dev_state;
      hdfu->dev_status[5] = 0U;
      hdfu->wlength = 0U;
      hdfu->wblock_num = 0U;

      return (uint8_t)USBD_FAIL;
    }

    /* Reset the global length and block number */
    hdfu->wlength = 0U;
    hdfu->wblock_num = 0U;

    /* Update the state machine */
    hdfu->dev_state =  DFU_STATE_DNLOAD_SYNC;

    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U;
    hdfu->dev_status[4] = hdfu->dev_state;
  }
  else if (hdfu->dev_state == DFU_STATE_MANIFEST)/* Manifestation in progress */
  {
    /* Start leaving DFU mode */
    DFU_Leave(pdev);
  }
  else
  {
    return (uint8_t)USBD_FAIL;
  }

  return (uint8_t)USBD_OK;
}
/**
  * @brief  USBD_DFU_SOF
  *         handle SOF event
  * @param  pdev: device instance
  * @retval status
  */
static uint8_t USBD_DFU_SOF(USBD_HandleTypeDef *pdev)
{
  UNUSED(pdev);

  return (uint8_t)USBD_OK;
}

#ifndef USE_USBD_COMPOSITE
/**
  * @brief  DeviceQualifierDescriptor
  *         return Device Qualifier descriptor
  * @param  length : pointer data length
  * @retval pointer to descriptor buffer
  */
static uint8_t *USBD_DFU_GetDeviceQualifierDesc(uint16_t *length)
{
  *length = (uint16_t)sizeof(USBD_DFU_DeviceQualifierDesc);

  return USBD_DFU_DeviceQualifierDesc;
}
#endif /* USE_USBD_COMPOSITE */

/**
  * @brief  USBD_DFU_GetUsrStringDesc
  *         Manages the transfer of memory interfaces string descriptors.
  * @param  pdev: device instance
  * @param  index: descriptor index
  * @param  length : pointer data length
  * @retval pointer to the descriptor table or NULL if the descriptor is not supported.
  */
#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
static uint8_t *USBD_DFU_GetUsrStringDesc(USBD_HandleTypeDef *pdev, uint8_t index, uint16_t *length)
{
  /* One string descriptor per alternate setting, index = USBD_IDX_INTERFACE_STR + alt + 1 */
  if ((index > USBD_IDX_INTERFACE_STR) && ((index - USBD_IDX_INTERFACE_STR - 1U) < USBD_DFU_MAX_ITF_NUM))
  {
    return USBD_DFU_GetAltStrDesc(pdev, (uint8_t)(index - USBD_IDX_INTERFACE_STR - 1U), length);
  }

  /* Not supported Interface Descriptor index */
  *length = 0U;
  return NULL;
}

/**
  * @brief  USBD_DFU_GetAltStrDesc
  *         Return the string descriptor "@<name> /0x<partition id>/1*<size><unit>e" of
  *         the DFU alternate setting alt.
  * @param  pdev: device instance
  * @param  alt: alternate setting index
  * @param  length : pointer data length
  * @retval pointer to the descriptor buffer, or NULL if alt is out of range.
  */
static uint8_t *USBD_DFU_GetAltStrDesc(USBD_HandleTypeDef *pdev, uint8_t alt, uint16_t *length)
{
  UNUSED(pdev);
  const USBD_DFU_AltTypeDef *entry;
  char ascii[USBD_DFU_ALT_STR_SIZ];
  int len;

  if (alt >= USBD_DFU_MAX_ITF_NUM)
  {
    *length = 0U;
    return NULL;
  }

  entry = &USBD_DFU_AltTable[alt];

  /* Format expected by STM32CubeProgrammer to associate an alternate setting to a partition id */
  len = snprintf(ascii, sizeof(ascii), "@%s /0x%02lX/1*%lu%c%c",
                 entry->Name, (unsigned long)entry->PartId, (unsigned long)entry->Size, entry->Unit, 'e');
  if ((len <= 0) || (len >= (int)sizeof(ascii)))
  {
    *length = 0U;
    return NULL;
  }

  USBD_GetString((uint8_t *)ascii, USBD_DFU_AltStrDesc, length);

  return USBD_DFU_AltStrDesc;
}
#endif /* USBD_SUPPORT_USER_STRING_DESC */

/**
  * @brief  USBD_DFU_GetAltPartId
  *         Give the partition id of a DFU alternate setting, i.e. the "/0x<id>/" field
  *         of its interface string, which STM32CubeProgrammer matches against the
  *         flash layout "Id" column.
  * @param  alt: alternate setting index
  * @param  part_id: pointer to the partition id
  * @retval USBD_OK if the alternate setting exists, USBD_FAIL otherwise.
  */
uint8_t USBD_DFU_GetAltPartId(uint8_t alt, uint32_t *part_id)
{
  if ((alt >= USBD_DFU_MAX_ITF_NUM) || (part_id == NULL))
  {
    return (uint8_t)USBD_FAIL;
  }

  *part_id = USBD_DFU_AltTable[alt].PartId;

  return (uint8_t)USBD_OK;
}

/**
  * @brief  USBD_MSC_RegisterStorage
  * @param  pdev: device instance
  * @param  fops: storage callback
  * @retval status
  */
uint8_t USBD_DFU_RegisterMedia(USBD_HandleTypeDef *pdev,
                               USBD_DFU_MediaTypeDef *fops)
{
  if (fops == NULL)
  {
    return (uint8_t)USBD_FAIL;
  }

  pdev->pUserData[pdev->classId] = fops;

  return (uint8_t)USBD_OK;
}

/******************************************************************************
     DFU Class requests management
  ******************************************************************************/
/**
  * @brief  DFU_Detach
  *         Handles the DFU DETACH request.
  * @param  pdev: device instance
  * @param  req: pointer to the request structure.
  * @retval None.
  */
static void DFU_Detach(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];
  USBD_DFUFuncDescTypeDef *pDfuFunc = (USBD_DFUFuncDescTypeDef *)USBD_DFU_GetDfuFuncDesc(pdev->pConfDesc);

  if ((hdfu == NULL) || (pDfuFunc == NULL))
  {
    return;
  }

  if ((hdfu->dev_state == DFU_STATE_IDLE) ||
      (hdfu->dev_state == DFU_STATE_DNLOAD_SYNC) ||
      (hdfu->dev_state == DFU_STATE_DNLOAD_IDLE) ||
      (hdfu->dev_state == DFU_STATE_MANIFEST_SYNC) ||
      (hdfu->dev_state == DFU_STATE_UPLOAD_IDLE))
  {
    /* Update the state machine */
    hdfu->dev_state = DFU_STATE_IDLE;
    hdfu->dev_status[0] = DFU_ERROR_NONE;
    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U; /*bwPollTimeout=0ms*/
    hdfu->dev_status[4] = hdfu->dev_state;
    hdfu->dev_status[5] = 0U; /*iString*/
    hdfu->wblock_num = 0U;
    hdfu->wlength = 0U;
  }

  /* Check the detach capability in the DFU functional descriptor */
  if ((pDfuFunc->bmAttributes & DFU_DETACH_MASK) != 0U)
  {
    /* Perform an Attach-Detach operation on USB bus */
    (void)USBD_Stop(pdev);
    (void)USBD_Start(pdev);
  }
  else
  {
    /* Wait for the period of time specified in Detach request */
    USBD_Delay((uint32_t)req->wValue);
  }
}

/**
  * @brief  DFU_Download
  *         Handles the DFU DNLOAD request.
  * @param  pdev: device instance
  * @param  req: pointer to the request structure
  * @retval None
  */
static void DFU_Download(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];

  if (hdfu == NULL)
  {
    return;
  }

  /* Data setup request */
  if (req->wLength > 0U)
  {
    if ((hdfu->dev_state == DFU_STATE_IDLE) || (hdfu->dev_state == DFU_STATE_DNLOAD_IDLE))
    {
      /* Update the global length and block number */
      hdfu->wblock_num = req->wValue;
      hdfu->wlength = MIN(req->wLength, USBD_DFU_XFER_SIZE);

      /* Update the state machine */
      hdfu->dev_state = DFU_STATE_DNLOAD_SYNC;
      hdfu->dev_status[4] = hdfu->dev_state;

      /* Prepare the reception of the buffer over EP0 */
      (void)USBD_CtlPrepareRx(pdev, (uint8_t *)hdfu->buffer.d8, hdfu->wlength);
    }
    /* Unsupported state */
    else
    {
      /* Call the error management function (command will be NAKed */
      USBD_CtlError(pdev, req);
    }
  }
  /* 0 Data DNLOAD request */
  else
  {
    /* End of DNLOAD operation*/
    if ((hdfu->dev_state == DFU_STATE_DNLOAD_IDLE) || (hdfu->dev_state == DFU_STATE_IDLE))
    {
      hdfu->manif_state = DFU_MANIFEST_IN_PROGRESS;
      hdfu->dev_state = DFU_STATE_MANIFEST_SYNC;
      hdfu->dev_status[1] = 0U;
      hdfu->dev_status[2] = 0U;
      hdfu->dev_status[3] = 0U;
      hdfu->dev_status[4] = hdfu->dev_state;
    }
    else
    {
      /* Call the error management function (command will be NAKed */
      USBD_CtlError(pdev, req);
    }
  }
}

/**
  * @brief  DFU_Upload
  *         Handles the DFU UPLOAD request.
  * @param  pdev: instance
  * @param  req: pointer to the request structure
  * @retval status
  */
static void DFU_Upload(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];
  USBD_DFU_MediaTypeDef *DfuInterface = (USBD_DFU_MediaTypeDef *)pdev->pUserData[pdev->classId];
  uint8_t *phaddr;

  if (hdfu == NULL)
  {
    return;
  }

  /* Data setup request */
  if (req->wLength > 0U)
  {
    if ((hdfu->dev_state == DFU_STATE_IDLE) || (hdfu->dev_state == DFU_STATE_UPLOAD_IDLE))
    {
      /* Update the global length and block number */
      hdfu->wblock_num = req->wValue;
      hdfu->wlength = MIN(req->wLength, USBD_DFU_XFER_SIZE);

      hdfu->dev_state = DFU_STATE_UPLOAD_IDLE;

      hdfu->dev_status[1] = 0U;
      hdfu->dev_status[2] = 0U;
      hdfu->dev_status[3] = 0U;
      hdfu->dev_status[4] = hdfu->dev_state;

      /* Return the physical address where data are stored */
      phaddr = DfuInterface->Read(hdfu->alt_setting, hdfu->buffer.d8, hdfu->wlength, hdfu->wblock_num);

      /* Send the status data over EP0 */
      (void)USBD_CtlSendData(pdev, phaddr, hdfu->wlength);
    }
    /* Unsupported state */
    else
    {
      hdfu->wlength = 0U;
      hdfu->wblock_num = 0U;

      /* Call the error management function (command will be NAKed */
      USBD_CtlError(pdev, req);
    }
  }
  /* No Data setup request */
  else
  {
    hdfu->dev_state = DFU_STATE_IDLE;

    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U;
    hdfu->dev_status[4] = hdfu->dev_state;
  }
}

/**
  * @brief  DFU_GetStatus
  *         Handles the DFU GETSTATUS request.
  * @param  pdev: instance
  * @retval status
  */
static void DFU_GetStatus(USBD_HandleTypeDef *pdev)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];
  USBD_DFU_MediaTypeDef *DfuInterface = (USBD_DFU_MediaTypeDef *)pdev->pUserData[pdev->classId];
  USBD_DFUFuncDescTypeDef *pDfuFunc = (USBD_DFUFuncDescTypeDef *)USBD_DFU_GetDfuFuncDesc(pdev->pConfDesc);

  if ((hdfu == NULL) || (DfuInterface == NULL) || (pDfuFunc == NULL))
  {
    return;
  }

  switch (hdfu->dev_state)
  {
    case DFU_STATE_DNLOAD_SYNC:
      if (hdfu->wlength != 0U)
      {
        hdfu->dev_state = DFU_STATE_DNLOAD_BUSY;

        hdfu->dev_status[1] = 0U;
        hdfu->dev_status[2] = 0U;
        hdfu->dev_status[3] = 0U;
        hdfu->dev_status[4] = hdfu->dev_state;
      }
      else  /* (hdfu->wlength==0)*/
      {
        hdfu->dev_state = DFU_STATE_DNLOAD_IDLE;

        hdfu->dev_status[1] = 0U;
        hdfu->dev_status[2] = 0U;
        hdfu->dev_status[3] = 0U;
        hdfu->dev_status[4] = hdfu->dev_state;
      }
      break;

    case DFU_STATE_MANIFEST_SYNC:
      if (hdfu->manif_state == DFU_MANIFEST_IN_PROGRESS)
      {
        hdfu->dev_state = DFU_STATE_MANIFEST;

        hdfu->dev_status[1] = 1U;             /*bwPollTimeout = 1ms*/
        hdfu->dev_status[2] = 0U;
        hdfu->dev_status[3] = 0U;
        hdfu->dev_status[4] = hdfu->dev_state;
      }
      else
      {
        if ((hdfu->manif_state == DFU_MANIFEST_COMPLETE) &&
            ((pDfuFunc->bmAttributes & DFU_MANIFEST_MASK) != 0U))
        {
          hdfu->dev_state = DFU_STATE_IDLE;

          hdfu->dev_status[1] = 0U;
          hdfu->dev_status[2] = 0U;
          hdfu->dev_status[3] = 0U;
          hdfu->dev_status[4] = hdfu->dev_state;
        }
      }
      break;

    default:
      break;
  }

  /* Send the status data over EP0 */
  (void)USBD_CtlSendData(pdev, (uint8_t *)(&(hdfu->dev_status[0])), 6U);
}

/**
  * @brief  DFU_ClearStatus
  *         Handles the DFU CLRSTATUS request.
  * @param  pdev: device instance
  * @retval status
  */
static void DFU_ClearStatus(USBD_HandleTypeDef *pdev)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];

  if (hdfu == NULL)
  {
    return;
  }

  if (hdfu->dev_state == DFU_STATE_ERROR)
  {
    hdfu->dev_state = DFU_STATE_IDLE;
    hdfu->dev_status[0] = DFU_ERROR_NONE; /* bStatus */
    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U; /* bwPollTimeout=0ms */
    hdfu->dev_status[4] = hdfu->dev_state; /* bState */
    hdfu->dev_status[5] = 0U; /* iString */
  }
  else
  {
    /* State Error */
    hdfu->dev_state = DFU_STATE_ERROR;
    hdfu->dev_status[0] = DFU_ERROR_UNKNOWN; /* bStatus */
    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U; /* bwPollTimeout=0ms */
    hdfu->dev_status[4] = hdfu->dev_state; /* bState */
    hdfu->dev_status[5] = 0U; /* iString */
  }
}

/**
  * @brief  DFU_GetState
  *         Handles the DFU GETSTATE request.
  * @param  pdev: device instance
  * @retval None
  */
static void DFU_GetState(USBD_HandleTypeDef *pdev)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];

  if (hdfu == NULL)
  {
    return;
  }

  /* Return the current state of the DFU interface */
  (void)USBD_CtlSendData(pdev, &hdfu->dev_state, 1U);
}

/**
  * @brief  DFU_Abort
  *         Handles the DFU ABORT request.
  * @param  pdev: device instance
  * @retval None
  */
static void DFU_Abort(USBD_HandleTypeDef *pdev)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];

  if (hdfu == NULL)
  {
    return;
  }

  if ((hdfu->dev_state == DFU_STATE_IDLE) ||
      (hdfu->dev_state == DFU_STATE_DNLOAD_SYNC) ||
      (hdfu->dev_state == DFU_STATE_DNLOAD_IDLE) ||
      (hdfu->dev_state == DFU_STATE_MANIFEST_SYNC) ||
      (hdfu->dev_state == DFU_STATE_UPLOAD_IDLE))
  {
    hdfu->dev_state = DFU_STATE_IDLE;
    hdfu->dev_status[0] = DFU_ERROR_NONE;
    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U; /* bwPollTimeout=0ms */
    hdfu->dev_status[4] = hdfu->dev_state;
    hdfu->dev_status[5] = 0U; /* iString */
    hdfu->wblock_num = 0U;
    hdfu->wlength = 0U;
  }
}

/**
  * @brief  DFU_Leave
  *         Handles the sub-protocol DFU leave DFU mode request (leaves DFU mode
  *         and resets device to jump to user loaded code).
  * @param  pdev: device instance
  * @retval None
  */
static void DFU_Leave(USBD_HandleTypeDef *pdev)
{
  USBD_DFU_HandleTypeDef *hdfu = (USBD_DFU_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];
  USBD_DFUFuncDescTypeDef *pDfuFunc = (USBD_DFUFuncDescTypeDef *)USBD_DFU_GetDfuFuncDesc(pdev->pConfDesc);

  if ((hdfu == NULL) || (pDfuFunc == NULL))
  {
    return;
  }

  hdfu->manif_state = DFU_MANIFEST_COMPLETE;

  if ((pDfuFunc->bmAttributes & DFU_MANIFEST_MASK) != 0U)
  {
    hdfu->dev_state = DFU_STATE_MANIFEST_SYNC;

    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U;
    hdfu->dev_status[4] = hdfu->dev_state;
    return;
  }
  else
  {
    hdfu->dev_state = DFU_STATE_MANIFEST_WAIT_RESET;

    hdfu->dev_status[1] = 0U;
    hdfu->dev_status[2] = 0U;
    hdfu->dev_status[3] = 0U;
    hdfu->dev_status[4] = hdfu->dev_state;

    /* Disconnect the USB device */
    (void)USBD_Stop(pdev);

    /* The next instructions will not be reached (system reset) */
  }
}

/**
  * @brief  USBD_DFU_GetDfuFuncDesc
  *         This function return the DFU descriptor
  * @param  pdev: device instance
  * @param  pConfDesc:  pointer to Bos descriptor
  * @retval pointer to the DFU descriptor
  */
static void *USBD_DFU_GetDfuFuncDesc(uint8_t *pConfDesc)
{
  USBD_ConfigDescTypeDef *desc = (USBD_ConfigDescTypeDef *)(void *)pConfDesc;
  USBD_DescHeaderTypeDef *pdesc = (USBD_DescHeaderTypeDef *)(void *)pConfDesc;
  uint8_t *pDfuDesc = NULL;
  uint16_t ptr;

  if (desc->wTotalLength > desc->bLength)
  {
    ptr = desc->bLength;

    while (ptr < desc->wTotalLength)
    {
      pdesc = USBD_GetNextDesc((uint8_t *)pdesc, &ptr);

      if (pdesc->bDescriptorType == DFU_DESCRIPTOR_TYPE)
      {
        pDfuDesc = (uint8_t *)pdesc;
        break;
      }
    }
  }
  return pDfuDesc;
}

/**
  * @}
  */


/**
  * @}
  */


/**
  * @}
  */
