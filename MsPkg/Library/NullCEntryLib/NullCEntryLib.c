/** @file
  Provides a null implementation for shell application CEntryPoint.
  This is mainly consumed by drivers using subset of the C run-time.

  Copyright (c) 2009 - 2016, Intel Corporation. All rights reserved.<BR>
  Copyright (c) Microsoft Corporation. All rights reserved. <BR>

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Base.h>
#include <Library/DebugLib.h>

/**
  UEFI entry point for an application that will in turn call the
  ShellAppMain function which has parameters similar to a standard C
  main function. In our case we do nothing since Shell support is not required.

  @param  ImageHandle  The image handle of the UEFI Application.
  @param  SystemTable  A pointer to the EFI System Table.

  @retval  EFI_SUCCESS               The application exited normally.
  @retval  Other                     An error occurred.

**/
EFI_STATUS
EFIAPI
ShellCEntryLib (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  (void) ImageHandle;
  (void) SystemTable;

  return EFI_SUCCESS;
}
