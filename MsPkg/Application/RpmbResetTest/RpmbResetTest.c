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
#include <Library/DebugLib.h> // ASSERT
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/TimerLib.h>
#include <Library/MemoryAllocationLib.h>

#include <Protocol/RpmbIo.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "EdkTest.h"

MODULE ("RPMB Reset Test");

EFI_RPMB_IO_PROTOCOL *mRpmbProtocol = NULL;
UINT8 *mBuffer = NULL;
UINT8 *mIntermediateBuffer = NULL;

// RPMB Test Key used for Test and Development.
static UINT8 mRpmbAuthKey[EFI_RPMB_PACKET_KEY_MAC_SIZE] = {
  0xD3, 0xEB, 0x3E, 0xC3, 0x6E, 0x33, 0x4C, 0x9F,
  0x98, 0x8C, 0xE2, 0xC0, 0xB8, 0x59, 0x54, 0x61,
  0x0D, 0x2B, 0xCF, 0x86, 0x64, 0x84, 0x4D, 0xF2,
  0xAB, 0x56, 0xE6, 0xC6, 0x1B, 0xB7, 0x01, 0xE4
};

UINT32
RpmbReadCounterValue (
  VOID
  );

VOID
HexDump (
  IN CONST UINT8  *Buffer,
  IN UINTN        Size
  )
{
  UINTN i;

  for (i = 0; i < Size; i++) {
    if ((i != 0) && (i % 16) == 0) {
      LOG_COMMENT ("\n");
    }
    LOG_COMMENT ("%02x ", Buffer[i]);
  }
  LOG_COMMENT ("\n");
}

VOID
DumpPacket (
  IN EFI_RPMB_DATA_PACKET   *Packet
  )
{
  LOG_COMMENT ("Key/MAC:\n");
  HexDump (Packet->KeyOrMAC, EFI_RPMB_PACKET_KEY_MAC_SIZE);
  LOG_COMMENT ("Data:\n");
  HexDump (Packet->PacketData, EFI_RPMB_PACKET_DATA_SIZE);
  LOG_COMMENT ("Write Counter: ");
  HexDump (Packet->WriteCounter, EFI_RPMB_PACKET_WCOUNTER_SIZE);
  LOG_COMMENT ("Address: ");
  HexDump (Packet->Address, EFI_RPMB_PACKET_ADDRESS_SIZE);
  LOG_COMMENT ("Block Count: ");
  HexDump (Packet->BlockCount, EFI_RPMB_PACKET_BLOCKCOUNT_SIZE);
  LOG_COMMENT ("Result: ");
  HexDump (Packet->OperationResult, EFI_RPMB_PACKET_RESULT_SIZE);
  LOG_COMMENT ("Req/Res Type: ");
  HexDump (Packet->RequestOrResponseType, EFI_RPMB_PACKET_TYPE_SIZE);
}

VOID
RandomBytes (
  IN UINTN    BufferSize,
  OUT UINT8   *Buffer
  )
{
  UINTN Index;

  ASSERT (Buffer != NULL);

  for (Index = 0; Index < BufferSize; ++Index) {
    // Generate random number between 0 and 255 inclusive
    Buffer[Index] = (UINT8) (rand() % 256);
  }
}

BOOLEAN
AreEqualBytes (
  IN CONST UINT8   *LeftBuffer,
  IN CONST UINT8   *RightBuffer,
  IN UINTN          BufferSize
  )
{
  UINTN Index;

  ASSERT (LeftBuffer != NULL);
  ASSERT (RightBuffer != NULL);

  for (Index = 0; Index < BufferSize; ++Index) {
    if (LeftBuffer[Index] != RightBuffer[Index]) {
      return FALSE;
    }
  }

  return TRUE;
}

VOID
VerifyAreEqualBytes (
  IN CONST UINT8   *LeftBuffer,
  IN CONST UINT8   *RightBuffer,
  IN UINTN          BufferSize
  )
{
  UINTN Index;

  ASSERT (LeftBuffer != NULL);
  ASSERT (RightBuffer != NULL);

  for (Index = 0; Index < BufferSize; ++Index) {
    if (*LeftBuffer++ != *RightBuffer++) {
      VERIFY_IS_TRUE (
        FALSE,
        "Buffers don't match at byte 0x%p. (Left Byte = %02x, Right Byte = %02x)",
        Index,
        *LeftBuffer,
        *RightBuffer);
    }
  }
}

VOID
ReverseBuffer (
  IN UINT8    *SrcBuffer,
  IN UINTN    DstBufferSize,
  OUT UINT8   *DstBuffer
  )
{
  UINTN Index;

  ASSERT (SrcBuffer != NULL);
  ASSERT (DstBuffer != NULL);

  for (Index = 0; Index < DstBufferSize; ++Index) {
    DstBuffer[DstBufferSize - 1 - Index] = SrcBuffer[Index];
  }
}

// JEDEC Standard No. 84-A441:
// Byte order of the RPMB data frame is MSB first, e.g. Write Counter MSB [11]
// is storing the upmost byte of the counter value.

VOID
Uint16ToRpmbBytes (
  IN UINT16   Value,
  OUT UINT8   *RpmbBytes
  )
{
  ASSERT (RpmbBytes != NULL);

  RpmbBytes[0] = (UINT8) (Value >> 8);
  RpmbBytes[1] = (UINT8) (Value & 0xF);
}


UINT16
RpmbBytesToUint16 (
  IN CONST UINT8  *RpmbBytes
  )
{
  ASSERT (RpmbBytes != NULL);

  return ((UINT16) RpmbBytes[0] << 8) | ((UINT16) RpmbBytes[1]);
}

VOID
HashPacket (
  IN EFI_RPMB_DATA_PACKET   *Packet,
  OUT UINT8                 *Hash
  )
{
  UINTN HmacLen;
  UINT8 *Temp;

  ASSERT (Packet != NULL);
  ASSERT (Hash != NULL);

  HmacLen = EFI_RPMB_PACKET_KEY_MAC_SIZE;
  Temp = HMAC (
            EVP_sha256 (),
            (VOID *) mRpmbAuthKey,
            sizeof (mRpmbAuthKey),
            (UINT8 *) Packet->PacketData,
            EFI_RPMB_PACKET_DATA_HASH_SIZE,
            Hash,
            &HmacLen);

  VERIFY_ARE_EQUAL(PVOID, Temp, Hash);
}

VOID
GeneratePacket (
  IN UINT16                 RequestOrResponseType,
  OUT EFI_RPMB_DATA_PACKET  *Packet
  )
{
  ASSERT (Packet != NULL);

  memset (Packet, 0, sizeof (*Packet));

  Uint16ToRpmbBytes (RequestOrResponseType, Packet->RequestOrResponseType);

  if (RequestOrResponseType == EFI_RPMB_REQUEST_PROGRAM_KEY) {
    memcpy (Packet->KeyOrMAC, mRpmbAuthKey, sizeof (mRpmbAuthKey));
  }
}

VOID
GenerateDataPacket (
  IN UINT8                  *Data,
  IN UINTN                  DataSize,
  IN UINT16                 Address,
  IN UINT16                 BlockCount,
  IN UINT16                 RequestOrResponseType,
  OUT EFI_RPMB_DATA_PACKET  *Packet
  )
{
  UINT32 CounterValue;

  ASSERT (Packet != NULL);

  memset (Packet, 0, sizeof (*Packet));

  Uint16ToRpmbBytes (RequestOrResponseType, Packet->RequestOrResponseType);
  Uint16ToRpmbBytes (BlockCount, Packet->BlockCount);
  Uint16ToRpmbBytes (Address, Packet->Address);

  if (RequestOrResponseType == EFI_RPMB_REQUEST_AUTH_WRITE) {
    CounterValue = RpmbReadCounterValue ();
    ReverseBuffer (
      (UINT8 *) &CounterValue,
      sizeof (Packet->WriteCounter),
      Packet->WriteCounter);

    ASSERT (Data != NULL);
    memcpy (Packet->PacketData, Data, DataSize);

    HashPacket (Packet, Packet->KeyOrMAC);
  }
}

VOID
RpmbVerifyResponseStatus (
  IN EFI_RPMB_DATA_PACKET   *Packet,
  IN UINT16                 ExpectedResponseType,
  IN UINT16                 ExpectedOperationResult
  )
{
  UINT16 OperationResult;
  UINT16 ResponseType;

  ASSERT (Packet != NULL);

  ResponseType = RpmbBytesToUint16 (Packet->RequestOrResponseType);

  VERIFY_ARE_EQUAL (
    UINT16,
    ExpectedResponseType,
    ResponseType,
    "Verify response type");

  OperationResult = RpmbBytesToUint16 (Packet->OperationResult);

  if (OperationResult & EFI_RPMB_ERROR_CNT_EXPIRED_BIT) {
    OperationResult &= ~EFI_RPMB_ERROR_CNT_EXPIRED_BIT;
    LOG_COMMENT ("*** Warning: Write counter has expired! ***");
  }

  LOG_COMMENT (
    "OperationResult: %s (0x%X)\n",
    RpmbOperationResultToString (OperationResult),
    OperationResult);

  VERIFY_ARE_EQUAL (
    UINT16,
    ExpectedOperationResult,
    OperationResult,
    "Verify operation result");
}

UINT32
RpmbReadCounterValue (
  VOID
  )
{
  UINT32 CounterValue;
  EFI_RPMB_DATA_PACKET Request;
  EFI_RPMB_DATA_PACKET ResultResponse;

  GeneratePacket (EFI_RPMB_REQUEST_COUNTER_VALUE, &Request);
  GeneratePacket (0, &ResultResponse);

  VERIFY_SUCCEEDED (
    mRpmbProtocol->ReadCounter (
      mRpmbProtocol,
      &Request,
      &ResultResponse),
    "Verify reading the write counter value");

  ReverseBuffer (
    ResultResponse.WriteCounter,
    sizeof (CounterValue),
    (UINT8 *) &CounterValue);

  RpmbVerifyResponseStatus (
    &ResultResponse,
    EFI_RPMB_RESPONSE_COUNTER_VALUE,
    EFI_RPMB_OK);

  LOG_COMMENT ("Retrieved Write Counter (CounterValue = 0x%X)\n", CounterValue);

  return CounterValue;
}

UINT64
RpmbRead (
  IN UINTN    Address,
  IN UINTN    BufferSize,
  OUT UINT8   *Buffer
  )
{
  EFI_RPMB_DATA_PACKET ReadRequest;
  EFI_RPMB_DATA_BUFFER ReadResponseBuffer;
  UINT64 TimerStart;
  UINT64 ElapsedMs;
  UINT16 PacketCount;
  UINTN Index;

  GenerateDataPacket (
    NULL,
    0,
    Address,
    0,
    EFI_RPMB_REQUEST_AUTH_READ,
    &ReadRequest);

  ASSERT (BufferSize % EFI_RPMB_PACKET_DATA_SIZE == 0);
  PacketCount = BufferSize / EFI_RPMB_PACKET_DATA_SIZE;
  mIntermediateBuffer = AllocateZeroPool (
                          (UINTN)PacketCount * sizeof (EFI_RPMB_DATA_PACKET));

  ReadResponseBuffer.PacketCount = PacketCount;
  ReadResponseBuffer.Packets = (EFI_RPMB_DATA_PACKET *) mIntermediateBuffer;

  LOG_COMMENT ("Read (BufferSize: 0x%p, PacketCount: %d)\n", BufferSize, (UINT32) PacketCount);

  TimerStart = HpcTimerStart ();

  VERIFY_SUCCEEDED (
    mRpmbProtocol->AuthenticatedRead (
      mRpmbProtocol,
      &ReadRequest,
      &ReadResponseBuffer),
    "Verify authenticated data read");

  ElapsedMs = HpcTimerElapsedMilliseconds (TimerStart);

  //
  // Copy the buffer back from the packets
  //
  for (Index = 0; Index < PacketCount; ++Index) {
    memcpy (Buffer, ReadResponseBuffer.Packets[Index].PacketData, EFI_RPMB_PACKET_DATA_SIZE);
    Buffer += EFI_RPMB_PACKET_DATA_SIZE;
  }

  RpmbVerifyResponseStatus (
    ReadResponseBuffer.Packets,
    EFI_RPMB_RESPONSE_AUTH_READ,
    EFI_RPMB_OK);

  FreePool (mIntermediateBuffer);
  mIntermediateBuffer = NULL;

  return ElapsedMs;
}

UINT64
RpmbWrite (
  IN UINTN  Address,
  IN UINTN  BufferSize,
  IN UINT8  *Buffer
  )
{
  EFI_RPMB_DATA_PACKET WriteRequest;
  EFI_RPMB_DATA_BUFFER WriteRequestBuffer;
  EFI_RPMB_DATA_PACKET WriteResponse;
  UINT64 TimerStart;
  UINT64 ElapsedMs;

  // TODO: Support writing more than 1 block at once if REL_SEC_C is > 1
  ASSERT (BufferSize <= EFI_RPMB_PACKET_DATA_SIZE);

  GenerateDataPacket (
    Buffer,
    BufferSize,
    Address,
    1,
    EFI_RPMB_REQUEST_AUTH_WRITE,
    &WriteRequest);

  WriteRequestBuffer.PacketCount = 1;
  WriteRequestBuffer.Packets = &WriteRequest;

  GeneratePacket (0, &WriteResponse);

  TimerStart = HpcTimerStart ();

  VERIFY_SUCCEEDED (
    mRpmbProtocol->AuthenticatedWrite (
      mRpmbProtocol,
      &WriteRequestBuffer,
      &WriteResponse),
    "Verify authenticated data write");

  ElapsedMs = HpcTimerElapsedMilliseconds (TimerStart);

  RpmbVerifyResponseStatus (
    &WriteResponse,
    EFI_RPMB_RESPONSE_AUTH_WRITE,
    EFI_RPMB_OK);

  return ElapsedMs;
}

VOID
RpmbTestClearAll (
  VOID
  )
{
  UINT8 WriteData[EFI_RPMB_PACKET_DATA_SIZE] = { 0 };

  const UINTN RpmbStorageSize = (UINTN)mRpmbProtocol->RpmbSizeMult * (UINTN)(128 * 1024);
  const UINTN TotalPacketCount = RpmbStorageSize / EFI_RPMB_PACKET_DATA_SIZE;
  UINTN Lba;
  UINT64 TotalWriteElapsedTime = 0;

  LOG_COMMENT (
    "RpmbStorageSize: 0x%p byte(s) ~ %d Kb. TotalPacketCount: 0x%p\n",
    RpmbStorageSize,
    (UINT32) RpmbStorageSize / 1024,
    TotalPacketCount);

  for (Lba = 0; Lba < TotalPacketCount; ++Lba) {

    if (Lba % 16 == 0) {
      LOG_COMMENT (".", Lba);
    }

    SET_LOG_LEVEL (TestLogError);
    {
      //
      // Write Packet
      //
      TotalWriteElapsedTime += RpmbWrite (Lba, sizeof (WriteData), WriteData);
    }
    SET_LOG_LEVEL (TestLogComment);
  }

  LOG_COMMENT (
    "Block Write Avg Time: %lldus\n",
    (TotalWriteElapsedTime * 1000ULL) / TotalPacketCount);
}

VOID
RpmbTestReset (
  VOID
  )
{
  UINT8 WriteData[EFI_RPMB_PACKET_DATA_SIZE] = { 0 };
  const UINTN PartitionMetaDataStartLba = 0;
  const UINTN PartitionMetaDataSize = 1024;
  const UINTN TotalPacketCount = PartitionMetaDataSize / EFI_RPMB_PACKET_DATA_SIZE;
  UINTN Lba;
  UINT64 TotalWriteElapsedTime = 0;

  LOG_COMMENT (
    "## Resetting RPMB partition ##\n"
    "Writing 0s to the first few blocks to clear the RPMB"
    "partition data and the FAT header\n");

  for (Lba = PartitionMetaDataStartLba; Lba < TotalPacketCount; ++Lba) {

    if (Lba % 16 == 0) {
      LOG_COMMENT (".", Lba);
    }

    SET_LOG_LEVEL (TestLogError);
    {
      //
      // Write Packet
      //
      TotalWriteElapsedTime += RpmbWrite (Lba, sizeof (WriteData), WriteData);
    }
    SET_LOG_LEVEL (TestLogComment);
  }

  LOG_COMMENT (
    "\nResetting system for OPTEE to re-initialize the RPMB partition\n");

  gRT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
}

BOOLEAN
TestModuleSetup (
  VOID
  )
{
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (
            &gEfiRpmbIoProtocolGuid,
            NULL,
            (VOID **) &mRpmbProtocol);
  if (EFI_ERROR(Status)) {
    LOG_ERROR("RPMB not available on the system");
    return FALSE;
  }

  // Print RPMB Related Properties

  LOG_COMMENT (
    "REL_WR_SEC_C: %d, RPMB_SIZE_MULT: %d\n",
    mRpmbProtocol->ReliableSectorCount,
    mRpmbProtocol->RpmbSizeMult);

  LOG_COMMENT ("CID: ");
  HexDump (mRpmbProtocol->Cid, EFI_RPMB_CID_SIZE);

  srand (time(NULL));

  return TRUE;
}

VOID
TestModuleCleanup (
  VOID
  )
{
  mRpmbProtocol = NULL;
}

VOID
TestCleanup (
  )
{
  if (mIntermediateBuffer != NULL) {
    FreePool (mIntermediateBuffer );
    mIntermediateBuffer = NULL;
  }

  if (mBuffer != NULL) {
    FreePool (mBuffer);
    mBuffer = NULL;
  }
}

int
main (
  IN int    argc,
  IN char   **argv
  )
{
  MODULE_SETUP (TestModuleSetup);
  MODULE_CLEANUP (TestModuleCleanup);
  TEST_CLEANUP (TestCleanup);
  TEST_FUNC (RpmbTestReset);

  if (!RUN_MODULE(argc, argv)) {
    return 1;
  }

  return 0;
}
