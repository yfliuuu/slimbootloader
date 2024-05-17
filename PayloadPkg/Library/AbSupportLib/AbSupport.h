/** @file
  Payload implements one instance of Paltform Hook Library.

  Copyright (c) 2015 - 2023, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiPei.h>
#include <Library/DebugLib.h>
#include <Library/Crc32Lib.h>
#include <Library/BootloaderCommonLib.h>
#include <Library/DebugLib.h>
#include <Library/PartitionLib.h>
#include <Library/MediaAccessLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FileSystemLib.h>
#include <Library/StringSupportLib.h>
#include <Library/ShellLib.h>
#include <Guid/OsBootOptionGuid.h>
#include <BlockDevice.h>

// Magic for the A/B struct, 0x42414342
#define AB_MAGIC_SIGNATURE       SIGNATURE_32('B', 'C', 'A', 'B')

#define MENDER_GRUBENV_MAX_SIZE  1024

CONST CHAR16  *mMenderConfigFile[4] = {
  L"grub-mender-grubenv/mender_grubenv1/env",
  L"grub-mender-grubenv/mender_grubenv1/lock",
  L"grub-mender-grubenv/mender_grubenv2/env",
  L"grub-mender-grubenv/mender_grubenv2/lock"
};

#define MENDER_GRUBENV_1_INVALID     BIT0
#define MENDER_GRUBENV_2_INVALID     BIT1

#pragma pack(1)

typedef struct {
  // Slot priority with 15 meaning highest priority, 1 lowest
  // priority and 0 the slot is unbootable.
  UINT8                   Priority:4;
  // Number of times left attempting to boot this slot.
  UINT8                   TriesRemaining:3;
  // 1 if this slot has booted successfully, 0 otherwise.
  UINT8                   SuccessBoot:1;
  // 1 if this slot is corrupted from a dm-verity corruption, 0
  // otherwise.
  UINT8                   VerityCorrupted:1;
  // Reserved for further use.
  UINT8                   Reserved:7;
} AB_SLOT_DATA;

typedef struct {
  // NUL terminated active slot suffix.
  UINT8                   SlotSuffix[4];
  // Bootloader Control AB magic number (see AB_MAGIC_SIGNATURE).
  UINT32                  Magic;
  // Version of struct being used (see AB_VERSION).
  UINT8                   Major;
  // Number of slots being managed.
  UINT8                   NbSlot:3;
  // Number of times left attempting to boot recovery.
  UINT8                   RecoveryTriesRemaining:3;
  // Status of any pending snapshot merge of dynamic partitions.
  UINT8                   MergeStatus:3;
  // Ensure 4-bytes alignment for slot_info field.
  UINT8                   Reserved1[1];
  // Per-slot information.  Up to 4 slots.
  AB_SLOT_DATA            SlotData[4];
  // Reserved for further use.
  UINT8                   Reserved2[8];
  // CRC32 of all 28 bytes preceding this field (little endian
  // format).
  UINT32                  Crc32;
} AB_BOOT_INFO;

typedef struct {
  UINT8                   LegacyData[2048];
  AB_BOOT_INFO            AbBootInfo;
} MISC_PARTITION_DATA;

//
// Mender partition layout:
// Part 1: boot
// Part 2: rootfs A
// Part 3: rootfs B
// Part 4: data
// Part...
//
typedef enum {
  MenderRootfsPartA = 2,
  MenderRootfsPartB = 3,
  MenderRootfsPartMAX
} MENDER_ROOTFS_PART;

typedef enum {
  MenderUpgradeNotAvail = 0,
  MenderUpgradeAvailable = 1,
  MenderUpgradeMax
} MENDER_UPGRADE_AVAIL;

// SBL is not capable of writing the 'editing' bit.
// This is just a placeholder for this env variable.
typedef enum {
  MenderEditInvalid = 0,
  MenderEditValid = 1,
  MenderEditMax
} MENDER_EDIT_LOCK;

typedef struct {
  UINT8                   BootCount;
  MENDER_ROOTFS_PART      MenderBootPart;
  MENDER_UPGRADE_AVAIL    UpgradeAvailable;
  MENDER_EDIT_LOCK        Editing;
  UINT8                   ConfigInvalid;
} MENDER_ENV_CONFIG;

#pragma pack()
