/**
  ******************************************************************************
  * @file    usbd_dfu_alt_conf.h
  * @brief   DFU alternate settings exposed by the OpenBootloader (USB transport).
  ******************************************************************************
  * Each DFU alternate setting is one "partition" the host (STM32CubeProgrammer)
  * selects with a SET_INTERFACE request, and is identified by its interface string:
  *
  *     @<name> /0x<partition id>/1*<size><unit>e
  *
  * STM32CubeProgrammer parses the "/0x<partition id>/" field of every alternate
  * setting and, for each flash layout (TSV) line, selects the alternate setting whose
  * partition id equals the TSV line "Id". More alternate settings therefore mean
  * more independently programmable partitions in a single session.
  *
  * The list below must keep exactly one entry for every alternate setting index
  * 0 .. USBD_DFU_MAX_ITF_NUM-1.
  *
  * The five first alternate settings are the ones of the ST original OpenBootloader:
  * keep their alternate setting indices and strings unchanged.
  *
  *   alt 0 : flashlayout        (TSV download, id 0x00)
  *   alt 1 : external loader    (id 0x03, written to RAM)
  *   alt 2 : first partition    (id 0x04)
  *   alt 3 : command / virtual  (id 0xF1)
  *   alt 4 : OTP                (id 0xF2)
  *
  * The next ones are additional partitions, each with its own partition id.
  *
  * Rules for the partition ids of a flash layout:
  *   - unique in the flash layout: the host looks the line up by id
  *   - 0x00, 0x03, 0xF1, 0xF2 are reserved (see above), 0xFE is "end of operation"
  *   - 0x01 is the id of the boot loader image line, which the OpenBootloader never
  *     programs itself (the boot ROM already loaded it), see README.md
  *   - at most 15 lines per flash layout
  *
  * The "size" field is only descriptive (shown by the host tool).
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USBD_DFU_ALT_CONF_H
#define __USBD_DFU_ALT_CONF_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Number of DFU alternate settings (interface descriptors), 1 .. 255 */
#define USBD_DFU_MAX_ITF_NUM            16U

/* Partition ids of the reserved alternate settings */
#define DFU_PART_ID_FLASHLAYOUT         0x00U
#define DFU_PART_ID_EXT_LOADER          0x03U
#define DFU_PART_ID_VIRTUAL             0xF1U
#define DFU_PART_ID_OTP                 0xF2U

/* Whole external NOR memory: the size is descriptive only and must never reject
   a valid image */
#define DFU_ALT_SIZE_NOR                128U, 'M'

/* Alternate setting table, in alternate-setting order */
#define USBD_DFU_ALT_TABLE(X)                                                   \
  /*  alt  name           id                          size    unit */          \
  X( 0U, "Flashlayout",  DFU_PART_ID_FLASHLAYOUT,   1U,    'K')                 \
  X( 1U, "FSBL-EXT",     DFU_PART_ID_EXT_LOADER,    32U,   'K')                 \
  X( 2U, "FSBL-APP",     0x04U,                    DFU_ALT_SIZE_NOR)             \
  X( 3U, "virtual",      DFU_PART_ID_VIRTUAL,       512U,  'B')                 \
  X( 4U, "OTP",          DFU_PART_ID_OTP,           752U,  'B')                 \
  X( 5U, "PART-05",      0x05U,                    DFU_ALT_SIZE_NOR)             \
  X( 6U, "PART-06",      0x06U,                    DFU_ALT_SIZE_NOR)             \
  X( 7U, "PART-07",      0x07U,                    DFU_ALT_SIZE_NOR)             \
  X( 8U, "PART-08",      0x08U,                    DFU_ALT_SIZE_NOR)             \
  X( 9U, "PART-09",      0x09U,                    DFU_ALT_SIZE_NOR)             \
  X(10U, "PART-0A",      0x0AU,                    DFU_ALT_SIZE_NOR)             \
  X(11U, "PART-0B",      0x0BU,                    DFU_ALT_SIZE_NOR)             \
  X(12U, "PART-0C",      0x0CU,                    DFU_ALT_SIZE_NOR)             \
  X(13U, "PART-0D",      0x0DU,                    DFU_ALT_SIZE_NOR)             \
  X(14U, "PART-0E",      0x0EU,                    DFU_ALT_SIZE_NOR)             \
  X(15U, "PART-0F",      0x0FU,                    DFU_ALT_SIZE_NOR)

/* Alternate setting index of the reserved settings and of the first user partition */
#define USBD_DFU_ALT_FLASHLAYOUT       0U
#define USBD_DFU_ALT_EXT_LOADER        1U
#define USBD_DFU_ALT_FIRST_PARTITION   2U
#define USBD_DFU_ALT_VIRTUAL           3U
#define USBD_DFU_ALT_OTP               4U

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __USBD_DFU_ALT_CONF_H */
