/**
  ******************************************************************************
  * @file    openbl_util.c
  * @author  MCD Application Team
  * @brief   Open Bootloader util file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "openbl_util.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* Exported functions --------------------------------------------------------*/
/* Exported variables --------------------------------------------------------*/
OPENBL_Flashlayout_TypeDef FlashlayoutStruct;

/**
  * @brief  This function is used to parse the flash layout options.
  * @param  address Address from where flash layout will be parsed.
  * @param  size Size of the flash layout to be parsed.
  * @retval Status PARSE_OK if success otherwise PARSE_ERROR.
  */
int32_t OPENBL_FlashLayout_Parse_Layout(uint32_t address, uint32_t size)
{
  /* STM32CubeProgrammer sends the flash layout info with TAB (ASCII 0x9)
   * between columns and Carriage Return ('\n') between each line.
   * Each line contains 6 columns: opt, id, name, type, ip and offset.
   *
   * Lines starting with '#' (after the line start) are comments and are skipped.
   * The layout can be parsed several times (a new flash layout download): the line
   * count restarts from zero and the strings of the previous parse are freed.
   * A layout without a line, or with a malformed one, is an error.
   */
  uint32_t partition_list_size = 0U;

  char *start;
  char *last;
  char *p;
  char opt[33];
  char id[33];
  char name[33];
  char type[33];
  char ip[33];
  char offset[33];
  char *col[6];
  char c;
  uint32_t i         = 0U;
  uint32_t tab_count = 0U;
  bool is_partition  = true;   /* false while the characters of a comment line are skipped */
  bool at_line_start = true;

  col[0] = opt;
  col[1] = id;
  col[2] = name;
  col[3] = type;
  col[4] = ip;
  col[5] = offset;

  start = (char *)address;
  last  = start + size;
  *last = '\0'; /* Force null terminated string */

  /* Free the strings of the previous parse (replaced below) */
  OPENBL_FlashLayout_Free();

  /* Reset the flash layout: on parse error no stale line remains usable */
  (void)memset(&FlashlayoutStruct, 0, sizeof(FlashlayoutStruct));

  p = start;

  while (p < last)
  {
    c = *p;

    /* A '#' as the first character of a line starts a comment line */
    if ((c == '#') && (is_partition != false) && (at_line_start != false))
    {
      is_partition = false;
    }

    if (c == '\n')
    {
      /* End of line: store the line unless it is a comment or an empty line */
      if ((is_partition != false) && (tab_count != 0U))
      {
        if (partition_list_size >= PHASE_LAST_USER)
        {
          return PARSE_ERROR;
        }

        if (OPENBL_Flashlayout_Parse_Option(opt, partition_list_size) == PARSE_ERROR)
        {
          return PARSE_ERROR;
        }
        if (OPENBL_Flashlayout_Parse_ID(id, partition_list_size) == PARSE_ERROR)
        {
          return PARSE_ERROR;
        }
        if (OPENBL_Flashlayout_Parse_Name(name, partition_list_size) == PARSE_ERROR)
        {
          return PARSE_ERROR;
        }
        if (OPENBL_Flashlayout_Parse_Type(type, partition_list_size) == PARSE_ERROR)
        {
          return PARSE_ERROR;
        }
        if (OPENBL_Flashlayout_Parse_IP(ip, partition_list_size) == PARSE_ERROR)
        {
          return PARSE_ERROR;
        }
        if (OPENBL_Flashlayout_Parse_Offset(offset, partition_list_size) == PARSE_ERROR)
        {
          return PARSE_ERROR;
        }

        partition_list_size++;
      }

      /* Every column of the next line starts empty: a missing one is an error, never the
         column of the line before left in the buffer */
      for (i = 0U; i < 6U; i++)
      {
        col[i][0] = '\0';
      }

      is_partition   = true;
      at_line_start  = true;
      tab_count      = 0U;
      i              = 0U;
    }
    else if (c == '\r')
    {
      /* Ignored (CRLF line ends) */
    }
    else if ((is_partition != false) && (c == 0x9U))
    {
      /* Column separator */
      tab_count++;
      i = 0U;
    }
    else if (is_partition != false)
    {
      /* Column content: the first six columns are used (the binary name is not) */
      if ((tab_count <= 5U) && (i < 32U))
      {
        col[tab_count][i++] = c;
        col[tab_count][i] = '\0';
      }
    }
    /* else: content of a comment line, skipped */

    at_line_start = false;
    p++;
  }

  /* A last line without line end */
  if ((is_partition != false) && (tab_count != 0U))
  {
    if ((partition_list_size >= PHASE_LAST_USER)
        || (OPENBL_Flashlayout_Parse_Option(opt, partition_list_size) == PARSE_ERROR)
        || (OPENBL_Flashlayout_Parse_ID(id, partition_list_size) == PARSE_ERROR)
        || (OPENBL_Flashlayout_Parse_Name(name, partition_list_size) == PARSE_ERROR)
        || (OPENBL_Flashlayout_Parse_Type(type, partition_list_size) == PARSE_ERROR)
        || (OPENBL_Flashlayout_Parse_IP(ip, partition_list_size) == PARSE_ERROR)
        || (OPENBL_Flashlayout_Parse_Offset(offset, partition_list_size) == PARSE_ERROR))
    {
      return PARSE_ERROR;
    }

    partition_list_size++;
  }

  /* Nothing could be programmed from a layout without a line */
  if (partition_list_size == 0U)
  {
    return PARSE_ERROR;
  }

  FlashlayoutStruct.partsize = partition_list_size;

  return PARSE_OK;
}

/**
  * @brief  This function frees the strings of the current flash layout.
  * @retval None.
  */
void OPENBL_FlashLayout_Free(void)
{
  uint32_t idx;

  for (idx = 0U; idx < PHASE_LAST_USER; idx++)
  {
    free(FlashlayoutStruct.opt[idx]);
    free(FlashlayoutStruct.name[idx]);
    free(FlashlayoutStruct.type[idx]);
    free(FlashlayoutStruct.ip[idx]);
    FlashlayoutStruct.opt[idx]  = NULL;
    FlashlayoutStruct.name[idx] = NULL;
    FlashlayoutStruct.type[idx] = NULL;
    FlashlayoutStruct.ip[idx]  = NULL;
  }
}

/**
  * @brief  This function is used to parse the flash layout ID.
  * @param  p_string_id Pointer to the string that contains the flash layout ID.
  * @param  idx Index of the flash layout in the flash layout structure.
  * @retval Returns PARSE_OK in case of success or returns PARSE_ERROR.
  */
int32_t OPENBL_Flashlayout_Parse_ID(char *p_string_id, uint32_t idx)
{
  uint32_t id;
  errno         = 0;
  char *end_ptr = 0;

  id = strtoul(p_string_id, &end_ptr, 0);

  /* Check if string to number conversion is OK */
  if (end_ptr != p_string_id)
  {
    /* If the number is not valid */
    if ((id == 0U || id == ULONG_MAX) && (errno == ERANGE))
    {
      return PARSE_ERROR;
    }
    else
    {
      FlashlayoutStruct.id[idx] = id;

      return PARSE_OK;
    }
  }
  else
  {
    return PARSE_ERROR;
  }
}

/**
  * @brief  This function is used to parse the flash layout offset.
  * @param  p_string_offset String containing the offset of flash layout.
  * @param  idx Index of the flash layout in the flash layout structure.
  * @retval Returns PARSE_OK in case of success or returns PARSE_ERROR.
  */
int32_t OPENBL_Flashlayout_Parse_Offset(char *p_string_offset, uint32_t idx)
{
  uint32_t offset;
  char *end_ptr = 0;
  errno         = 0;

  offset = strtoul(p_string_offset, &end_ptr, 0);

  /* Check if string to number conversion is OK */
  if (end_ptr != p_string_offset)
  {
    /* If the number is not valid */
    if ((offset == 0 || offset == ULONG_MAX) && (errno == ERANGE))
    {
      return PARSE_ERROR;
    }
    else
    {
      FlashlayoutStruct.offset[idx] = offset;

      return PARSE_OK;
    }
  }
  else
  {
    return PARSE_ERROR;
  }
}

/**
  * @brief  This function is used to parse the flash layout name.
  * @param  p_string_name String containing the name of flash layout.
  * @param  idx Index of the flash layout in the flash layout structure.
  * @retval Always returns PARSE_OK.
  */
int32_t OPENBL_Flashlayout_Parse_Name(char *p_string_name, uint32_t idx)
{
  size_t size = strlen(p_string_name) + 1;

  FlashlayoutStruct.name[idx] = (char *) malloc(size);

  strcpy(FlashlayoutStruct.name[idx], p_string_name);

  return PARSE_OK;
}

/**
  * @brief  This function is used to parse the flash layout type.
  * @param  p_string_type String containing the type of flash layout.
  * @param  idx Index of the flash layout in the flash layout structure.
  * @retval Always returns PARSE_OK.
  */
int32_t OPENBL_Flashlayout_Parse_Type(char *p_string_type, uint32_t idx)
{
  size_t size = strlen(p_string_type) + 1;

  FlashlayoutStruct.type[idx] = (char *) malloc(size);

  strcpy(FlashlayoutStruct.type[idx], p_string_type);

  return PARSE_OK;
}

/**
  * @brief  This function is used to parse the flash layout ip.
  * @param  p_string_ip String containing the flash layout ip.
  * @param  idx Index of the flash layout in the flash layout structure.
  * @retval Always returns PARSE_OK.
  */
int32_t OPENBL_Flashlayout_Parse_IP(char *p_string_ip, uint32_t idx)
{
  size_t size = strlen(p_string_ip) + 1;

  FlashlayoutStruct.ip[idx] = (char *) malloc(size);

  strcpy(FlashlayoutStruct.ip[idx], p_string_ip);

  return PARSE_OK;
}

/**
  * @brief  This function is used to parse the flash layout option.
  * @param  p_string_option String containing the flash layout option.
  * @param  idx Index of the flash layout in the flash layout structure.
  * @retval Always returns PARSE_OK.
  */
int32_t OPENBL_Flashlayout_Parse_Option(char *p_string_option, uint32_t idx)
{
  size_t size = strlen(p_string_option) + 1;

  FlashlayoutStruct.opt[idx] = (char *) malloc(size);

  strcpy(FlashlayoutStruct.opt[idx], p_string_option);

  return PARSE_OK;
}
