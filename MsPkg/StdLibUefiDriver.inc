################################################################
# Boilerplate text to be included by any DSC file using the
# Standard Libraries.
#
# The including DSC file must DEFINE the EMULATE macro if
# the application is to be run in an emulation environment.
#
#  Copyright (c) 2016, Daryl McDaniel. All rights reserved.<BR>
#  Copyright (c) 2011 - 2014, Intel Corporation. All rights reserved.<BR>
#  This program and the accompanying materials
#  are licensed and made available under the terms and conditions of the BSD License
#  which accompanies this distribution. The full text of the license may be found at
#  http://opensource.org/licenses/bsd-license.
#
#  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
#  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
################################################################


[LibraryClasses.Common.UEFI_DRIVER]
  ShellCEntryLib|MsPkg/Library/NullCEntryLib/NullCEntryLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf

  #
  # C Standard Libraries
  #
  LibC|StdLib/LibC/LibC.inf
  LibCType|StdLib/LibC/Ctype/Ctype.inf
  LibLocale|StdLib/LibC/Locale/Locale.inf
  LibMath|StdLib/LibC/Math/Math.inf
  LibSignal|StdLib/LibC/Signal/Signal.inf
  LibStdio|StdLib/LibC/Stdio/Stdio.inf
  LibStdLib|StdLib/LibC/StdLib/StdLibUefiDriver.inf
  LibString|StdLib/LibC/String/String.inf
  LibTime|StdLib/LibC/Time/Time.inf
  LibUefi|StdLib/LibC/Uefi/UefiDriver.inf
  LibWchar|StdLib/LibC/Wchar/Wchar.inf

  LibGen|StdLib/PosixLib/Gen/LibGen.inf
  LibIIO|StdLib/LibC/Uefi/InteractiveIO/IIO.inf

# Additional, non-standard, libraries
  LibContainer|StdLib/LibC/Containers/ContainerLib.inf

# Libraries for device abstractions within the Standard C Library
# Applications should not directly access any functions defined in these libraries.
  LibGdtoa|StdLib/LibC/gdtoa/gdtoa.inf
  DevConsole|StdLib/LibC/Uefi/Devices/daConsole.inf
  DevUtility|StdLib/LibC/Uefi/Devices/daUtility.inf