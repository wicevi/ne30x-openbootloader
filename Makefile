##########################################################################################################################
# OpenBootloader Makefile (GCC / arm-none-eabi)
#
# Builds the RAM-resident STM32 OpenBootloader and wraps it into a boot ROM (header v2.3)
# image that STM32CubeProgrammer downloads through the boot ROM USB DFU, see README.md.
#
# The project only depends on files of this directory except for the HAL / CMSIS drivers,
# which are taken from the STM32Cube_FW_N6 package the project originates from (CUBE_FW).
##########################################################################################################################

##############################################################################
# Target / paths
##############################################################################
PROJECT  = OpenBootloader
TARGET   = ne301_OpenBootloader
BUILD_DIR = build

# STM32Cube_FW_N6 package root (this project lives in <root>/Projects/STM32N6570-DK/Applications/OpenBootloader)
CUBE_FW ?= $(abspath ../../../..)
DRIVERS  = $(CUBE_FW)/Drivers

# OpenBootloader + USB device library, vendored from <CUBE_FW>/Middlewares/ST/OpenBootloader
OBL_MW   = Middlewares/ST/OpenBootloader

# GNU Arm Embedded toolchain prefix; add GCC_PATH if the tools are not in PATH
PREFIX  = arm-none-eabi-
ifdef GCC_PATH
CC  = $(GCC_PATH)/$(PREFIX)gcc
AS  = $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
CP  = $(GCC_PATH)/$(PREFIX)objcopy
SZ  = $(GCC_PATH)/$(PREFIX)size
else
CC  = $(PREFIX)gcc
AS  = $(PREFIX)gcc -x assembler-with-cpp
CP  = $(PREFIX)objcopy
SZ  = $(PREFIX)size
endif
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

# Adds the STM32 boot ROM header; ships with STM32CubeProgrammer
SIGNER  ?= STM32_SigningTool_CLI
SIGN_FLAGS ?=

##############################################################################
# Configuration
# Same settings as the STM32CubeIDE project (Release configuration)
##############################################################################
# Optimisation: -O0/-O1/-Og/-Os/-O2/-O3 (code region is 100K)
OPT = -Os

# Debug info: default is -g3; set DEBUG=0 to strip
DEBUG = 1
ifeq ($(DEBUG), 1)
OPT += -g3
else
OPT += -g0
endif

##############################################################################
# Sources
##############################################################################
C_SOURCES = \
  Core/Src/main.c \
  Core/Src/stm32n6xx_hal_msp.c \
  Core/Src/stm32n6xx_it.c \
  Core/Src/system_stm32n6xx_fsbl.c \
  STM32CubeIDE/Application/Core/syscalls.c \
  STM32CubeIDE/Application/Core/sysmem.c \
  OpenBootloader/App/app_openbootloader.c \
  OpenBootloader/Target/common_interface.c \
  OpenBootloader/Target/external_memory_interface.c \
  OpenBootloader/Target/iwdg_interface.c \
  OpenBootloader/Target/otp_interface.c \
  OpenBootloader/Target/ram_interface.c \
  OpenBootloader/Target/usart_interface.c \
  OpenBootloader/Target/usb_interface.c \
  USB_Device/App/usb_device.c \
  USB_Device/App/usbd_desc.c \
  USB_Device/App/usbd_dfu_if.c \
  USB_Device/Target/usbd_conf.c \
  $(OBL_MW)/Core/openbl_core.c \
  $(OBL_MW)/Util/openbl_util.c \
  $(OBL_MW)/Modules/Mem/openbl_mem.c \
  $(OBL_MW)/Modules/USART/openbl_usart_cmd.c \
  $(OBL_MW)/Modules/USB/openbl_usb_cmd.c \
  $(OBL_MW)/STM32_USB_Device_Library/Core/Src/usbd_core.c \
  $(OBL_MW)/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c \
  $(OBL_MW)/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c \
  $(OBL_MW)/STM32_USB_Device_Library/Class/DFU/Src/usbd_dfu.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_bsec.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_cortex.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_gpio.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_hcd.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_iwdg.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_pcd.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_pcd_ex.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_pwr.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_pwr_ex.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_rcc.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_rcc_ex.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_usart.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_usart_ex.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_ll_gpio.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_ll_rcc.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_ll_usart.c \
  $(DRIVERS)/STM32N6xx_HAL_Driver/Src/stm32n6xx_ll_usb.c

ASM_SOURCES = STM32CubeIDE/Application/Startup/startup_stm32n657x0hxq.s

##############################################################################
# MCU / flags
##############################################################################
CPU = -mcpu=cortex-m55
FPU = -mfpu=fpv5-d16
FLOAT-ABI = -mfloat-abi=hard
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# TrustZone secure build (STM32CubeIDE project property "Secure"): exposes the
# <PERIPH>_S aliases and the secure-only clock macros used throughout the project
TZ = -mcmse

C_DEFS = \
  -DUSE_HAL_DRIVER \
  -DUSER_VECT_TAB_ADDRESS \
  -DUSE_FULL_LL_DRIVER \
  -DSTM32N657xx \
  -DUSE_USB_HS

AS_DEFS =

C_INCLUDES = \
  -ICore/Inc \
  -IOpenBootloader/App \
  -IOpenBootloader/Target \
  -IUSB_Device/App \
  -IUSB_Device/Target \
  -I$(OBL_MW)/Core \
  -I$(OBL_MW)/Util \
  -I$(OBL_MW)/Modules/Mem \
  -I$(OBL_MW)/Modules/USART \
  -I$(OBL_MW)/Modules/USB \
  -I$(OBL_MW)/STM32_USB_Device_Library/Class/DFU/Inc \
  -I$(OBL_MW)/STM32_USB_Device_Library/Core/Inc \
  -I$(DRIVERS)/CMSIS/Core/Include \
  -I$(DRIVERS)/STM32N6xx_HAL_Driver/Inc \
  -I$(DRIVERS)/CMSIS/Device/ST/STM32N6xx/Include \
  -I$(DRIVERS)/CMSIS/Include

CFLAGS  = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -fstack-usage
CFLAGS += -std=gnu11 --specs=nano.specs -MMD -MP -MF"$(@:%.o=%.d)" $(TZ)

ASFLAGS = $(MCU) $(AS_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -g3 $(TZ)

# GNU Arm Embedded presets
LIBDIR =
LIBS = -lc -lm -lnosys
LDFLAGS = $(MCU) --specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS)
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections -static $(TZ)
LDFLAGS += -Wl,--start-group -lc -lm -Wl,--end-group -Wl,--print-memory-usage

# The linker script (code 0x34180400 100K / data 0x34199400 40K)
LDSCRIPT = STM32CubeIDE/STM32N657X0HXQ_AXISRAM2_fsbl.ld

##############################################################################
# Objects / rules
##############################################################################
OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

# Default action: build the elf, the raw binary and the boot ROM image
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET)_signed.stm32

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(BIN) $< $@

# Header v2.3 FSBL image, with the command documented in the ST README.md of this
# project: the boot ROM downloads it to AXISRAM2 (0x34180000) and enters the payload at 0x34180400.
$(BUILD_DIR)/$(TARGET)_signed.stm32: $(BUILD_DIR)/$(TARGET).bin
	$(SIGNER) -s -bin $< -nk -of 0x80000000 -t fsbl -hv 2.3 -o $@ $(SIGN_FLAGS)

$(BUILD_DIR):
	mkdir -p $@

##############################################################################
# Additional targets
##############################################################################
clean:
	-rm -fR $(BUILD_DIR)

# Flashlayout / images the bootloader is driven with are generated by the host tool
# (NE301: Script/usb_flash.py), which copies the resulting *_signed.stm32 next to them.

.PHONY: all clean
