/** @file
*
*  Copyright (c) Microsoft Corporation. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <setjmp.h>
#include <time.h>

#include <Uefi.h>
#include <Library/UefiLib.h>

#include "EdkTest.h"

MODULE ("Sample demonstrating EdkTest usage");

INT32
BuggyAdder (
  INT32 Left,
  INT32 Right
  )
{
  return Left | Right;
}

VOID
TestAdderTrivial (
  VOID
  )
{
  VERIFY_ARE_EQUAL (INT32, 3, BuggyAdder(1, 2), "Verify trivial adder case");
}

VOID
TestAdderTricky (
  VOID
  )
{
  VERIFY_ARE_EQUAL (INT32, 2, BuggyAdder(1, 1), "Verify tricky adder case");
}

BOOLEAN
TestModuleSetup (
  VOID
  )
{
  LOG_COMMENT ("Setting up some stuff\n");
  return TRUE;
}

VOID
TestModuleCleanup (
  VOID
  )
{
  LOG_COMMENT ("Cleaning up the mess\n");
}

int
main (
  IN int    argc,
  IN char   **argv
  )
{
  MODULE_SETUP (TestModuleSetup);
  MODULE_CLEANUP (TestModuleCleanup);
  TEST_FUNC (TestAdderTrivial);
  TEST_FUNC (TestAdderTricky);

  if (!RUN_MODULE(argc, argv)) {
    return 1;
  }

  return 0;
}
