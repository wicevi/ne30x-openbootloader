## <b>OpenBootloader for the NE301</b>

This is the ST OpenBootloader project of STM32Cube_FW_N6_V1.3.0, in its own git repository
(https://github.com/wicevi/ne30x-openbootloader; the untouched ST project
is the first commit), changed so that **every partition of a flash layout is programmed in
one session**. The ST original programs one external memory
partition per download over USB.

It is used by the NE301 firmware project to download all its firmware over USB
(`make usb-flash` there, `Script/usb_flash.py`). The images it uses are shipped with that
project, in its `Script/usb_flash` directory: rebuild here (`make`), then copy
`build/ne301_OpenBootloader_signed.stm32` there. The description of the ST application
follows below.

### <b>Build</b>

```
make                    # GNU Arm Embedded toolchain, STM32_SigningTool_CLI (STM32CubeProgrammer)
```

`build/ne301_OpenBootloader.bin`, and `build/ne301_OpenBootloader_signed.stm32`: the boot
ROM image (header v2.3) STM32CubeProgrammer downloads and the boot ROM starts. The
HAL and CMSIS drivers are taken from the STM32Cube_FW_N6 package this project sits in;
from elsewhere, `make CUBE_FW=<package root>`.

The CubeIDE, IAR and Keil projects build the same sources.

### <b>Tests</b>

```
Tests/usb_module/test.sh                # arm-none-eabi-gcc, Python 3 with the "unicorn" package
Tests/usb_module/test.sh --reference    # the same T0, against the untouched ST sources
Tests/usb_module/test.sh --layout <dir> # also play a layout built by a host tool
```

The real `openbl_usb_cmd.c`, `openbl_util.c`, `usbd_dfu.c` and `usbd_ctlreq.c` are
cross-compiled and executed in a CPU emulator (Unicorn, Cortex-M33: it has no M55 model
and the code is plain integer C), against a model of a NOR flash. The host side of the
tests takes the lines as the device announces them, without presuming which line comes:
the reference run (`--reference`) proves that what it expects is what the ST original
does, the one STM32CubeProgrammer was validated with. Not covered: the USB peripheral
driver, the boot ROM, the external memory loader (a precompiled ST binary) and the real
flash timings.

### <b>A download, as STM32CubeProgrammer runs it</b>

The host asks for the next item on the virtual alternate setting (id `0xF1`), twice per
item: once to download it ("phase"), once to start it:

```
0x00  phase  -> the host downloads the flash layout text to alternate setting 0
0x00  start
id    phase  -> the host downloads the binary of the line carrying that id,
                to the alternate setting of the id, at the address announced with it
id    start      ... for every line
0xFE         end of operation
```

Line 0 is the boot loader image in the layouts of ST: the boot ROM loaded and started
it, and it has no alternate setting, so the OpenBootloader never announces it (a layout
whose line 0 is something else is walked from line 0).

### <b>Changes to the ST original</b>

- **USB transport** (`Middlewares/ST/OpenBootloader/Modules/USB/openbl_usb_cmd.c`)
  - a flash layout line is programmed at the offset of its own line, whatever its id
  - the destination comes from the "ip" column: `none` -> RAM (the loader), `nor<n>` ->
    external memory; an offset that leaves the memory is refused
  - erase tracking per line: in the ST original a line at a lower address than the
    previous one is not erased (its sector counter only increases)
  - every 64 KB sector is erased for its own, aligned range: a line that ends exactly on
    a sector boundary never erases the first sector of what follows it, and a packet that
    straddles a boundary finds both sectors erased
  - the loader is written from the offset reached so far, not from a block number times
    a fixed packet length
  - the walk of the lines restarts after the end of operation: several downloads can be
    run without resetting the OpenBootloader; the layout of the session before is dropped
    when the flash layout is announced again, so a refused download never walks it
  - data sent on another alternate setting than the announced partition is refused; a
    flash layout line with the id of the flash layout or of the OTP is refused too (the
    ST original would take its binary for a flash layout, or program the OTP with it)
  - init, write and verify failures are reported to the host (dfuERROR)
- **Flash layout parser** (`Util/openbl_util.c`): the line counter is no longer static;
  comment lines are skipped (a '#' not at the very first character of the text made the
  parser loop forever); CR, empty lines, a last line without line end and more than six
  columns are handled; a layout without a line, a malformed line and a line with a
  missing column are errors (the ST original accepts an empty layout, ignores a malformed
  last line and takes the column of the line before for one that is missing); a rejected
  layout leaves nothing usable behind
- **DFU class** (`STM32_USB_Device_Library/Class/DFU`): the alternate settings come from
  a table, `usbd_dfu_alt_conf.h`; the configuration descriptor and the interface strings
  are built from it at run time; a failed write puts the device in the DFU error state
- The middleware is copied into the project (`Middlewares/`), and the IDE projects build
  on that copy

### <b>Alternate settings</b>

STM32CubeProgrammer finds the alternate setting of a flash layout line by the partition
id in its interface string, `@<name> /0x<id>/1*<size><unit>e`. The table
`usbd_dfu_alt_conf.h` declares 16 alternate settings; their indices and strings of the
five of the ST original are unchanged:

| Alternate setting | Partition id | Use                                        |
|-------------------|--------------|--------------------------------------------|
| 0                 | 0x00         | the flash layout itself                     |
| 1                 | 0x03         | the external memory loader (to RAM)         |
| 2                 | 0x04         | first user partition                        |
| 3                 | 0xF1         | command / virtual                           |
| 4                 | 0xF2         | OTP                                         |
| 5 .. 15           | 0x05 .. 0x0F | other user partitions                       |

Rules for a layout: 15 lines at most, one partition id per line, user partition ids in
`0x04`..`0x0F`; line 0 is normally the boot loader image, loaded by the boot ROM and
never programmed by the OpenBootloader. Its text must fit in the 768 bytes behind the
256 bytes reserved for a signature: STM32CubeProgrammer hands it over in one packet.

## <b>OpenBootloader Application Description</b>

This application exploits OpenBootloader Middleware to demonstrate how to develop an application
that can be used to program OTP words or external memories.

This application runs from RAM memory.

At the beginning of the main program, the HAL_Init() function is called to reset
all the peripherals, initialize the Flash interface and the systick.
The SystemClock_Config() function is used to configure the system clock (SYSCLK).

Then, these protocols and memories interfaces are configured:

  - USART1
  - USB1 HS
  - RAM interface
  - OTP interface

Once the interfaces are configured, the OpenBootloader application will wait for incoming communication on one of the supported protocols.

If a communication is established through one of the supported protocols,
the OpenBootloader application will wait for commands sent by the host.

### <b>Keywords</b>

OpenBootloader, USART, USB, OTP

### <b>Directory contents</b>

     - Core/Src/main.c                                     Main program file
     - Core/Src/stm32n6xx_hal_msp.c                        MSP Initialization file
     - Core/Src/stm32n6xx_it.c                             Interrupt handlers file
     - Core/Src/system_stm32n6xx_fsbl.c                    STM32N6xx system clock configuration file
     - Core/Inc/main.h                                     Main program header file
     - Core/Inc/stm32n6xx_hal_conf.h                       HAL Library Configuration file
     - Core/Inc/stm32n6xx_it.h                             Interrupt handlers header file
     - OpenBootloader/App/app_openbootloader.c             OpenBootloader application entry point
     - OpenBootloader/App/app_openbootloader.h             Header for OpenBootloader application entry file
     - OpenBootloader/Target/common_interface.c            Contains common functions used by different interfaces
     - OpenBootloader/Target/common_interface.h            Header for common functions file
     - OpenBootloader/Target/external_memory_interface.h   Contains support for external memories
     - OpenBootloader/Target/external_memory_interface.h   Header for external memories functions file
     - OpenBootloader/Target/interfaces_conf.h             Contains Interfaces configuration
     - OpenBootloader/Target/iwdg_interface.c              Contains IWDG interface
     - OpenBootloader/Target/iwdg_interface.h              Header of IWDG interface file
     - OpenBootloader/Target/openbootloader_conf.h         Header file that contains OpenBootloader HW dependent configuration
     - OpenBootloader/Target/otp_interface.c               Contains OTP interface
     - OpenBootloader/Target/otp_interface.h               Header of OTP interface file
     - OpenBootloader/Target/platform.h                    Header for patterns
     - OpenBootloader/Target/ram_interface.c               Contains RAM interface
     - OpenBootloader/Target/ram_interface.h               Header of RAM interface file
     - OpenBootloader/Target/usart_interface.c             Contains USART interface
     - OpenBootloader/Target/usart_interface.h             Header of USART interface file
     - OpenBootloader/Target/usb_interface.c               Contains USB interface
     - OpenBootloader/Target/usb_interface.h               Header of USB interface file
     - USB_Device/App/usb_device.c                         USB Device applicative file
     - USB_Device/App/usb_device.h                         USB Device applicative header file
     - USB_Device/App/usbd_desc.c                          USB Device descriptor file
     - USB_Device/App/usbd_desc.h                          USB Device descriptor header file
     - USB_Device/App/usbd_dfu_if.c                        USB Device interface applicative file
     - USB_Device/App/usbd_dfu_if.h                        USB Device interface applicative header file
     - USB_Device/Target/usbd_conf.c                       Contains user defines for USB configuration
     - USB_Device/Target/usbd_conf.h                       USB configuration header file

### <b>Hardware and Software environment</b>

  - This application runs on STM32N6 devices.

  - This application has been tested with STMicroelectronics STM32N6570-DK (MB1939-N6570-A03A)
    board and can be easily tailored to any other supported device and development board.

  - STM32N6570-DK set-up to use USART:
    - To use the USART1 for communication you have to connect:
      - Tx pin of your host adapter to PE5
      - Rx pin of your host adapter to PE6
    - These pins are already connected to the board embedded ST-Link

  - STM32N6570-DK set-up to use USB:
    - USB1 CN18

**WARNING**

When OTP fuses are set, they can not be reset.

### <b>How to use it?</b>

The example can be run either in development mode or in Load and Run mode.

#### <b>Development mode</b>

In order to make the program work, you must do the following :

  - Set the boot mode in development mode (BOOT1 switch position is 1-3, BOOT0 switch position doesn't matter).
  - Open your preferred toolchain
  - Rebuild all files and load your image into target memory. Code can be executed in this mode for debugging purposes.
  - Run the application
  - Run STM32CubeProgrammer and connect to OpenBootloader using USART1</br>
    Or
  - Connect your USB host adapter and connect to OpenBootloader using USB HS</br>

#### <b>Load and Run mode</b>

This mode enables execution without having to connect through an IDE. The application will be stored in RAM memory and therefore will not require any external tools once loaded onto the board.

 - Resort to STM32CubeProgrammer to add a header to a binary OpenBootloader_STM32N6570-DK.bin with the following command
   - *STM32_SigningTool_CLI.exe -bin OpenBootloader_STM32N6570-DK.bin -nk -of 0x80000000 -t fsbl -o OpenBootloader_STM32N6570-DK-trusted.bin -hv 2.3 -dump OpenBootloader_STM32N6570-trusted.bin*
   - The resulting binary is OpenBootloader_STM32N6570-DK-trusted.bin.
 - Create a file called FlashLayout.tsv and write this content in it
   - #Opt	Id	Name	Type	IP	Offset	Binary
   - P	0x1	fsbl-openbl	Binary	none	0x0	OpenBootloader_STM32N6570-DK-trusted.stm32

#### <b>How to program external memories?</b>

Open Bootloader application supports external memories programming (init, write...) operations using `STM32CubeProgrammer` and `External Loader`.

As Open Bootloader application relies on the External Loader to access the external memories, there is no need to recompile Open Bootloader application to support different types of external memories.

The External Loader provides some functions like:

- Init
- Write
- SectorErase
- MassErase
- CheckSum
- Verify

When the External Loader is programmed in RAM by STM32CubeProgrammer through TSV file, pointers to the above functions (init, write...) are written in a table in RAM.

In the current version of the external flash loader, the address of this table is at 0x38000000.
The Open Bootloader uses that address to retrieve the functions pointers to call the External Loader init, write... functions when needed.
For more details about how this is implemented, the user can check the file `external_memory_interface.c`.

Then STM32CubeProgrammer can send commands to Open Bootloader to access the external memories.

A precompiled version of Open Bootloader application that runs in RAM is provided in the folder `STM32N6570-DK\Applications\OpenBootloader\Binaries\NOR_Binary`.
To be able to program the external memory using one of the supported protocols (USART or USB), a TSV file can be used, an example of a TSV file can be found in the folder `STM32N6570-DK\Applications\OpenBootloader\Binaries\NOR_Binary`.

Here is an example of CLI commands that can be used to write in an external memory:

        STM32_Programmer_CLI.exe -c port=com110 -d FlashLayout.tsv
        STM32_Programmer_CLI.exe -c port=comUSB1 -d FlashLayout.tsv

To support a different external memory than the one available in the STM32N6570-DK board, the user can compile and use his own External Loader.
There is no need to recompile Open Bootloader application unless the user needs to tailor it to its need or change the address of the flash loader function pointers table.

Depending on whether the user compiled his own External Loader, or he is using the precompiled External Loader present in `STM32N6570-DK\Applications\OpenBootloader\Binaries\NOR_Binary directory`,
there are two ways to extract the address of the External Loader functions (init, write...) and write them in RAM:

1. User has compiled is own External Loader

When compiling his own External Loader, the user can modify its code to extract the address of needed functions (init, write...) and write them in RAM.

Here is an example of code that will place the functions addresses in RAM using IAR IDE:

        /* Define a structure with functions pointers */
        typedef struct
        {
          int      (*Init)        (void);
          int      (*Write)        (uint32_t Address, uint32_t Size, uint8_t* buffer);
          int      (*SectorErase)  (uint32_t EraseStartAddress ,uint32_t EraseEndAddress);
          int      (*MassErase)    (uint32_t Parallelism );
          uint32_t (*CheckSum)     (uint32_t StartAddress, uint32_t Size, uint32_t InitVal);
          uint64_t (*Verify)       (uint32_t MemoryAddr, uint32_t RAMBufferAddr, uint32_t Size, uint32_t missalignement);
        } FunctionTable;

        /* Declare the function table */
        #pragma location = "MY_FUNCTIONS"

        KeepInCompilation const FunctionTable myFunctionTable =
        {
          .Init        = Init,
          .Write       = Write,
          .SectorErase = SectorErase,
          .MassErase   = MassErase,
          .CheckSum    = CheckSum,
          .Verify      = Verify
        };

Then in the `icf` file of the IAR project this line can be added to ensure the placement of the structure in the needed address:

        place at address mem: 0x38000000  { readwrite section MY_FUNCTIONS };

If a different IDE is used, the user can tailor this line and update it to meet the IDE language needs.

If the address of the external flash loader functions pointers changes, the user must update it in the `external_memory_interface.c` file by changing the address in this line:

        #define FLASH_LOADER_FUNCTIONS_POINTERS           0x38000000U

2. User is using precompiled External Loader from `STM32N6570-DK\Applications\OpenBootloader\Binaries\NOR_Binary directory`

By default, the address of the needed external flash loader functions pointers is at address 0x38000000.

The Open Bootloader expects to find in RAM the address of the functions in this order (extract from `external_memory_interface.c`):

       #define INIT_INDEX               0x00U
       #define WRITE_INDEX              0x01U
       #define SECTOR_ERASE_INDEX       0x02U
       #define MASS_ERASE_INDEX         0x03U
       #define VERIFY_INDEX             0x05U
       #define READ_INDEX               0x06U

       static uint32_t OPENBL_ExtMem_IsAddressInRam(uint32_t);
       static uint32_t (*ExtLoader_Init)(void);
       static uint32_t (*ExtLoader_Write)(uint32_t Address, uint32_t DataLength, uint8_t *pData);
       static uint32_t (*ExtLoader_Erase)(uint32_t StartAddress, uint32_t EndAddress);
       static uint32_t (*ExtLoader_MassErase)(void);
       static uint64_t (*ExtLoader_Verify)(uint32_t Address, uint32_t DataAddr, uint32_t DataLength, uint32_t CrcInit);
       static void (*ExtLoader_Read)(uint32_t Address, uint32_t DataLength, uint8_t *pData);

The initialization of the functions pointers is done in the `OPENBL_ExtMem_Init` function:

       /* Functions pointers initialization */
       ExtLoader_Init      = (uint32_t (*)(void))(*(FlashLoaderFunctionsAdd + INIT_INDEX));
       ExtLoader_Write     = (uint32_t (*)(uint32_t, uint32_t, uint8_t *))(*(FlashLoaderFunctionsAdd + WRITE_INDEX));
       ExtLoader_Erase     = (uint32_t (*)(uint32_t, uint32_t))(*(FlashLoaderFunctionsAdd + SECTOR_ERASE_INDEX));
       ExtLoader_MassErase = (uint32_t (*)(void))(*(FlashLoaderFunctionsAdd + MASS_ERASE_INDEX));
       ExtLoader_Verify    = (uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))(*(FlashLoaderFunctionsAdd + VERIFY_INDEX));
       ExtLoader_Read      = (void (*)(uint32_t, uint32_t, uint8_t *))(*(FlashLoaderFunctionsAdd + READ_INDEX));

### <b>How to use precompiled binary to program NOR memory (MX66UW1G45G)?</b>

To be able to program the NOR memory (MX66UW1G45G) present in the STM32N6570-DK board, these steps must be followed:

  - Set the boot mode to boot on Boot Rom (BOOT1 switch position is 1-2, BOOT0 switch position doesn't matter).
  - Open the folder *NOR_Binary* that is inside *Binaries* directory.
  - Open the file *FlashLayout.tsv* in STM32CubeProgrammer.
  - Connect to USART or USB.
  - Then press *Download* button.

To load a different binary file, place the binary inside the directory *Binaries\\NOR_Binary* and edit the *FlashLayout.tsv* file to change the file name from *RamdomData.bin* to your file name.


**Warning** If using CubeProgrammer v2.21 version or more recent, add *-align* option in the command line.
