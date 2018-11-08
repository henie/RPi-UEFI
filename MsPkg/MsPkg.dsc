## @file
#  A package that contains generic headers and components.
#
#  Copyright (c) Microsoft Corporation. All rights reserved.
#
#   This program and the accompanying materials
#   are licensed and made available under the terms and conditions of the BSD License
#   which accompanies this distribution. The full text of the license may be found at
#   http://opensource.org/licenses/bsd-license.
#
#   THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
#   WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
##

[Defines]
  PLATFORM_NAME                  = MsPkg
  PLATFORM_GUID                  = 2F78367E-9C74-4FBE-82E7-1D2DAAF18CC6
  PLATFORM_VERSION               = 0.01
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/MsPkg
  SUPPORTED_ARCHITECTURES        = ARM|AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

[PcdsFeatureFlag]

[PcdsFixedAtBuild]

[LibraryClasses]

[Components]
  MsPkg/Drivers/SdMmcDxe/SdMmcDxe.inf
  MsPkg/Application/EdkTestSample/EdkTestSample.inf
  MsPkg/Application/EdkNoStdTestSample/EdkNoStdTestSample.inf
  MsPkg/Application/RpmbClientTest/RpmbClientTest.inf
  MsPkg/Application/RpmbResetTest/RpmbResetTest.inf

