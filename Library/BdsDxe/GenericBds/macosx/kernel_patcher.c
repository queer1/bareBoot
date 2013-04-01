/*
 * Copyright (c) 2011-2012 Frank Peng. All rights reserved.
 *
 */

#include "macosx.h"
#include "LoaderUefi.h"
#include "device_tree.h"

#include "kernel_patcher.h"
#include "sse3_patcher.h"
#include "sse3_5_patcher.h"

EFI_PHYSICAL_ADDRESS    KernelRelocBase = 0;
BootArgs1               *bootArgs1      = NULL;
BootArgs2               *bootArgs2      = NULL;
CHAR8                   *dtRoot         = NULL;
VOID                    *KernelData     = NULL;
UINT32                  KernelSlide     = 0;
BOOLEAN                 isKernelcache   = FALSE;
BOOLEAN                 is64BitKernel   = FALSE;
BOOLEAN                 PatcherInited   = FALSE;
BOOLEAN                 SSSE3;

// notes:
// - 64bit segCmd64->vmaddr is 0xffffff80xxxxxxxx and we are taking
//   only lower 32bit part into PrelinkTextAddr
// - PrelinkTextAddr is segCmd64->vmaddr + KernelRelocBase
UINT32     PrelinkTextLoadCmdAddr = 0;
UINT32     PrelinkTextAddr = 0;
UINT32     PrelinkTextSize = 0;

// notes:
// - 64bit sect->addr is 0xffffff80xxxxxxxx and we are taking
//   only lower 32bit part into PrelinkInfoAddr
// - PrelinkInfoAddr is sect->addr + KernelRelocBase
UINT32     PrelinkInfoLoadCmdAddr = 0;
UINT32     PrelinkInfoAddr = 0;
UINT32     PrelinkInfoSize = 0;

VOID
SetKernelRelocBase (
  VOID
)
{
    EFI_STATUS      Status;
    UINTN           DataSize = sizeof(KernelRelocBase);
    
    KernelRelocBase = 0;
    // OsxAptioFixDrv will set this
    Status = gRT->GetVariable(
                    L"OsxAptioFixDrv-RelocBase",
                    &gEfiAppleBootGuid,
                    NULL,
                    &DataSize,
                    &KernelRelocBase
                  );
    // KernelRelocBase is now either read or 0
    return;
}

VOID
KernelPatcher_64 (
  VOID  *kernelData
)
{
  BOOLEAN     check;
  UINT8       *bytes;
  UINT32      patchLocation;
  UINT32      patchLocation1;
  UINT32      i;
  UINT32      jumpaddr;
  UINT32      cpuid_family_addr;

  check = TRUE;
  bytes = (UINT8 *) kernelData;
  patchLocation=0;
  patchLocation1=0;

#if 0
  if (AsciiStrnCmp (OSVersion,"10.7",4) == 0) return;
#endif
  // _cpuid_set_info _panic address
  for (i = 0; i < 0x1000000; i++) {
    if (bytes[i] == 0xC7 && bytes[i+1] == 0x05 && bytes[i+6] == 0x07 && bytes[i+7] == 0x00 &&
        bytes[i+8] == 0x00 && bytes[i+9] == 0x00 && bytes[i+10] == 0xC7 && bytes[i+11] == 0x05 &&
        bytes[i-5] == 0xE8) {
      patchLocation = i-5;
      break;
    }
  }
   
  if (!patchLocation) {
       return;
  }
  
  // this for 10.6.0 and 10.6.1 kernel and remove tsc.c unknow cpufamily panic
  //  488d3df4632a00
  // find _tsc_init panic address
  for (i = 0; i < 0x1000000; i++) {   // _cpuid_set_info _panic address
    if (bytes[i] == 0x48 && bytes[i+1] == 0x8D && bytes[i+2] == 0x3D && bytes[i+3] == 0xF4 &&
        bytes[i+4] == 0x63 && bytes[i+5] == 0x2A && bytes[i+6] == 0x00) {
      patchLocation1 = i+9;
      break;
    }
  }
  
  // found _tsc_init panic addres and patch it
  if (patchLocation1) {
    bytes[patchLocation1 + 0] = 0x90;
    bytes[patchLocation1 + 1] = 0x90;
    bytes[patchLocation1 + 2] = 0x90;
    bytes[patchLocation1 + 3] = 0x90;
    bytes[patchLocation1 + 4] = 0x90;
  }
  // end tsc.c panic

  //first move panic code total 5 bytes, if patch cpuid fail still can boot with kernel
  bytes[patchLocation + 0] = 0x90;
  bytes[patchLocation + 1] = 0x90;
  bytes[patchLocation + 2] = 0x90;
  bytes[patchLocation + 3] = 0x90;
  bytes[patchLocation + 4] = 0x90;
  
  jumpaddr = patchLocation;
       
  for (i = 0; i < 500; i++) {
    if( bytes[jumpaddr-i-4] == 0x85 && bytes[jumpaddr-i-3] == 0xC0 &&
        bytes[jumpaddr-i-2] == 0x0f ) {
      jumpaddr -= i;
      bytes[jumpaddr-1] = 0x87;
      bytes[jumpaddr] -= 10;
      break;
    }
  }
  
  if (jumpaddr == patchLocation) {
    for(i = 0; i < 500; i++) {
      if( bytes[jumpaddr-i-3] == 0x85 && bytes[jumpaddr-i-2] == 0xC0 &&
          bytes[jumpaddr-i-1] == 0x75 ) {
        jumpaddr -= i;
        bytes[jumpaddr-1] = 0x77;
        check = FALSE;
        break;
      }
    }    
  }
  
  if (jumpaddr == patchLocation) {
    return;  //can't find jump location
  }
  
  if (check) {
    cpuid_family_addr = bytes[jumpaddr + 6] <<  0 |
                        bytes[jumpaddr + 7] <<  8 |
                        bytes[jumpaddr + 8] << 16 |
                        bytes[jumpaddr + 9] << 24;
  } else {
    cpuid_family_addr = bytes[jumpaddr + 3] <<  0 |
                        bytes[jumpaddr + 4] <<  8 |
                        bytes[jumpaddr + 5] << 16 |
                        bytes[jumpaddr + 6] << 24;
  }
     
  if (check) {
    bytes[patchLocation - 13] = (CPUFAMILY_INTEL_YONAH & 0x000000FF) >>  0;
    bytes[patchLocation - 12] = (CPUFAMILY_INTEL_YONAH & 0x0000FF00) >>  8;
    bytes[patchLocation - 11] = (CPUFAMILY_INTEL_YONAH & 0x00FF0000) >> 16;
    bytes[patchLocation - 10] = (CPUFAMILY_INTEL_YONAH & 0xFF000000) >> 24;
  }
      
  if (check && (AsciiStrnCmp (OSVersion, "10.6.8", 6) != 0) &&
      (AsciiStrnCmp (OSVersion, "10.7", 4) == 0)) {
    cpuid_family_addr -= 255;
  }

  if (!check) {
    cpuid_family_addr += 10;
  }
  
  if (AsciiStrnCmp (OSVersion,"10.6.8",6) == 0) goto SE3;
  
  //patch info->cpuid_cpufamily
  bytes[patchLocation -  9] = 0x90;
  bytes[patchLocation -  8] = 0x90;
  
  bytes[patchLocation -  7] = 0xC7;
  bytes[patchLocation -  6] = 0x05;

  bytes[patchLocation -  5] = (cpuid_family_addr & 0x000000FF);
  bytes[patchLocation -  4] = (UINT8) ((cpuid_family_addr & 0x0000FF00) >>  8);
  bytes[patchLocation -  3] = (UINT8) ((cpuid_family_addr & 0x00FF0000) >> 16);
  bytes[patchLocation -  2] = (UINT8) ((cpuid_family_addr & 0xFF000000) >> 24);
  
  bytes[patchLocation -  1] = CPUIDFAMILY_DEFAULT; //cpuid_family need alway set 0x06
  bytes[patchLocation +  0] = CPUID_MODEL_YONAH;   //cpuid_model set CPUID_MODEL_MEROM
  bytes[patchLocation +  1] = 0x01;                //cpuid_extmodel alway set 0x01
  bytes[patchLocation +  2] = 0x00;                //cpuid_extfamily alway set 0x00
  bytes[patchLocation +  3] = 0x90;                
  bytes[patchLocation +  4] = 0x90;
SE3:
  // patch sse3
  if (!SSSE3 && (AsciiStrnCmp (OSVersion, "10.6", 4) == 0)) {
      Patcher_SSE3_6 ((VOID *) bytes);
  }
  if (!SSSE3 && (AsciiStrnCmp (OSVersion, "10.7", 4) == 0)) {
      Patcher_SSE3_7 ((VOID *) bytes);
  }
}

VOID
KernelPatcher_32 (
  VOID  *kernelData
)
{
  UINT8   *bytes;
  UINT32  patchLocation;
  UINT32  patchLocation1;
  UINT32  i;
  UINT32  jumpaddr;

  bytes = (UINT8 *) kernelData;
  patchLocation = 0;
  patchLocation1 = 0;
  
  // _cpuid_set_info _panic address
  for (i = 0; i < 0x1000000; i++) {
    if (bytes[i] == 0xC7 && bytes[i+1] == 0x05 && bytes[i+6] == 0x07 && bytes[i+7] == 0x00 &&
        bytes[i+8] == 0x00 && bytes[i+9] == 0x00 && bytes[i+10] == 0xC7 && bytes[i+11] == 0x05 &&
        bytes[i-5] == 0xE8) {
      patchLocation = i-5;
      break;
    }
  }
   
  if (!patchLocation) {
    return;
  }
  
  // this for 10.6.0 and 10.6.1 kernel and remove tsc.c unknow cpufamily panic
  //  c70424540e5900
  // find _tsc_init panic address
  for (i = 0; i < 0x1000000; i++) {   // _cpuid_set_info _panic address
    if (bytes[i] == 0xC7 && bytes[i+1] == 0x04 && bytes[i+2] == 0x24 && bytes[i+3] == 0x54 &&
        bytes[i+4] == 0x0E && bytes[i+5] == 0x59 && bytes[i+6] == 0x00) {
      patchLocation1 = i+7;
      break;
    }
  }
  
  // found _tsc_init panic addres and patch it
  if (patchLocation1) {
    bytes[patchLocation1 + 0] = 0x90;
    bytes[patchLocation1 + 1] = 0x90;
    bytes[patchLocation1 + 2] = 0x90;
    bytes[patchLocation1 + 3] = 0x90;
    bytes[patchLocation1 + 4] = 0x90;
  }
  // end tsc.c panic
  
  //first move panic code total 5 bytes, if patch cpuid fail still can boot with kernel
  bytes[patchLocation + 0] = 0x90;
  bytes[patchLocation + 1] = 0x90;
  bytes[patchLocation + 2] = 0x90;
  bytes[patchLocation + 3] = 0x90;
  bytes[patchLocation + 4] = 0x90;
   
  jumpaddr = patchLocation;
   
  for (i = 0; i < 500; i++) {
    if (bytes[jumpaddr-i-3] == 0x85 && bytes[jumpaddr-i-2] == 0xC0 &&
        bytes[jumpaddr-i-1] == 0x75 ) {
      jumpaddr -= i;
      bytes[jumpaddr-1] = 0x77;
      if(bytes[patchLocation - 17] == 0xC7) {
        bytes[jumpaddr] -=10;
      }
      break;
    } 
  }

  if (jumpaddr == patchLocation) {
      return;  //can't find jump location
  }

  // patch info_p->cpufamily to CPUFAMILY_INTEL_MEROM
  if (bytes[patchLocation - 17] == 0xC7) {
    bytes[patchLocation - 11] = (CPUFAMILY_INTEL_YONAH & 0x000000FF) >>  0;
    bytes[patchLocation - 10] = (CPUFAMILY_INTEL_YONAH & 0x0000FF00) >>  8;
    bytes[patchLocation -  9] = (CPUFAMILY_INTEL_YONAH & 0x00FF0000) >> 16;
    bytes[patchLocation -  8] = (CPUFAMILY_INTEL_YONAH & 0xFF000000) >> 24;
  } 
  
  //patch info->cpuid_cpufamily
  bytes[patchLocation -  7] = 0xC7;
  bytes[patchLocation -  6] = 0x05;
  bytes[patchLocation -  5] = bytes[jumpaddr + 3];
  bytes[patchLocation -  4] = bytes[jumpaddr + 4];
  bytes[patchLocation -  3] = bytes[jumpaddr + 5];
  bytes[patchLocation -  2] = bytes[jumpaddr + 6];
  
  bytes[patchLocation -  1] = CPUIDFAMILY_DEFAULT; //cpuid_family  need alway set 0x06
  bytes[patchLocation +  0] = CPUID_MODEL_YONAH;   //cpuid_model set CPUID_MODEL_MEROM
  bytes[patchLocation +  1] = 0x01;                //cpuid_extmodel alway set 0x01
  bytes[patchLocation +  2] = 0x00;                //cpuid_extfamily alway set 0x00
  bytes[patchLocation +  3] = 0x90;
  bytes[patchLocation +  4] = 0x90;
  
  if (AsciiStrnCmp (OSVersion, "10.7", 4) == 0) {
    return;
  }
  
  if (!SSSE3 && (AsciiStrnCmp (OSVersion, "10.6", 4) == 0)) {
    Patcher_SSE3_6 ((VOID *) bytes);
  }
  if (!SSSE3 && (AsciiStrnCmp (OSVersion, "10.5", 4) == 0)) {
    Patcher_SSE3_5 ((VOID *) bytes);
  } 
}
       
VOID
Patcher_SSE3_6 (
  VOID  *kernelData
)
{
  UINT8   *bytes;
  UINT32  patchLocation1;
  UINT32  patchLocation2;
  UINT32  patchlast;
  UINT32  i; 

  bytes = (UINT8 *) kernelData;
  patchLocation1 = 0;
  patchLocation2 = 0;
  patchlast = 0;
  i = 0;

  while (TRUE) {
    if (bytes[i] == 0x66 && bytes[i+1] == 0x0F && bytes[i+2] == 0x6F && 
        bytes[i+3] == 0x44 && bytes[i+4] == 0x0E && bytes[i+5] == 0xF1 &&
        bytes[i-1664-32] == 0x55) {
      patchLocation1 = i-1664-32;
    }

    // khasSSE2+..... title
    if (bytes[i] == 0xE3 && bytes[i+1] == 0x07 && bytes[i+2] == 0x00 &&
        bytes[i+3] == 0x00 && bytes[i+4] == 0x80 && bytes[i+5] == 0x07 &&
        bytes[i+6] == 0xFF && bytes[i+7] == 0xFF && bytes[i+8] == 0x24 &&
        bytes[i+9] == 0x01) {
      patchLocation2 = i;
      break;
    }
    i++;
  }
  
  if (!patchLocation1 || !patchLocation2) {
    return;
  }
   
  i = patchLocation1 + 1500;
  while (TRUE) {
    if (bytes[i] == 0x90 && bytes[i+1] == 0x90 && bytes[i+2] == 0x55 ) {
      patchlast = (i+1) - patchLocation1;
      break;
    }
    i++;
  }
   
  if (!patchlast) {
    return;
  }

  // patch sse3_64 data
  for (i = 0; i < patchlast; i++) {
    if (i < sizeof (sse3_patcher)) {
      bytes[patchLocation1 + i] = sse3_patcher[i];
    } else {
      bytes[patchLocation1 + i] = 0x90;
    }
  }

  // patch kHasSSE3 title
  bytes[patchLocation2 + 0] = 0xFC;
  bytes[patchLocation2 + 1] = 0x05;
  bytes[patchLocation2 + 8] = 0x2C;
  bytes[patchLocation2 + 9] = 0x00;
}

VOID
Patcher_SSE3_5 (
  VOID  *kernelData
)
{
  UINT8   *bytes;
  UINT32  patchLocation1;
  UINT32  patchLocation2;
  UINT32  patchlast;
  UINT32  Length;
  UINT32  i; 

  bytes = (UINT8 *) kernelData;
  patchLocation1 = 0;
  patchLocation2 = 0;
  patchlast = 0;
  Length = sizeof (kernelData);

  for (i = 256; i < (Length - 256); i++) {
    if (bytes[i] == 0x66 && bytes[i+1] == 0x0F && bytes[i+2] == 0x6F &&
        bytes[i+3] == 0x44 && bytes[i+4] == 0x0E && bytes[i+5] == 0xF1 &&
        bytes[i-1680-32] == 0x55) {
      patchLocation1 = i-1680-32;
    }

    // khasSSE2+..... title
    if (bytes[i] == 0xF3 && bytes[i+1] == 0x07 && bytes[i+2] == 0x00 &&
        bytes[i+3] == 0x00 && bytes[i+4] == 0x80 && bytes[i+5] == 0x07 &&
        bytes[i+6] == 0xFF && bytes[i+7] == 0xFF && bytes[i+8] == 0x24 &&
        bytes[i+9] == 0x01) {
       patchLocation2 = i;
       break;
    }
  }            
  
  if (!patchLocation1 || !patchLocation2)  {
    return;
  }
   
  for (i = (patchLocation1 + 1500); i < Length; i++) {
    if (bytes[i] == 0x90 && bytes[i+1] == 0x90 && bytes[i+2] == 0x55) {
     patchlast = (i+1) - patchLocation1;
     break;
    }
  }

  if (!patchlast) {
    return;
  }

  // patech sse3_64 data
  for (i = 0; i < patchlast; i++) {
    if (i < sizeof (sse3_5_patcher)) {
      bytes[patchLocation1 + i] = sse3_5_patcher[i];
    } else {
        bytes[patchLocation1 + i] = 0x90;
    }
  }

  // patch kHasSSE3 title
  bytes[patchLocation2 + 0] = 0x0C;
  bytes[patchLocation2 + 1] = 0x06;
  bytes[patchLocation2 + 8] = 0x2C;
  bytes[patchLocation2 + 9] = 0x00;
} 

VOID
Patcher_SSE3_7 (
  VOID  *kernelData
)
{
     // not support yet
  return;
}

VOID
Get_PreLink (
  VOID
)
{
  UINT32  ncmds;
  UINT32  cmdsize;
  UINT32  binaryIndex;
  UINTN   cnt;
  UINT8   *binary;
  UINT32  sectionIndex;

  struct load_command         *loadCommand;
  struct segment_command      *segCmd;
  struct segment_command_64   *segCmd64;
  struct section              *sect;
  struct section_64           *sect64;

  binary = (UINT8 *) KernelData;

  if (is64BitKernel) {
    binaryIndex = sizeof (struct mach_header_64);
  } else {
    binaryIndex = sizeof (struct mach_header);
  }
  
  ncmds = MACH_GET_NCMDS(binary);
  
  for (cnt = 0; cnt < ncmds; cnt++) {
    loadCommand = (struct load_command *) (binary + binaryIndex);
    cmdsize = loadCommand->cmdsize;
    
    switch (loadCommand->cmd) {
      case LC_SEGMENT_64: 
        segCmd64 = (struct segment_command_64 *) loadCommand;
        if (AsciiStrCmp (segCmd64->segname, kPrelinkTextSegment) == 0) {
          if (segCmd64->vmsize > 0) {
            PrelinkTextAddr = (UINT32) (segCmd64->vmaddr ? segCmd64->vmaddr + KernelRelocBase : 0);
            PrelinkTextSize = (UINT32) segCmd64->vmsize;
            PrelinkTextLoadCmdAddr = (UINT32) (UINTN) segCmd64;
          }
        }
        if (AsciiStrCmp (segCmd64->segname, kPrelinkInfoSegment) == 0) {
          sectionIndex = sizeof (struct segment_command_64);
          
          while(sectionIndex < segCmd64->cmdsize) {
            sect64 = (struct section_64 *) ((UINT8 *) segCmd64 + sectionIndex);
            sectionIndex += sizeof (struct section_64);
            
            if(AsciiStrCmp (sect64->sectname, kPrelinkInfoSection) == 0 &&
               AsciiStrCmp (sect64->segname, kPrelinkInfoSegment) == 0) {
              if (sect64->size > 0) {
                PrelinkInfoLoadCmdAddr = (UINT32) (UINTN) sect64;
                PrelinkInfoAddr = (UINT32) (sect64->addr ? sect64->addr + KernelRelocBase : 0);
                PrelinkInfoSize = (UINT32) sect64->size;
              }
            }
          }
        }
        break;

      case LC_SEGMENT:
        segCmd = (struct segment_command *) loadCommand;
        if (AsciiStrCmp (segCmd->segname, kPrelinkTextSegment) == 0) {
          if (segCmd->vmsize > 0) {
            PrelinkTextAddr = (UINT32) (segCmd->vmaddr ? segCmd->vmaddr + KernelRelocBase : 0);
            PrelinkTextSize = (UINT32) segCmd->vmsize;
            PrelinkTextLoadCmdAddr = (UINT32) (UINTN) segCmd;
          }
        }
        if (AsciiStrCmp (segCmd->segname, kPrelinkInfoSegment) == 0) {
          sectionIndex = sizeof (struct segment_command);
          
          while(sectionIndex < segCmd->cmdsize) {
            sect = (struct section *) ((UINT8*)segCmd + sectionIndex);
            sectionIndex += sizeof (struct section);
            
            if(AsciiStrCmp (sect->sectname, kPrelinkInfoSection) == 0 &&
               AsciiStrCmp (sect->segname, kPrelinkInfoSegment) == 0) {
              if (sect->size > 0) { 
                PrelinkInfoLoadCmdAddr = (UINT32) (UINTN) sect;
                PrelinkInfoAddr = (UINT32) (sect->addr ? sect->addr + KernelRelocBase : 0);
                PrelinkInfoSize = (UINT32) sect->size;
              }
            }
          }
        }
        break;
        
      default:
        break;
    }
    binaryIndex += cmdsize;
  }
  
  return;
}

VOID
FindBootArgs (
  VOID
)
{
  UINT8           *ptr;
  UINT8           archMode;

  archMode = sizeof (UINTN) * 8;
  // start searching from 0x200000.
  ptr = (UINT8 *) (UINTN) 0x200000;
  
  while(TRUE) {
    // check bootargs for 10.7 and up
    bootArgs2 = (BootArgs2 *) ptr;
    
    if (bootArgs2->Version == 2 && bootArgs2->Revision == 0 &&
        bootArgs2->efiMode == archMode &&
        bootArgs2->kaddr == 0 && bootArgs2->ksize == 0 &&
        bootArgs2->efiSystemTable == 0) {
      // set vars
      dtRoot = (CHAR8 *) (UINTN) bootArgs2->deviceTreeP;
      KernelSlide = bootArgs2->kslide;

      // disable other pointer
      bootArgs1 = NULL;
      break;
    }
    
    // check bootargs for 10.4 - 10.6.x  
    bootArgs1 = (BootArgs1 *) ptr;
    
    if (bootArgs1->Version == 1 &&
        (bootArgs1->Revision == 6 || bootArgs1->Revision == 5 || bootArgs1->Revision == 4) &&
        bootArgs1->efiMode == archMode &&
        bootArgs1->kaddr == 0 && bootArgs1->ksize == 0 &&
        bootArgs1->efiSystemTable == 0) {
      // set vars
      dtRoot = (CHAR8 *) (UINTN) bootArgs1->deviceTreeP;

      // disable other pointer
      bootArgs2 = NULL;
      break;
    }
    
    ptr += 0x1000;
  }
}

VOID
KernelAndKextPatcherInit (
  VOID
)
{
  if (PatcherInited) {
    return;
  }
  
  // KernelRelocBase will normally be 0
  // but if OsxAptioFixDrv is used, then it will be > 0
  SetKernelRelocBase ();
  // Find bootArgs - we need then for proper detection
  // of kernel Mach-O header
  FindBootArgs ();

  if (bootArgs1 == NULL && bootArgs2 == NULL) {
    return;
  }
  
  // Find kernel Mach-O header:
  // for ML: bootArgs2->kslide + 0x00200000
  // for older versions: just 0x200000
  // for AptioFix booting - it's always at KernelRelocBase + 0x200000
  KernelData = (VOID*)(UINTN)(KernelSlide + KernelRelocBase + 0x00200000);
  
  // check that it is Mach-O header and detect architecture
  if (MACH_GET_MAGIC(KernelData) == MH_MAGIC || MACH_GET_MAGIC(KernelData) == MH_CIGAM) {
    is64BitKernel = FALSE;
  } else if (MACH_GET_MAGIC(KernelData) == MH_MAGIC_64 || MACH_GET_MAGIC(KernelData) == MH_CIGAM_64) {
    is64BitKernel = TRUE;
  } else {
    // not valid Mach-O header - exiting
    KernelData = NULL;
    return;
  }

  // find __PRELINK_TEXT and __PRELINK_INFO
  Get_PreLink ();
  isKernelcache = PrelinkTextSize > 0 && PrelinkInfoSize > 0;

  PatcherInited = TRUE;
}

VOID
KernelAndKextsPatcherStart (
  VOID
)
{
  // we will call KernelAndKextPatcherInit() only if needed
  if (gSettings.KPKernelCpu) {
    //
    // Kernel patches
    //
    if ((gCPUStructure.Family != 0x06 && AsciiStrStr(OSVersion,"10.7") != 0)||
        (gCPUStructure.Model == CPU_MODEL_ATOM &&
        ((AsciiStrStr (OSVersion, "10.7") != 0) || AsciiStrStr (OSVersion, "10.6") != 0)) ||
        (gCPUStructure.Model == CPU_MODEL_IVY_BRIDGE && AsciiStrStr (OSVersion, "10.7") != 0) ||
        (gCPUStructure.Model == CPU_MODEL_IVY_BRIDGE_E5 && AsciiStrStr (OSVersion, "10.7") != 0)) {
      KernelAndKextPatcherInit ();
      if (KernelData == NULL) {
        return;
      }

      if(is64BitKernel) {
        KernelPatcher_64 (KernelData);
      } else {
        KernelPatcher_32 (KernelData);
      }
    }
  }
  //
  // Kext patches
  //
  if (gSettings.KPKextPatchesNeeded) {
    KernelAndKextPatcherInit ();
    if (KernelData == NULL) {
      return;
    }
    KextPatcherStart ();
  }
#if 0
  //
  // Kext add
  //
  if (AsciiStrStr(gSettings.BootArgs, "WithKexts") == NULL)
  {
    UINT32      deviceTreeP;
    UINT32      deviceTreeLength;
    EFI_STATUS  Status;

    KernelAndKextPatcherInit ();
    if (KernelData == NULL) {
      return;
    }

    if (bootArgs1 != NULL) {
        deviceTreeP = bootArgs1->deviceTreeP;
        deviceTreeLength = bootArgs1->deviceTreeLength;
    } else if (bootArgs2 != NULL) {
        deviceTreeP = bootArgs2->deviceTreeP;
        deviceTreeLength = bootArgs2->deviceTreeLength;
    } else return;

    Status = InjectKexts (deviceTreeP, &deviceTreeLength);

    if (!EFI_ERROR(Status)) KernelBooterExtensionsPatch (KernelData);
  }
#endif
}