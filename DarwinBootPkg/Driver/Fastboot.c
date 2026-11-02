// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Resident polled Qualcomm USB fastboot server for DarwinBoot.
//
// The UEFI image stays alive on the boot CPU.  XNU is started on a secondary
// CPU with the MMU and caches off; the small stubs below grad a shared log and
// hand control back to this loop.  Nothing here calls ExitBootServices().
//
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "BootArgs.h"
#include "Fastboot.h"

#define QCOM_USB_DEVICE_PROTOCOL_GUID \
  { 0xd9d9ce48, 0x44b8, 0x4f49, { 0x8e, 0x3e, 0x2a, 0x3b, 0x92, 0x7d, 0xc6, 0xc1 } }
#define QCOM_USB_INIT_EVENT_GUID \
  { 0x8d1e5c31, 0xd6a5, 0x4c62, { 0x94, 0x1e, 0x2b, 0x59, 0x3a, 0x8f, 0x77, 0x90 } }

#define USB_CHUNK_SIZE            511
#define FASTBOOT_MAX_DOWNLOAD     (64ULL * 1024 * 1024)
#define FASTBOOT_UEFI_LOG_SIZE    (64 * 1024)
#define FASTBOOT_KERNEL_LOG_MAX   (1024 * 1024)
#define FASTBOOT_STAGED_SIZE      (FASTBOOT_UEFI_LOG_SIZE + FASTBOOT_KERNEL_LOG_MAX + 8192)

#define FASTBOOT_SLOT_MAGIC       0x545352444c554e58ULL  // "XNULDRST"
#define FASTBOOT_LIVE_MAGIC       0x214556494c554e58ULL  // "XNULIVE!"
#define FASTBOOT_PSCI_CPU_OFF     0x84000002ULL
#define FASTBOOT_PSCI_CPU_ON      0xC4000003ULL
#define FASTBOOT_PSCI_ALREADY_ON  (-4)

#pragma pack(1)
typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT16  BcdUSB;
  UINT8   DeviceClass;
  UINT8   DeviceSubClass;
  UINT8   DeviceProtocol;
  UINT8   MaxPacketSize0;
  UINT16  IdVendor;
  UINT16  IdProduct;
  UINT16  BcdDevice;
  UINT8   StrManufacturer;
  UINT8   StrProduct;
  UINT8   StrSerialNumber;
  UINT8   NumConfigurations;
} QCOM_USB_DEVICE_DESCRIPTOR;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT16  TotalLength;
  UINT8   NumInterfaces;
  UINT8   ConfigurationValue;
  UINT8   Configuration;
  UINT8   Attributes;
  UINT8   MaxPower;
} QCOM_USB_CONFIG_DESCRIPTOR;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   InterfaceNumber;
  UINT8   AlternateSetting;
  UINT8   NumEndpoints;
  UINT8   InterfaceClass;
  UINT8   InterfaceSubClass;
  UINT8   InterfaceProtocol;
  UINT8   Interface;
} QCOM_USB_INTERFACE_DESCRIPTOR;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   EndpointAddress;
  UINT8   Attributes;
  UINT16  MaxPacketSize;
  UINT8   Interval;
} QCOM_USB_ENDPOINT_DESCRIPTOR;

typedef struct {
  QCOM_USB_DEVICE_DESCRIPTOR       Device;
  QCOM_USB_CONFIG_DESCRIPTOR       Config;
  QCOM_USB_INTERFACE_DESCRIPTOR    Interface;
  QCOM_USB_ENDPOINT_DESCRIPTOR     EndpointIn;
  QCOM_USB_ENDPOINT_DESCRIPTOR     EndpointOut;
  UINT8                            String0[4];
  UINT8                            String1[8];
  UINT8                            String2[36];
  UINT8                            String3[16];
} QCOM_USB_DEVICE_DESCRIPTOR_SET;
#pragma pack()

typedef
EFI_STATUS
(EFIAPI *QCOM_USB_START_EX)(
  IN VOID                           *This,
  IN QCOM_USB_DEVICE_DESCRIPTOR_SET *DescriptorSet
  );

typedef
EFI_STATUS
(EFIAPI *QCOM_USB_STOP)(
  IN VOID  *This
  );

typedef
EFI_STATUS
(EFIAPI *QCOM_USB_HANDLE_EVENT)(
  IN VOID  *This
  );

typedef
EFI_STATUS
(EFIAPI *QCOM_USB_ALLOCATE_TRANSFER_BUFFER)(
  IN  VOID                  *This,
  IN  UINTN                 Size,
  OUT VOID                  **Buffer,
  OUT EFI_PHYSICAL_ADDRESS  *DmaAddress
  );

typedef
EFI_STATUS
(EFIAPI *QCOM_USB_FREE_TRANSFER_BUFFER)(
  IN VOID  *This,
  IN VOID  *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *QCOM_USB_SEND)(
  IN VOID   *This,
  IN UINT8  Endpoint,
  IN VOID   *Buffer,
  IN UINTN  Size
  );

typedef struct {
  QCOM_USB_START_EX               StartEx;
  QCOM_USB_STOP                   Stop;
  QCOM_USB_HANDLE_EVENT           HandleEvent;
  QCOM_USB_ALLOCATE_TRANSFER_BUFFER AllocateTransferBuffer;
  QCOM_USB_FREE_TRANSFER_BUFFER   FreeTransferBuffer;
  QCOM_USB_SEND                   Send;
  UINT8                           *StringTable;
  UINT8                           *DeviceTree;
} QCOM_USB_DEVICE_PROTOCOL;

typedef struct {
  UINT64  Magic;
  UINT64  Heartbeat;
  UINT64  Log;
  UINT64  LogLength;
  UINT64  Reason;
  volatile UINT64 Stop;
  UINT64  Reserved[10];
} KERNEL_LIVE;

typedef struct {
  UINT64  Entry;
  UINT64  BootArgs;
  UINT64  Cpacr;
  UINT64  Sctlr;
  UINT64  Context;
  UINT64  StackTop;
} FASTBOOT_START_BLOCK;

STATIC EFI_GUID mQcomUsbGuid = QCOM_USB_DEVICE_PROTOCOL_GUID;
STATIC EFI_GUID mQcomUsbInitGuid = QCOM_USB_INIT_EVENT_GUID;

STATIC QCOM_USB_DEVICE_PROTOCOL *mUsb;
STATIC EFI_EVENT                 mUsbInitEvent;
STATIC volatile BOOLEAN          mUsbEvent;
STATIC BOOLEAN                   mUsbReading;
STATIC VOID                      *mUsbRx;
STATIC VOID                      *mUsbTx;
STATIC EFI_PHYSICAL_ADDRESS      mUsbRxDma;
STATIC EFI_PHYSICAL_ADDRESS      mUsbTxDma;

STATIC UINT64  mArgsAddress;
STATIC UINT64  mEntryAddress;
STATIC UINT64  mMemoryBase;
STATIC UINT64  mMemorySize;
STATIC UINT64  mKernelBase;
STATIC UINT64  mKernelSize;
STATIC UINT64  mKernelSlide;
STATIC UINT64  mKernelLivePa;
STATIC UINT64  mVideo[6];
STATIC UINT64  mCheckpoint;
STATIC BOOLEAN mBusy;
STATIC BOOLEAN mStarted;
STATIC CHAR8   mResult[64];

STATIC CHAR8   mUefiLog[FASTBOOT_UEFI_LOG_SIZE];
STATIC UINTN   mUefiLogLength;
STATIC UINT8   mStaged[FASTBOOT_STAGED_SIZE];
STATIC UINTN   mStagedLength;

STATIC CHAR8   mCommand[USB_CHUNK_SIZE + 1];

// Referenced by FastbootEntry.S.  The return stub runs with the MMU and caches
// off, so these must be identity-mapped UEFI data addresses.
volatile UINT64 gFastbootDone;
UINT64          gFastbootContext[8];
UINT8           gFastbootSecondaryStack[0x4000] __attribute__ ((aligned (16)));

extern VOID FastbootOtherCpuEntry (VOID);
extern VOID FastbootOtherCpuReturnStub (VOID);

STATIC
VOID
EFIAPI
FastbootUsbEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  (VOID)Context;
  mUsbEvent = TRUE;
}

STATIC QCOM_USB_DEVICE_DESCRIPTOR_SET mDescriptors = {
  .Device = {
    .Length = sizeof (QCOM_USB_DEVICE_DESCRIPTOR),
    .DescriptorType = 0x01,
    .BcdUSB = 0x0200,
    .DeviceClass = 0xFF,
    .DeviceSubClass = 0x42,
    .DeviceProtocol = 0x03,
    .MaxPacketSize0 = 64,
    .IdVendor = 0x18D1,
    .IdProduct = 0xD00D,
    .BcdDevice = 0x0100,
    .StrManufacturer = 1,
    .StrProduct = 2,
    .StrSerialNumber = 3,
    .NumConfigurations = 1,
  },
  .Config = {
    .Length = sizeof (QCOM_USB_CONFIG_DESCRIPTOR),
    .DescriptorType = 0x02,
    .TotalLength = sizeof (QCOM_USB_CONFIG_DESCRIPTOR) + sizeof (QCOM_USB_INTERFACE_DESCRIPTOR) +
                   sizeof (QCOM_USB_ENDPOINT_DESCRIPTOR) * 2,
    .NumInterfaces = 1,
    .ConfigurationValue = 1,
    .Configuration = 0,
    .Attributes = 0x80,
    .MaxPower = 0x32,
  },
  .Interface = {
    .Length = sizeof (QCOM_USB_INTERFACE_DESCRIPTOR),
    .DescriptorType = 0x04,
    .InterfaceNumber = 0,
    .AlternateSetting = 0,
    .NumEndpoints = 2,
    .InterfaceClass = 0xFF,
    .InterfaceSubClass = 0x42,
    .InterfaceProtocol = 0x03,
    .Interface = 0,
  },
  .EndpointIn = {
    .Length = sizeof (QCOM_USB_ENDPOINT_DESCRIPTOR),
    .DescriptorType = 0x05,
    .EndpointAddress = 0x81,
    .Attributes = 0x02,
    .MaxPacketSize = 512,
    .Interval = 0,
  },
  .EndpointOut = {
    .Length = sizeof (QCOM_USB_ENDPOINT_DESCRIPTOR),
    .DescriptorType = 0x05,
    .EndpointAddress = 0x01,
    .Attributes = 0x02,
    .MaxPacketSize = 512,
    .Interval = 0,
  },
  .String0 = { 4, 3, 0x09, 0x04 },
  .String1 = { 8, 3, 'U', 0, 'n', 0, 'i', 0 },
  .String2 = { 36, 3, 'Q', 0, 'u', 0, 'a', 0, 'l', 0, 'c', 0, 'o', 0, 'm', 0, 'm', 0, ' ', 0,
               'F', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 },
  .String3 = { 16, 3, 'p', 0, 'e', 0, 'r', 0, 'i', 0, 'd', 0, 'o', 0, 't', 0 },
};

STATIC
VOID
FastbootLog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST Marker;
  CHAR8   Buffer[256];
  UINTN   Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  if (Length >= sizeof (Buffer)) {
    Length = sizeof (Buffer) - 1;
  }
  if (mUefiLogLength + Length < sizeof (mUefiLog)) {
    CopyMem (mUefiLog + mUefiLogLength, Buffer, Length);
    mUefiLogLength += Length;
  }
  DEBUG ((DEBUG_INFO, "FASTBOOT: %a", Buffer));
}

STATIC
BOOLEAN
FastbootStartsWith (
  IN CONST CHAR8  *Text,
  IN CONST CHAR8  *Prefix
  )
{
  UINTN Length;

  Length = AsciiStrLen (Prefix);
  return AsciiStrnCmp (Text, Prefix, Length) == 0;
}

STATIC
BOOLEAN
FastbootEquals (
  IN CONST CHAR8  *Text,
  IN CONST CHAR8  *Value
  )
{
  return AsciiStrCmp (Text, Value) == 0;
}

STATIC
BOOLEAN
FastbootContains (
  IN CONST CHAR8  *Text,
  IN CONST CHAR8  *Needle
  )
{
  UINTN TextLength;
  UINTN NeedleLength;
  UINTN Index;

  TextLength = AsciiStrLen (Text);
  NeedleLength = AsciiStrLen (Needle);
  if (NeedleLength == 0) {
    return TRUE;
  }
  if (TextLength < NeedleLength) {
    return FALSE;
  }
  for (Index = 0; Index <= TextLength - NeedleLength; Index++) {
    if (AsciiStrnCmp (Text + Index, Needle, NeedleLength) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
VOID
FastbootCleanRange (
  IN UINT64  Address,
  IN UINT64  Length
  )
{
  UINT64 Current;

  if (Length == 0) {
    return;
  }
  for (Current = Address & ~0x3FULL; Current < Address + Length; Current += 64) {
    __asm__ __volatile__ ("dc cvac, %0" : : "r" (Current) : "memory");
  }
  __asm__ __volatile__ ("dsb sy" : : : "memory");
}

STATIC
VOID
FastbootInvalidateRange (
  IN UINT64  Address,
  IN UINT64  Length
  )
{
  UINT64 Current;

  if (Length == 0) {
    return;
  }
  for (Current = Address & ~0x3FULL; Current < Address + Length; Current += 64) {
    __asm__ __volatile__ ("dc civac, %0" : : "r" (Current) : "memory");
  }
  __asm__ __volatile__ ("dsb sy" : : : "memory");
}

STATIC
VOID
FastbootIcacheInvalidate (
  VOID
  )
{
  __asm__ __volatile__ ("ic iallu" : : : "memory");
  __asm__ __volatile__ ("dsb sy" : : : "memory");
  __asm__ __volatile__ ("isb" : : : "memory");
}

STATIC
UINT64
FastbootReadSctlr (
  VOID
  )
{
  UINT64 Value;

  __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Value));
  return Value;
}

STATIC
UINT64
FastbootReadCpacr (
  VOID
  )
{
  UINT64 Value;

  __asm__ __volatile__ ("mrs %0, cpacr_el1" : "=r" (Value));
  return Value;
}

STATIC
INT64
FastbootPsci (
  IN UINT64  Function,
  IN UINT64  Arg1,
  IN UINT64  Arg2,
  IN UINT64  Arg3
  )
{
  register UINT64 X0 __asm__ ("x0") = Function;
  register UINT64 X1 __asm__ ("x1") = Arg1;
  register UINT64 X2 __asm__ ("x2") = Arg2;
  register UINT64 X3 __asm__ ("x3") = Arg3;

  __asm__ __volatile__ (
    "smc #0"
    : "+r" (X0), "+r" (X1), "+r" (X2), "+r" (X3)
    :
    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
      "x12", "x13", "x14", "x15", "x16", "x17", "memory"
    );
  return (INT64)X0;
}

STATIC
UINT64 *
FastbootFindMagic (
  IN UINT64  Base,
  IN UINT64  Size,
  IN UINT64  Magic
  )
{
  UINT64 Offset;

  for (Offset = 0; Offset + 32 <= Size; Offset += 8) {
    if (*(volatile UINT64 *)(UINTN)(Base + Offset) == Magic) {
      return (UINT64 *)(UINTN)(Base + Offset);
    }
  }
  return NULL;
}

STATIC
VOID
FastbootPatchKernel (
  IN UINT64   Checkpoint,
  IN BOOLEAN  SetCheckpoint
  )
{
  UINT64       *Slot;
  UINT64       *Live;
  UINT64       Count;
  KERNEL_LIVE  *KernelLive;

  Slot = FastbootFindMagic (mMemoryBase, mKernelSize, FASTBOOT_SLOT_MAGIC);
  Live = FastbootFindMagic (mMemoryBase, mKernelSize, FASTBOOT_LIVE_MAGIC);
  mKernelLivePa = (UINT64)(UINTN)Live;
  if (Live != NULL) {
    KernelLive = (KERNEL_LIVE *)(UINTN)Live;
    KernelLive->Reason = 0;
    KernelLive->Stop = 0;
    FastbootCleanRange (mKernelLivePa, sizeof (KERNEL_LIVE));
    FastbootLog ("kernel live block at 0x%lx\n", mKernelLivePa);
  } else {
    FastbootLog ("kernel live block not found\n");
  }

  if (Slot != NULL) {
    Slot[1] = (UINT64)(UINTN)&FastbootOtherCpuReturnStub;
    Slot[2] = SetCheckpoint ? Checkpoint : 0;
    Slot[3] = mKernelSlide;
    FastbootCleanRange ((UINT64)(UINTN)Slot, 32);
    FastbootLog ("loader slot at 0x%lx checkpoint %lu\n", (UINT64)(UINTN)Slot, Slot[2]);
  } else {
    FastbootLog ("loader slot not found\n");
  }

  Count = 0;
  if (Slot != NULL) {
    Count++;
  }
  if (Live != NULL) {
    Count++;
  }
  FastbootLog ("kernel scan: %lu structures found\n", Count);
}

STATIC
VOID
FastbootUpdateContext (
  VOID
  )
{
  if (!mBusy) {
    return;
  }
  FastbootInvalidateRange ((UINT64)(UINTN)&gFastbootDone, sizeof (gFastbootDone));
  if (gFastbootDone == 0) {
    return;
  }
  FastbootInvalidateRange ((UINT64)(UINTN)gFastbootContext, sizeof (gFastbootContext));
  mBusy = FALSE;
  switch (gFastbootContext[2]) {
    case 0:
      AsciiStrCpyS (mResult, sizeof (mResult), "returned");
      break;
    case 1:
      AsciiStrCpyS (mResult, sizeof (mResult), "early-exception");
      break;
    case 2:
      AsciiStrCpyS (mResult, sizeof (mResult), "checkpoint");
      break;
    default:
      AsciiSPrint (mResult, sizeof (mResult), "stopped-%lu", gFastbootContext[2]);
      break;
  }
  FastbootLog ("kernel stopped: %a esr=%lx elr=%lx far=%lx\n",
               mResult, gFastbootContext[3], gFastbootContext[4], gFastbootContext[5]);
}

STATIC
UINTN
FastbootCopyKernelLog (
  OUT UINT8  *Dst,
  IN  UINTN  Max
  )
{
  UINT64       LogPa;
  UINT64       LogLength;
  UINT64       LogLengthPa;
  KERNEL_LIVE  *KernelLive;

  LogPa = 0;
  LogLength = 0;
  if ((gFastbootContext[0] != 0) && (gFastbootContext[1] != 0)) {
    LogPa = gFastbootContext[0];
    LogLength = gFastbootContext[1];
  } else if (mKernelLivePa != 0) {
    FastbootInvalidateRange (mKernelLivePa, sizeof (KERNEL_LIVE));
    KernelLive = (KERNEL_LIVE *)(UINTN)mKernelLivePa;
    if ((KernelLive->Magic == FASTBOOT_LIVE_MAGIC) && (KernelLive->LogLength != 0)) {
      LogLengthPa = KernelLive->LogLength + mKernelSlide;
      FastbootInvalidateRange (LogLengthPa, sizeof (UINT32));
      LogLength = *(volatile UINT32 *)(UINTN)LogLengthPa;
      LogPa = KernelLive->Log + mKernelSlide;
    }
  }
  if ((LogPa == 0) || (LogLength == 0)) {
    return 0;
  }
  if (LogLength > Max) {
    LogLength = Max;
  }
  FastbootInvalidateRange (LogPa, LogLength);
  CopyMem (Dst, (VOID *)(UINTN)LogPa, (UINTN)LogLength);
  return (UINTN)LogLength;
}

STATIC
VOID
FastbootStageLogs (
  VOID
  )
{
  UINTN Used;
  UINTN Length;
  INTN  Printed;

  FastbootUpdateContext ();
  Used = 0;
  mStagedLength = 0;
  Printed = AsciiSPrint (
              (CHAR8 *)mStaged,
              sizeof (mStaged),
              "UEFI fastboot log\nbusy=%u started=%u result=%a checkpoint=%lu\n",
              mBusy, mStarted, mResult[0] ? mResult : "none", mCheckpoint
              );
  Used = (Printed > 0) ? (UINTN)Printed : 0;
  if (Used >= sizeof (mStaged)) {
    Used = sizeof (mStaged) - 1;
  }
  if (mUefiLogLength != 0) {
    Length = MIN (mUefiLogLength, sizeof (mStaged) - Used - 1);
    CopyMem (mStaged + Used, mUefiLog, Length);
    Used += Length;
  }
  if (Used + 16 < sizeof (mStaged)) {
    CopyMem (mStaged + Used, "\nkernel log:\n", 13);
    Used += 13;
  }
  Length = FastbootCopyKernelLog (mStaged + Used, sizeof (mStaged) - Used - 1);
  Used += Length;
  if (Used + 6 < sizeof (mStaged)) {
    CopyMem (mStaged + Used, "\nend\n", 5);
    Used += 5;
  }
  mStaged[Used] = 0;
  mStagedLength = Used;
  FastbootLog ("staged %lu log bytes\n", mStagedLength);
}

STATIC
VOID
FastbootUsbPump (
  VOID
  )
{
  UINTN Index;

  if (mUsb == NULL) {
    return;
  }
  for (Index = 0; Index < 8; Index++) {
    mUsb->HandleEvent (mUsb);
    gBS->Stall (100);
  }
  mUsbEvent = FALSE;
}

STATIC
EFI_STATUS
FastbootUsbTxRaw (
  IN CONST VOID  *Data,
  IN UINTN       Length
  )
{
  CONST UINT8  *Bytes;
  UINTN        Chunk;
  EFI_STATUS   Status;

  if (mUsb == NULL) {
    return EFI_NOT_READY;
  }
  Bytes = Data;
  while (Length != 0) {
    Chunk = MIN (Length, USB_CHUNK_SIZE);
    CopyMem (mUsbTx, Bytes, Chunk);
    Status = mUsb->Send (mUsb, 0x81, mUsbTx, Chunk);
    if (EFI_ERROR (Status)) {
      FastbootLog ("usb tx failed: %r\n", Status);
      return Status;
    }
    FastbootUsbPump ();
    Bytes += Chunk;
    Length -= Chunk;
  }
  return EFI_SUCCESS;
}

STATIC
VOID
FastbootReply (
  IN CONST CHAR8  *StatusText,
  IN CONST CHAR8  *Info
  )
{
  CHAR8  Reply[256];
  UINTN  Length;

  Length = AsciiSPrint (Reply, sizeof (Reply), "%a%a", StatusText, Info);
  if (Length >= sizeof (Reply)) {
    Length = sizeof (Reply) - 1;
  }
  FastbootUsbTxRaw (Reply, Length);
}

STATIC
VOID
FastbootFail (
  IN CONST CHAR8  *Info
  )
{
  FastbootReply ("FAIL", Info);
}

STATIC
VOID
FastbootUploadLog (
  VOID
  )
{
  CHAR8  Header[16];
  UINTN  HeaderLength;

  FastbootStageLogs ();
  HeaderLength = AsciiSPrint (Header, sizeof (Header), "DATA%08x", (UINT32)mStagedLength);
  if (HeaderLength != 12) {
    return;
  }
  FastbootUsbTxRaw (Header, HeaderLength);
  FastbootUsbTxRaw (mStaged, mStagedLength);
}

STATIC
EFI_STATUS
FastbootUsbRead (
  IN  BOOLEAN  Blocking,
  OUT UINTN    *Length
  )
{
  EFI_STATUS  Status;

  if (mUsb == NULL) {
    return EFI_NOT_READY;
  }
  if (!mUsbReading) {
    ZeroMem (mUsbRx, USB_CHUNK_SIZE);
    mUsbEvent = FALSE;
    Status = mUsb->Send (mUsb, 0x01, mUsbRx, USB_CHUNK_SIZE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mUsbReading = TRUE;
  }
  for (;;) {
    mUsb->HandleEvent (mUsb);
    if (mUsbEvent) {
      mUsbEvent = FALSE;
      if (mUsbReading) {
        mUsbReading = FALSE;
        *Length = USB_CHUNK_SIZE;
        return EFI_SUCCESS;
      }
    }
    if (!Blocking) {
      return EFI_NOT_READY;
    }
    gBS->Stall (1000);
  }
}

STATIC
EFI_STATUS
FastbootStartKernel (
  IN UINT64  Checkpoint
  )
{
  FASTBOOT_START_BLOCK  Start;
  INT64                 PsciStatus;
  UINT64                Core;
  UINT64                StartAddress;

  if (mBusy || mStarted) {
    return EFI_ACCESS_DENIED;
  }
  FastbootPatchKernel (Checkpoint, TRUE);
  FastbootLog ("starting kernel entry=0x%lx args=0x%lx slide=0x%lx\n",
               mEntryAddress, mArgsAddress, mKernelSlide);
  ZeroMem (&Start, sizeof (Start));
  ZeroMem ((VOID *)gFastbootContext, sizeof (gFastbootContext));
  gFastbootDone = 0;
  Start.Entry = mEntryAddress;
  Start.BootArgs = mArgsAddress;
  Start.Cpacr = FastbootReadCpacr ();
  Start.Sctlr = FastbootReadSctlr ();
  Start.Context = (UINT64)(UINTN)gFastbootContext;
  Start.StackTop = (UINT64)(UINTN)(gFastbootSecondaryStack + sizeof (gFastbootSecondaryStack));
  StartAddress = (UINT64)(UINTN)&Start;

  FastbootCleanRange ((UINT64)(UINTN)&FastbootOtherCpuEntry,
                      (UINT64)(UINTN)&FastbootOtherCpuReturnStub + 0x200 -
                      (UINT64)(UINTN)&FastbootOtherCpuEntry);
  FastbootCleanRange ((UINT64)(UINTN)gFastbootSecondaryStack, sizeof (gFastbootSecondaryStack));
  FastbootCleanRange ((UINT64)(UINTN)&Start, sizeof (Start));
  FastbootCleanRange ((UINT64)(UINTN)gFastbootContext, sizeof (gFastbootContext));
  FastbootCleanRange ((UINT64)(UINTN)&gFastbootDone, sizeof (gFastbootDone));
  FastbootCleanRange (mMemoryBase, mMemorySize);
  FastbootIcacheInvalidate ();

  mBusy = TRUE;
  mResult[0] = 0;
  for (Core = 7; Core >= 1; Core--) {
    PsciStatus = FastbootPsci (FASTBOOT_PSCI_CPU_ON, Core, (UINT64)(UINTN)&FastbootOtherCpuEntry, StartAddress);
    if (PsciStatus == 0) {
      mStarted = TRUE;
      mCheckpoint = Checkpoint;
      AsciiStrCpyS (mResult, sizeof (mResult), "running");
      FastbootLog ("kernel on cpu%lu checkpoint %lu\n", Core, Checkpoint);
      return EFI_SUCCESS;
    }
    if (PsciStatus == FASTBOOT_PSCI_ALREADY_ON) {
      FastbootLog ("cpu%lu already on\n", Core);
      continue;
    }
    FastbootLog ("cpu%lu CPU_ON returned %ld\n", Core, PsciStatus);
  }
  mBusy = FALSE;
  AsciiStrCpyS (mResult, sizeof (mResult), "not-started");
  FastbootLog ("PSCI CPU_ON failed\n");
  return EFI_DEVICE_ERROR;
}

STATIC
VOID
FastbootDownload (
  IN CONST CHAR8  *Command
  )
{
  UINT64      Size;
  UINT64      Remaining;
  UINTN       Length;
  EFI_STATUS  Status;
  CHAR8       Header[16];
  UINTN       HeaderLength;

  Size = AsciiStrHexToUintn (Command + 9);
  if (Size > FASTBOOT_MAX_DOWNLOAD) {
    FastbootFail ("download too large");
    return;
  }
  HeaderLength = AsciiSPrint (Header, sizeof (Header), "DATA%08x", (UINT32)Size);
  if (HeaderLength != 12) {
    FastbootFail ("download unavailable");
    return;
  }
  FastbootUsbTxRaw (Header, HeaderLength);
  Remaining = Size;
  while (Remaining != 0) {
    Status = FastbootUsbRead (TRUE, &Length);
    if (EFI_ERROR (Status)) {
      FastbootFail ("download read failed");
      return;
    }
    Remaining -= MIN (Remaining, (UINT64)Length);
  }
  FastbootReply ("OKAY", "downloaded\n");
}

STATIC
VOID
FastbootGetVar (
  IN CONST CHAR8  *Name
  )
{
  CHAR8  Value[96];

  if (FastbootEquals (Name, "result")) {
    FastbootReply ("OKAY", mResult[0] ? mResult : "unknown");
  } else if (FastbootEquals (Name, "checkpoint")) {
    AsciiSPrint (Value, sizeof (Value), "%lu", mCheckpoint);
    FastbootReply ("OKAY", Value);
  } else if (FastbootEquals (Name, "product")) {
    FastbootReply ("OKAY", "unixnu-peridot");
  } else if (FastbootEquals (Name, "version")) {
    FastbootReply ("OKAY", "0.1");
  } else if (FastbootEquals (Name, "version-bootloader")) {
    FastbootReply ("OKAY", "DarwinBoot");
  } else if (FastbootEquals (Name, "serialno")) {
    FastbootReply ("OKAY", "peridot");
  } else if (FastbootEquals (Name, "secure")) {
    FastbootReply ("OKAY", "no");
  } else if (FastbootEquals (Name, "max-download-size")) {
    FastbootReply ("OKAY", "0x04000000");
  } else if (FastbootEquals (Name, "kernel")) {
    FastbootReply ("OKAY", mStarted ? "started" : "idle");
  } else if (FastbootEquals (Name, "log-size")) {
    AsciiSPrint (Value, sizeof (Value), "%lu", mStagedLength);
    FastbootReply ("OKAY", Value);
  } else if (FastbootEquals (Name, "all")) {
    FastbootReply ("INFO", "product: unixnu-peridot\n");
    FastbootReply ("INFO", "version: 0.1\n");
    FastbootReply ("INFO", "secure: no\n");
    FastbootReply ("INFO", "max-download-size: 0x04000000\n");
    FastbootReply ("OKAY", "");
  } else {
    FastbootFail ("unknown variable");
  }
}

STATIC
VOID
FastbootOem (
  IN CONST CHAR8  *Command
  )
{
  CONST CHAR8  *Arg;
  UINT64       Checkpoint;
  EFI_STATUS   Status;
  KERNEL_LIVE  *KernelLive;

  Arg = Command + 4;
  while (*Arg == ' ') {
    Arg++;
  }
  if (FastbootStartsWith (Arg, "run")) {
    if (mBusy || mStarted) {
      FastbootFail ("kernel already started");
      return;
    }
    Arg += 3;
    while (*Arg == ' ') {
      Arg++;
    }
    Checkpoint = (*Arg != 0) ? AsciiStrDecimalToUintn (Arg) : 0;
    Status = FastbootStartKernel (Checkpoint);
    if (EFI_ERROR (Status)) {
      FastbootFail ("kernel start failed");
    } else {
      FastbootReply ("OKAY", "kernel started\n");
    }
  } else if (FastbootStartsWith (Arg, "log")) {
    FastbootStageLogs ();
    FastbootReply ("OKAY", "log staged\n");
  } else if (FastbootStartsWith (Arg, "stop")) {
    if (!mBusy || (mKernelLivePa == 0)) {
      FastbootFail ("kernel not running");
      return;
    }
    FastbootInvalidateRange (mKernelLivePa, sizeof (KERNEL_LIVE));
    KernelLive = (KERNEL_LIVE *)(UINTN)mKernelLivePa;
    KernelLive->Stop = 1;
    FastbootCleanRange (mKernelLivePa, sizeof (KERNEL_LIVE));
    FastbootReply ("OKAY", "stop requested\n");
  } else if (FastbootStartsWith (Arg, "hang")) {
    FastbootReply ("OKAY", "hanging\n");
    CpuDeadLoop ();
  } else {
    FastbootReply ("INFO", "oem run [checkpoint] | oem log | oem stop | oem hang\n");
    FastbootReply ("OKAY", "");
  }
}

STATIC
VOID
FastbootCommand (
  IN CONST CHAR8  *Command
  )
{
  FastbootLog ("command: %a\n", Command);

  if (FastbootStartsWith (Command, "getvar:")) {
    FastbootGetVar (Command + 7);
  } else if (FastbootStartsWith (Command, "download:")) {
    FastbootDownload (Command);
  } else if (FastbootEquals (Command, "upload") || FastbootEquals (Command, "get_staged")) {
    FastbootUploadLog ();
  } else if (FastbootStartsWith (Command, "oem ")) {
    FastbootOem (Command);
  } else if (FastbootEquals (Command, "boot")) {
    if (mBusy || mStarted) {
      FastbootFail ("kernel already started");
      return;
    }
    if (!EFI_ERROR (FastbootStartKernel (0))) {
      FastbootReply ("OKAY", "kernel started\n");
    } else {
      FastbootFail ("kernel start failed");
    }
  } else if (FastbootEquals (Command, "reboot")) {
    FastbootReply ("OKAY", "rebooting\n");
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop ();
  } else {
    FastbootFail ("unknown command");
  }
}

STATIC
EFI_STATUS
FastbootUsbInit (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&mQcomUsbGuid, NULL, (VOID **)&mUsb);
  if (EFI_ERROR (Status)) {
    FastbootLog ("Qcom USB protocol unavailable: %r\n", Status);
    return Status;
  }
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  (EFI_EVENT_NOTIFY)FastbootUsbEvent,
                  NULL,
                  &mQcomUsbInitGuid,
                  &mUsbInitEvent
                  );
  if (EFI_ERROR (Status)) {
    FastbootLog ("USB init event failed: %r\n", Status);
    return Status;
  }
  gBS->SignalEvent (mUsbInitEvent);
  Status = mUsb->StartEx (mUsb, &mDescriptors);
  if (EFI_ERROR (Status)) {
    FastbootLog ("USB start_ex failed: %r\n", Status);
    return Status;
  }
  Status = mUsb->AllocateTransferBuffer (mUsb, USB_CHUNK_SIZE, &mUsbRx, &mUsbRxDma);
  if (EFI_ERROR (Status)) {
    FastbootLog ("USB rx buffer failed: %r\n", Status);
    return Status;
  }
  Status = mUsb->AllocateTransferBuffer (mUsb, USB_CHUNK_SIZE, &mUsbTx, &mUsbTxDma);
  if (EFI_ERROR (Status)) {
    FastbootLog ("USB tx buffer failed: %r\n", Status);
    return Status;
  }
  FastbootLog ("USB fastboot ready (rx 0x%lx tx 0x%lx)\n", mUsbRxDma, mUsbTxDma);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DarwinFastbootMode (
  IN UINT64       ArgsAddress,
  IN UINT64       EntryAddress,
  IN UINT64       MemoryBase,
  IN UINT64       MemorySize,
  IN UINT64       KernelBase,
  IN UINT64       KernelSize,
  IN UINT64       Video[6],
  IN CONST CHAR8  *CommandLine
  )
{
  DARWIN_BOOT_ARGS  *Args;
  EFI_STATUS        Status;
  UINTN             Length;
  UINTN             Index;

  mArgsAddress = ArgsAddress;
  mEntryAddress = EntryAddress;
  mMemoryBase = MemoryBase;
  mMemorySize = MemorySize;
  mKernelBase = KernelBase;
  mKernelSize = KernelSize;
  mKernelSlide = MemoryBase - KernelBase;
  mKernelLivePa = 0;
  mBusy = FALSE;
  mStarted = FALSE;
  mCheckpoint = 0;
  mResult[0] = 0;
  CopyMem (mVideo, Video, sizeof (mVideo));
  Args = (DARWIN_BOOT_ARGS *)(UINTN)ArgsAddress;
  if (!FastbootContains (Args->CommandLine, "cpus=")) {
    AsciiStrnCatS (Args->CommandLine, sizeof (Args->CommandLine), " cpus=1", 7);
  }
  Print (L"DARWINBOOT: fastboot mode; resident UEFI, secondary-core XNU\n");
  FastbootLog ("fastboot mode args=0x%lx entry=0x%lx kernel=0x%lx+0x%lx slide=0x%lx\n",
               ArgsAddress, EntryAddress, KernelBase, KernelSize, mKernelSlide);
  FastbootLog ("command line: %a\n", Args->CommandLine);
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Status = FastbootUsbInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  FastbootPatchKernel (0, FALSE);
  for (;;) {
    FastbootUpdateContext ();
    Status = FastbootUsbRead (mBusy ? FALSE : TRUE, &Length);
    if (Status == EFI_SUCCESS) {
      Index = 0;
      while ((Index < Length) &&
             (((UINT8 *)mUsbRx)[Index] >= 0x20) &&
             (((UINT8 *)mUsbRx)[Index] <= 0x7E)) {
        Index++;
      }
      if (Index != 0) {
        CopyMem (mCommand, mUsbRx, Index);
        mCommand[Index] = 0;
        FastbootCommand (mCommand);
      }
    } else if ((Status != EFI_NOT_READY) && (Status != EFI_TIMEOUT)) {
      FastbootLog ("USB read failed: %r\n", Status);
      gBS->Stall (10000);
    }
    if (mBusy) {
      FastbootUpdateContext ();
      gBS->Stall (2000);
    }
  }
  return EFI_ABORTED;
}
