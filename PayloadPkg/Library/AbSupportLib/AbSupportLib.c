/** @file
  Payload implements one instance of Platform Hook Library.

  Copyright (c) 2015 - 2019, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include   "AbSupport.h"


/**
  Check if slot is bootable.

  @param[in]  SlotData        Pointer to slot data

  @retval  TRUE               This slot is bootable
  @retval  FALSE              This slot is not bootable
**/
BOOLEAN
SlotBootable (
  IN  AB_SLOT_DATA           *SlotData
  )
{
  if ((SlotData->Priority > 0) && ((SlotData->SuccessBoot > 0) || (SlotData->TriesRemaining > 0))) {
    return TRUE;
  }
  return FALSE;
}

/**
  Check which slot to boot.

  @param[in]  AbBootInfo     Pointer to AB boot info data

  @retval   0                Select first slot to boot
  @retval   1                Select second slot to boot
  @retval  -1                Magic doesn't match
  @retval  -2                SlotData CRC error
  @retval  -3                Both SlotA and SlotB are not bootable
**/
INT32
ParseBootSlot (
  IN AB_BOOT_INFO            *AbBootInfo
  )
{
  EFI_STATUS                 Status;
  UINT32                     CrcOut;
  UINTN                      DataSize;

  if (AbBootInfo->Magic != AB_MAGIC_SIGNATURE) {
    DEBUG ((DEBUG_INFO, "AB magic error: 0x%x\n", AbBootInfo->Magic));
    return -1;
  }

  DataSize = sizeof (AB_BOOT_INFO) - sizeof (UINT32);
  Status = CalculateCrc32WithType ((UINT8 *)AbBootInfo, DataSize, Crc32TypeDefault, &CrcOut);
  if (EFI_ERROR (Status) || (CrcOut != AbBootInfo->Crc32)) {
    DEBUG ((DEBUG_INFO, "BootSlot CRC error: 0x%x !=0x%x\n", CrcOut, AbBootInfo->Crc32));
    return -2;
  }

  if (SlotBootable (&AbBootInfo->SlotData[0])) {
    if (SlotBootable (&AbBootInfo->SlotData[1])) {
      if (AbBootInfo->SlotData[1].Priority > AbBootInfo->SlotData[0].Priority) {
        return 1;
      }
    }
    return 0;
  } else if (SlotBootable (&AbBootInfo->SlotData[1])) {
    return 1;
  }

  return -3;
}


/**
  Load AB boot information from misc partition

  This function will read AB boot information from boot device.

  @param[in]  BootOption      Image information where to load image.
  @param[in]  HwPartHandle    The hardware partition handle
  @param[out] AbBootInfo      AB boot info loaded from misc partition.

  @retval  RETURN_SUCCESS     Read AB boot information successfully
  @retval  Others             There is error when read boot info
**/
EFI_STATUS
LoadMisc (
  IN  OS_BOOT_OPTION         *BootOption,
  IN  EFI_HANDLE             HwPartHandle,
  OUT AB_BOOT_INFO           *AbBootInfo
  )
{
  EFI_STATUS                 Status;
  MISC_PARTITION_DATA        *MiscPartitionData;
  DEVICE_BLOCK_INFO          BlockInfo;
  VOID                       *Buffer;
  LOGICAL_BLOCK_DEVICE       LogicBlkDev;
  UINTN                      ReadSize;
  UINT32                     BlockSize;
  LBA_IMAGE_LOCATION         *LbaImage;

  LbaImage = &BootOption->Image[LoadImageTypeMisc].LbaImage;
  Status = GetLogicalPartitionInfo (LbaImage->SwPart, HwPartHandle, &LogicBlkDev);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "Get logical partition error, Status = %r\n", Status));
    return Status;
  }

  Status = MediaGetMediaInfo (BootOption->HwPart, &BlockInfo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GetMediaInfo Error %r\n", Status));
    return Status;
  }

  //
  // Data is in raw block IO partition
  // Read size should be block aligned in bytes.
  //
  BlockSize = BlockInfo.BlockSize;
  ReadSize  = ((sizeof (MISC_PARTITION_DATA) % BlockSize) == 0) ? \
              sizeof (MISC_PARTITION_DATA) : \
              ((sizeof (MISC_PARTITION_DATA) / BlockSize) + 1) * BlockSize;

  Buffer = (UINT8 *) AllocatePages (EFI_SIZE_TO_PAGES (ReadSize));
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = MediaReadBlocks (
             BootOption->HwPart,
             LogicBlkDev.StartBlock + LbaImage->LbaAddr,
             ReadSize,
             Buffer
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "Read Mics error, Status = %r\n", Status));
  } else {
    MiscPartitionData = (MISC_PARTITION_DATA *)Buffer;
    CopyMem (AbBootInfo, &MiscPartitionData->AbBootInfo, sizeof (AB_BOOT_INFO));
  }

  //
  // Free temporary memory
  //
  if (Buffer != NULL) {
    FreePages (Buffer, EFI_SIZE_TO_PAGES (ReadSize));
  }

  return Status;
}

INT32
ParseMenderConfig(IN MENDER_ENV_CONFIG *MenderEnvConfig) {
  INT32 BootSlot;
  INT32 Index;

  if (MenderEnvConfig[0].Editing == 0 &&
      ((MenderEnvConfig[0].ConfigInvalid & MENDER_GRUBENV_1_INVALID) == 0)) {
    Index = 0;
  } else if (MenderEnvConfig[1].Editing == 0 &&
             ((MenderEnvConfig[1].ConfigInvalid & MENDER_GRUBENV_2_INVALID) ==
              0)) {
    Index = 1;
  } else {
    //
    // Both 'editing' bits are non-zero. It means files are corrupted.
    // Set -1 to reject this boot.
    //
    DEBUG((DEBUG_ERROR, "Mender: grub-mender-grubenv files are corrupted\n"));
    return -1;
  }

  BootSlot = MenderEnvConfig[Index].MenderBootPart;
  if (MenderEnvConfig[Index].UpgradeAvailable == MenderUpgradeAvailable) {
    if (MenderEnvConfig[Index].BootCount != 0) {
      ShellPrint(L"Rolling back...\n");
      if (MenderEnvConfig[Index].MenderBootPart == MenderRootfsPartA) {
        BootSlot = MenderRootfsPartB;
      } else {
        BootSlot = MenderRootfsPartA;
      }
    } else {
      ShellPrint(L"Booting new update...\n");
    }
  }

  return BootSlot;
}

EFI_STATUS
LoadMenderConfig(IN VOID *ConfigFile, IN UINT32 DirIdx,
                 OUT MENDER_ENV_CONFIG *MenderEnvConfig) {
  CHAR8 *CurrLine;
  CHAR8 *NextLine;
  CHAR8 *LineCursor;
  UINT32 LineLen;
  UINT8 BootCount;
  MENDER_ROOTFS_PART RootfsPart;
  MENDER_UPGRADE_AVAIL UpgradeAvail;
  EFI_STATUS Status;

  Status = EFI_SUCCESS;
  CurrLine = ConfigFile;
  while (CurrLine != NULL) {
    NextLine = GetNextLine(CurrLine, &LineLen);
    LineCursor = CurrLine;
    while ((LineCursor[0] != '=') && (LineCursor[0] != '\n')) {
      LineCursor++;
    }
    if (LineCursor[0] == '=') {
      LineCursor++;
      if (MatchAssignment(CurrLine, "bootcount") > 0) {
        BootCount = (UINT32)AsciiStrDecimalToUintn(LineCursor);
        if (BootCount != 0 && BootCount != 1) {
          MenderEnvConfig[DirIdx].ConfigInvalid |= (1 << DirIdx);
          DEBUG((DEBUG_ERROR, "Mender: Variable 'bootcount' invalid\n"));
          Status = EFI_INVALID_PARAMETER;
          goto Done;
        }
        MenderEnvConfig[DirIdx].BootCount = BootCount;
      } else if (MatchAssignment(CurrLine, "mender_boot_part") > 0) {
        RootfsPart = (UINT32)AsciiStrDecimalToUintn(LineCursor);
        if (RootfsPart != MenderRootfsPartA &&
            RootfsPart != MenderRootfsPartB) {
          MenderEnvConfig[DirIdx].ConfigInvalid |= (1 << DirIdx);
          DEBUG((DEBUG_ERROR, "Mender: Variable 'mender_boot_part' invalid\n"));
          Status = EFI_INVALID_PARAMETER;
          goto Done;
        }
        MenderEnvConfig[DirIdx].MenderBootPart = RootfsPart;
      } else if (MatchAssignment(CurrLine, "upgrade_available") > 0) {
        UpgradeAvail = (UINT32)AsciiStrDecimalToUintn(LineCursor);
        if (UpgradeAvail != 0 && UpgradeAvail != 1) {
          MenderEnvConfig[DirIdx].ConfigInvalid |= (1 << DirIdx);
          Status = EFI_INVALID_PARAMETER;
          goto Done;
        }
        MenderEnvConfig[DirIdx].UpgradeAvailable = UpgradeAvail;
      } else if (MatchAssignment(CurrLine, "editing") > 0) {
        MenderEnvConfig[DirIdx].Editing =
            (UINT32)AsciiStrDecimalToUintn(LineCursor);
      } else {
        if ((MatchAssignment(CurrLine, "mender_boot_part_hex") > 0) ||
            (MatchAssignment(CurrLine, "mender_uboot_separator") > 0) ||
            (MatchAssignment(CurrLine, "systemd_machine_id") > 0) ||
            (MatchAssignment(CurrLine, "mender_systemd_machine_id") > 0)) {
          DEBUG(
              (DEBUG_INFO, "Mender: grub-mender-grubenv irrelevant config\n"));
        } else {
          DEBUG((DEBUG_ERROR,
                 "Mender: grub-mender-grubenv config not recognized\n"));
          Status = EFI_INVALID_PARAMETER;
        }
      }
    }
    CurrLine = NextLine;
  }

Done:
  return Status;
}

EFI_STATUS
LoadMender(IN OS_BOOT_OPTION *BootOption, IN EFI_HANDLE HwPartHandle,
           OUT MENDER_ENV_CONFIG *MenderEnvConfig) {
  EFI_STATUS Status;
  UINT8 SwPart;
  UINT8 FsType;
  EFI_HANDLE FsHandle;
  EFI_HANDLE FileHandle;
  VOID *ConfigFile;
  UINTN ConfigFileSize;
  UINT32 Index;

  FsHandle = NULL;
  FileHandle = NULL;
  ConfigFile = NULL;
  ConfigFileSize = 0;
  Status = RETURN_NOT_FOUND;

  // Mender has ESP partition number 0.
  SwPart = 0;
  // Mender ESP partition has FAT filesystem
  FsType = EnumFileSystemTypeFat;

  Status = InitFileSystem(SwPart, FsType, HwPartHandle, &FsHandle);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,
           "Mender: Init file system failed on SwPart %u, Status = %r\n",
           SwPart, Status));
    goto Done;
  }

  for (Index = 0; Index < sizeof(mMenderConfigFile) / sizeof(CHAR16 *);
       Index++) {
    DEBUG((DEBUG_INFO, "Checking %s\n", mMenderConfigFile[Index]));
    Status =
        OpenFile(FsHandle, (CHAR16 *)mMenderConfigFile[Index], &FileHandle);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "Open file '%a' failed, Status = %r\n",
             mMenderConfigFile[Index], Status));
      goto Done;
    }

    Status = GetFileSize(FileHandle, &ConfigFileSize);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "Get file size failed, Status = %r\n", Status));
      goto Done;
    }
    DEBUG((DEBUG_INFO, "File '%s' size %d\n", mMenderConfigFile[Index],
           ConfigFileSize));

    if (ConfigFileSize == 0 || ConfigFileSize > MENDER_GRUBENV_MAX_SIZE) {
      Status = EFI_LOAD_ERROR;
      goto Done;
    }

    ConfigFile = AllocatePool(ConfigFileSize + 1);
    if (ConfigFile == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }

    Status = ReadFile(FileHandle, ConfigFile, &ConfigFileSize);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (DEBUG_ERROR, "Failed to load file %s\n", mMenderConfigFile[Index]));
      goto Done;
    }
    DEBUG((DEBUG_INFO, "Load file %s [size 0x%x]: %r\n",
           mMenderConfigFile[Index], ConfigFileSize, Status));

    Status = LoadMenderConfig(ConfigFile, Index / 2, MenderEnvConfig);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "Failed to load Mender configuration from %s\n",
             mMenderConfigFile[Index]));
      ConfigFile = NULL;
      goto Done;
    }

    CloseFile(FileHandle);
    FileHandle = NULL;
    FreePool(ConfigFile);
    ConfigFile = NULL;
  }

Done:
  if (ConfigFile != NULL) {
    FreePool(ConfigFile);
    ConfigFile = NULL;
  }
  if (FileHandle != NULL) {
    CloseFile(FileHandle);
  }
  if (FsHandle != NULL) {
    CloseFileSystem(FsHandle);
  }

  return Status;
}

/**
  Check which slot to boot.

  @param[in]  BootOption      Image information where to load image.
  @param[in]  HwPartHandle    The hardware partition handle

  @retval  0                  Select first slot to boot
  @retval  1                  Select second slot to boot
**/
INT32
EFIAPI
GetBootSlot (
  IN OS_BOOT_OPTION          *BootOption,
  IN EFI_HANDLE              HwPartHandle
  )
{
  EFI_STATUS                 Status;
  AB_BOOT_INFO               AbBootInfo;
  MENDER_ENV_CONFIG MenderEnvConfig[2];
  INT32                      BootSlot;

  BootSlot = 0;

  if ((BootOption->BootFlags & BOOT_FLAGS_MISC) != 0) {
    Status   = LoadMisc (BootOption, HwPartHandle, &AbBootInfo);
    if (!EFI_ERROR (Status)) {
      BootSlot = ParseBootSlot (&AbBootInfo);
      if (BootSlot < 0) {
        DEBUG ((DEBUG_ERROR, "ERROR: boot slot error (%d)\n", BootSlot));
        BootSlot = 0;
      }
    } else {
      DEBUG ((DEBUG_ERROR, "LoadMisc Status = %r\n", Status));
    }
  } else if ((BootOption->BootFlags & BOOT_FLAGS_MENDER) != 0) {
    ZeroMem(MenderEnvConfig, sizeof(MenderEnvConfig));
    Status = LoadMender(BootOption, HwPartHandle, MenderEnvConfig);
    if (!EFI_ERROR(Status)) {
      BootSlot = ParseMenderConfig(MenderEnvConfig);
      if (BootSlot != MenderRootfsPartA && BootSlot != MenderRootfsPartB) {
        DEBUG((DEBUG_ERROR, "ERROR: boot slot error (%d)\n", BootSlot));
      }
      BootSlot -= 2;
    } else {
      DEBUG((DEBUG_ERROR, "ERROR: Load Mender Configuration failed\n"));
    }
  }

  return BootSlot;
}

