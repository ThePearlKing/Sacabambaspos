/* efi.h - minimal freestanding UEFI definitions for SacabambaspOS.
 * Only the subset the boot app needs. x86_64 UEFI uses the MS ABI, so every
 * firmware-called function pointer is marked EFIAPI (ms_abi). */
#ifndef SBMP_EFI_H
#define SBMP_EFI_H

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed long long    INT64;
#if defined(__x86_64__)
typedef unsigned long long  UINTN;   /* natural width: 64-bit on x86_64 */
#define EFIAPI __attribute__((ms_abi))   /* x86-64 UEFI uses the MS ABI */
#else
typedef unsigned int        UINTN;   /* 32-bit on ia32 */
#define EFIAPI                            /* ia32 UEFI uses plain cdecl */
#endif
typedef UINT8               BOOLEAN;
typedef UINT16              CHAR16;
typedef void                VOID;
typedef UINTN               EFI_STATUS;
typedef VOID               *EFI_HANDLE;
typedef VOID               *EFI_EVENT;
#define IN
#define OUT
#define OPTIONAL
#define NULL ((void*)0)

#define EFI_SUCCESS            0
#define EFIERR(a)             ((EFI_STATUS)(((UINTN)1 << (sizeof(UINTN)*8-1)) | (a)))
#define EFI_LOAD_ERROR        EFIERR(1)
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED       EFIERR(3)
#define EFI_BUFFER_TOO_SMALL  EFIERR(5)
#define EFI_NOT_FOUND         EFIERR(14)

typedef struct { UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8]; } EFI_GUID;

typedef struct {
  UINT64 Signature; UINT32 Revision; UINT32 HeaderSize;
  UINT32 CRC32; UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Simple Text Output ---- */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, BOOLEAN);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, CHAR16*);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*);
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  EFI_TEXT_RESET  Reset;
  EFI_TEXT_STRING OutputString;
  VOID *TestString, *QueryMode, *SetMode;
  EFI_TEXT_SET_ATTRIBUTE SetAttribute;
  EFI_TEXT_CLEAR_SCREEN  ClearScreen;
  VOID *SetCursorPosition, *EnableCursor, *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- Simple Text Input ---- */
typedef struct { UINT16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, BOOLEAN);
typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, EFI_INPUT_KEY*);
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
  EFI_INPUT_RESET    Reset;
  EFI_INPUT_READ_KEY ReadKeyStroke;
  EFI_EVENT          WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* ---- Graphics Output Protocol ---- */
typedef struct { UINT8 Blue, Green, Red, Reserved; } EFI_GRAPHICS_OUTPUT_BLT_PIXEL;
typedef enum { PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor,
               PixelBitMask, PixelBltOnly, PixelFormatMax } EFI_GRAPHICS_PIXEL_FORMAT;
typedef struct { UINT32 RedMask, GreenMask, BlueMask, ReservedMask; } EFI_PIXEL_BITMASK;
typedef struct {
  UINT32 Version, HorizontalResolution, VerticalResolution;
  EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
  EFI_PIXEL_BITMASK PixelInformation;
  UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {
  UINT32 MaxMode, Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  UINTN SizeOfInfo;
  UINT64 FrameBufferBase;
  UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
  struct _EFI_GRAPHICS_OUTPUT_PROTOCOL*, UINT32, UINTN*, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION**);
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
  struct _EFI_GRAPHICS_OUTPUT_PROTOCOL*, UINT32);
typedef enum { EfiBltVideoFill, EfiBltVideoToBltBuffer, EfiBltBufferToVideo,
               EfiBltVideoToVideo, EfiGraphicsOutputBltOperationMax
             } EFI_GRAPHICS_OUTPUT_BLT_OPERATION;
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
  struct _EFI_GRAPHICS_OUTPUT_PROTOCOL*, EFI_GRAPHICS_OUTPUT_BLT_PIXEL*,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION,
  UINTN SourceX, UINTN SourceY, UINTN DestX, UINTN DestY,
  UINTN Width, UINTN Height, UINTN Delta);
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
  EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT        Blt;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- Loaded Image ---- */
typedef struct {
  UINT32 Revision; EFI_HANDLE ParentHandle; VOID *SystemTable;
  EFI_HANDLE DeviceHandle; VOID *FilePath; VOID *Reserved;
  UINT32 LoadOptionsSize; VOID *LoadOptions;
  VOID *ImageBase; UINT64 ImageSize;
  UINT32 ImageCodeType, ImageDataType; VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- Simple File System / File ---- */
struct _EFI_FILE_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(struct _EFI_FILE_PROTOCOL*, struct _EFI_FILE_PROTOCOL**, CHAR16*, UINT64, UINT64);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(struct _EFI_FILE_PROTOCOL*);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(struct _EFI_FILE_PROTOCOL*, UINTN*, VOID*);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(struct _EFI_FILE_PROTOCOL*, UINT64);
typedef struct _EFI_FILE_PROTOCOL {
  UINT64 Revision;
  EFI_FILE_OPEN  Open;
  EFI_FILE_CLOSE Close;
  VOID *Delete;
  EFI_FILE_READ  Read;
  VOID *Write;
  VOID *GetPosition;
  EFI_FILE_SET_POSITION SetPosition;
  VOID *GetInfo, *SetInfo, *Flush;
} EFI_FILE_PROTOCOL;
#define EFI_FILE_MODE_READ 0x0000000000000001ULL
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FS_OPEN_VOLUME)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**);
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
  UINT64 Revision;
  EFI_SIMPLE_FS_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ---- Boot Services (subset; padded to keep offsets correct) ---- */
typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
typedef enum { EfiReservedMemoryType, EfiLoaderData_ = 2 } EFI_MEMORY_TYPE_STUB;
#define EfiLoaderData 2

typedef struct {
  EFI_TABLE_HEADER Hdr;
  /* Task Priority */
  VOID *RaiseTPL, *RestoreTPL;
  /* Memory */
  EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, UINTN, UINTN, UINT64*);
  VOID *FreePages;
  VOID *GetMemoryMap;
  EFI_STATUS (EFIAPI *AllocatePool)(UINTN, UINTN, VOID**);
  EFI_STATUS (EFIAPI *FreePool)(VOID*);
  /* Event & Timer */
  VOID *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
  /* Protocol Handler */
  VOID *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
  EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID*, VOID**);
  VOID *Reserved_;
  VOID *RegisterProtocolNotify;
  EFI_STATUS (EFIAPI *LocateHandle)(UINTN, EFI_GUID*, VOID*, UINTN*, EFI_HANDLE*);
  VOID *LocateDevicePath, *InstallConfigurationTable;
  /* Image */
  VOID *LoadImage, *StartImage, *Exit, *UnloadImage, *ExitBootServices;
  /* Misc */
  VOID *GetNextMonotonicCount;
  EFI_STATUS (EFIAPI *Stall)(UINTN);
  EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
                                        UINTN DataSize, CHAR16 *WatchdogData);
  /* DriverSupport */
  VOID *ConnectController, *DisconnectController;
  /* Open/Close Protocol */
  VOID *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
  /* Library */
  VOID *ProtocolsPerHandle, *LocateHandleBuffer;
  EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID*, VOID*, VOID**);
  VOID *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces;
  VOID *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
  EFI_TABLE_HEADER Hdr;
  CHAR16 *FirmwareVendor; UINT32 FirmwareRevision;
  EFI_HANDLE ConsoleInHandle; EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
  EFI_HANDLE ConsoleOutHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE StandardErrorHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
  VOID *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;
  UINTN NumberOfTableEntries; VOID *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
 {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
 {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
 {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}

#endif
