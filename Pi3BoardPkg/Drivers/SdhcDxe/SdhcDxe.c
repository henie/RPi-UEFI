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

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DmaLib.h>
#include <Library/TimerLib.h>

#include <Protocol/EmbeddedExternalDevice.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/Sdhc.h>

#include <LedLib.h>
#include <Bcm2836.h>
#include <Bcm2836SdHost.h>
#include <BcmMailbox.h>

typedef struct {
    UINT32 SdhcId;
    EFI_HANDLE SdhcProtocolHandle;
    VOID *RegistersBase;
} USDHC_PRIVATE_CONTEXT;

#define LOG_FMT_HELPER(FMT, ...) \
    "SDHC%d:" FMT "%a\n", ((SdhcCtx != NULL) ? SdhcCtx->SdhcId : -1), __VA_ARGS__

#define LOG_INFO(...) \
    DEBUG((DEBUG_INFO | DEBUG_BLKIO, LOG_FMT_HELPER(__VA_ARGS__, "")))

#define LOG_TRACE(...) \
    DEBUG((DEBUG_VERBOSE | DEBUG_BLKIO, LOG_FMT_HELPER(__VA_ARGS__, "")))

#define LOG_ERROR(...) \
    DEBUG((DEBUG_ERROR | DEBUG_BLKIO, LOG_FMT_HELPER(__VA_ARGS__, "")))

#define LOG_ASSERT(TXT) \
    ASSERT(!"Sdhc: " TXT "\n")

#ifdef MIN
#undef MIN
#define MIN(x,y) ((x) > (y) ? (y) : (x))
#endif // MIN

//
// Max block count allowed in a single transfer
//
#define USDHC_MAX_BLOCK_COUNT           0xFFFF

//
// Number of register maximum polls
//
#define USDHC_POLL_RETRY_COUNT          1000000 //100ms

//
// Waits between each registry poll
//
#define USDHC_POLL_WAIT_US              20  // 20 us

#define STALL_TO_STABILIZE_US               10000 // 10ms


//
// uSDHC input clock. Ideally, should query it from clock manager
//
#define USDHC_BASE_CLOCK_FREQ_HZ        250000000

#define USDHC_BLOCK_LENGTH_BYTES               512

// Ensure 16 byte alignment
volatile MAILBOX_GET_CLOCK_RATE MbGcr __attribute__((aligned(16)));

#define NO_DETAIL 0

VOID
DumpState(
    IN USDHC_PRIVATE_CONTEXT *SdhcCtx
    )
{
DEBUG_CODE_BEGIN ();

if(NO_DETAIL)
{return;}

    LOG_TRACE( "SdHost: Registers Dump:");
    LOG_TRACE( "  CMD:  0x%8.8X", MmioRead32(SDHOST_CMD));
    LOG_TRACE( "  ARG:  0x%8.8X", MmioRead32(SDHOST_ARG));
    LOG_TRACE( "  TOUT: 0x%8.8X", MmioRead32(SDHOST_TOUT));
    LOG_TRACE( "  CDIV: 0x%8.8X", MmioRead32(SDHOST_CDIV));
    LOG_TRACE( "  RSP0: 0x%8.8X", MmioRead32(SDHOST_RSP0));
    LOG_TRACE( "  RSP1: 0x%8.8X", MmioRead32(SDHOST_RSP1));
    LOG_TRACE( "  RSP2: 0x%8.8X", MmioRead32(SDHOST_RSP2));
    LOG_TRACE( "  RSP3: 0x%8.8X", MmioRead32(SDHOST_RSP3));
    LOG_TRACE( "  HSTS: 0x%8.8X", MmioRead32(SDHOST_HSTS));
    LOG_TRACE( "  VDD:  0x%8.8X", MmioRead32(SDHOST_VDD));
    LOG_TRACE( "  EDM:  0x%8.8X", MmioRead32(SDHOST_EDM));
    LOG_TRACE( "  HCFG: 0x%8.8X", MmioRead32(SDHOST_HCFG));
    LOG_TRACE( "  HBCT: 0x%8.8X", MmioRead32(SDHOST_HBCT));
	LOG_TRACE( "  DATA: 0x%8.8X", MmioRead32(SDHOST_DATA));
    LOG_TRACE( "  HBLC: 0x%8.8X\n", MmioRead32(SDHOST_HBLC));

DEBUG_CODE_END ();
}

EFI_STATUS
WaitForCmd(
    IN USDHC_PRIVATE_CONTEXT *SdhcCtx,
    IN const SD_COMMAND *Cmd
    )
{
    UINT32 CmdReg = MmioRead32(SDHOST_CMD);
    UINT32 Retry = USDHC_POLL_RETRY_COUNT;
	EFI_STATUS Status = EFI_SUCCESS;

    while ((CmdReg & SDHOST_CMD_NEW_FLAG) &&
           Retry) {
        gBS->Stall(USDHC_POLL_WAIT_US);
        --Retry;
        CmdReg = MmioRead32(SDHOST_CMD);
    }

    if(CmdReg & SDHOST_CMD_NEW_FLAG)
    {
    	Status = EFI_DEVICE_ERROR;
    }

	return Status;
}

EFI_STATUS
FlushReadFifo(
    IN USDHC_PRIVATE_CONTEXT *SdhcCtx
    )
{
    UINT32 Retry = USDHC_POLL_RETRY_COUNT;
	EFI_STATUS Status = EFI_SUCCESS;

    while ((MmioRead32(SDHOST_HSTS) & SDHOST_HSTS_DATA_FLAG) &&
           Retry) {
        MmioRead32(SDHOST_DATA);
        gBS->Stall(USDHC_POLL_WAIT_US);
        --Retry;
    }

	if(!Retry)
	{
		LOG_ERROR("Time-out for Flush Fifo");
		return EFI_TIMEOUT;
	}

	return Status;
}



EFI_STATUS
WaitForCmdResponse(
    IN USDHC_PRIVATE_CONTEXT *SdhcCtx,
    IN const SD_COMMAND *Cmd,
    IN UINT32 Argument
    )
{
    UINT32 CmdReg; CmdReg = MmioRead32(SDHOST_CMD);
    UINT32 Retry = USDHC_POLL_RETRY_COUNT;
	UINT32 StsReg = 0;
	EFI_STATUS Status = EFI_SUCCESS;

    //
    // Wait for command to finish execution either with success or failure
    //
    while ((CmdReg & SDHOST_CMD_NEW_FLAG) &&
           Retry) {
        gBS->Stall(USDHC_POLL_WAIT_US);
        --Retry;
        CmdReg = MmioRead32(SDHOST_CMD);
    }

	StsReg = MmioRead32(SDHOST_HSTS) & SDHOST_HSTS_ERROR;

	if(!Retry)
	{
        Status = EFI_TIMEOUT;
    }

    if (CmdReg & SDHOST_CMD_FAIL_FLAG) {
        Status =  EFI_DEVICE_ERROR;
		
		if(StsReg & SDHOST_HSTS_TIMOUT_ERROR)
		{
			Status = EFI_TIMEOUT;
		}

		// Ignore CRC7 error for command response
		if(StsReg == SDHOST_HSTS_CRC7_ERROR)
		{
			
			LOG_ERROR(
        		"Ignore CRC7 error for %cCMD%d",
        		((Cmd->Class == SdCommandClassApp) ? 'A' : ' '),
        		(UINT32)Cmd->Index);
			Status = EFI_SUCCESS;
		}
    }
	
	// Deselecting the SDCard with CMD7 and RCA=0x0 always timeout on SDHost
	if((Cmd->Index == 7) && Argument == 0)
	{
		Status = EFI_SUCCESS;
	}
	if(Status != EFI_SUCCESS)
	{		
		LOG_ERROR(
        	"Error in getting %cCMD%d response, EFI Stauts 0x%x, Status Reg 0x%x",
        	((Cmd->Class == SdCommandClassApp) ? 'A' : ' '),
        	(UINT32)Cmd->Index,
        	Status, StsReg);
		if((Status == EFI_TIMEOUT) &&
			((Cmd->Index == 1) || (Cmd->Index == 5) || (Cmd->Index == 8)))
		{
			LOG_ERROR("Time-out for CMD%d may expected", Cmd->Index);
		}
		else
		{
			DumpState(SdhcCtx);
		}
	}
    if(Status != EFI_SUCCESS)
	{		
		LOG_INFO(
        	"Wait Response %cCMD%d",
        	((Cmd->Class == SdCommandClassApp) ? 'A' : ' '),
        	(UINT32)Cmd->Index);
		DumpState(SdhcCtx);
	}
	MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);

	return Status;
}

EFI_STATUS
SdhcSetBusWidth(
  IN EFI_SDHC_PROTOCOL *This,
  IN SD_BUS_WIDTH BusWidth
  )
{
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;
    UINT32 HCFG; HCFG = MmioRead32(SDHOST_HCFG);

    LOG_TRACE("SdhcSetBusWidth(%d)", BusWidth);

    switch (BusWidth) {
    case SdBusWidth1Bit:
        HCFG &= (~SDHOST_HCFG_WIDE_EXT_BUS);
        break;
    case SdBusWidth4Bit:
        HCFG |= SDHOST_HCFG_WIDE_EXT_BUS;
        break;
	case SdBusWidth8Bit:
    default:
        LOG_ASSERT("Invalid bus width");
        return EFI_INVALID_PARAMETER;
    }

    MmioWrite32(SDHOST_HCFG, HCFG);

    return EFI_SUCCESS;
}

EFI_STATUS
SdhcSetClock(
    IN EFI_SDHC_PROTOCOL *This,
    IN UINT32 TargetSdFreqHz
    )
{
	EFI_STATUS Status;
    UINT32 CoreClockFreqHz;
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;

    LOG_TRACE("SdhcSetClock(%dHz)", TargetSdFreqHz);

    // First figure out the core clock
    ZeroMem((void*)&MbGcr, sizeof(MbGcr));
    MbGcr.Header.BufferSize = sizeof(MbGcr);
    MbGcr.Header.TagID = TAG_GET_CLOCK_RATE;
    MbGcr.Header.TagLength = 8;
    MbGcr.ClockId = CLOCK_ID_CORE;

    Status = MailboxProperty(
        MAILBOX_CHANNEL_PROPERTY_ARM_VC,
        (MAILBOX_HEADER*)&MbGcr);
    if (EFI_ERROR(Status)) {
        LOG_ERROR("SdHost: SdHostSetClockFrequency(): Failed to query core clock\n");
        return Status;
    }

    CoreClockFreqHz = MbGcr.Rate;

	// fSDCLK = fcore_pclk/(ClockDiv+2)
    UINT32 ClockDiv = (CoreClockFreqHz - (2 * TargetSdFreqHz)) / TargetSdFreqHz;
    UINT32 ActualSdFreqHz = CoreClockFreqHz / (ClockDiv + 2);

    LOG_TRACE(
        "SdHost: CoreClock=%dHz, CDIV=%d, Requested SdClock=%dHz, Actual SdClock=%dHz\n",
        CoreClockFreqHz,
        ClockDiv,
        TargetSdFreqHz,
        ActualSdFreqHz);

    MmioWrite32(SDHOST_CDIV, ClockDiv);
    // Set timeout after 1 second, i.e ActualSdFreqHz SD clock cycles
    MmioWrite32(SDHOST_TOUT, ActualSdFreqHz);

    return EFI_SUCCESS;
}

BOOLEAN
SdhcIsCardPresent(
    IN EFI_SDHC_PROTOCOL *This
    )
{
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;
    BOOLEAN IsCardPresent;

    IsCardPresent = TRUE;

  // Enable if needed while trace debugging, otherwise this will flood the debug
  // console due to being called periodically every second for each SDHC
  LOG_TRACE("SdhcIsCardPresent(): %d", IsCardPresent);

  return IsCardPresent;
}

BOOLEAN
SdhcIsReadOnly(
    IN EFI_SDHC_PROTOCOL *This
    )
{
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;
    BOOLEAN IsReadOnly;

    IsReadOnly = FALSE;

    LOG_TRACE("SdhcIsReadOnly(): %d", IsReadOnly);
    return IsReadOnly;
}

EFI_STATUS
SdhcSendCommand(
    IN EFI_SDHC_PROTOCOL *This,
    IN const SD_COMMAND *Cmd,
    IN UINT32 Argument,
    IN OPTIONAL const SD_COMMAND_XFR_INFO *XfrInfo
    )
{
    EFI_STATUS Status;
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;
	UINT32 CmdReg;

    LOG_TRACE(
        "SdhcSendCommand(%cCMD%d, %08x)",
        ((Cmd->Class == SdCommandClassApp) ? 'A' : ' '),
        (UINT32)Cmd->Index,
        Argument);

    Status = WaitForCmd(SdhcCtx, Cmd);
    if (Status != EFI_SUCCESS) {
        LOG_ERROR("SdhcWaitForCmd failed");
        return Status;
    }

    //
    // Clear Interrupt status
    //
    MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);

	CmdReg = Cmd->Index | SDHOST_CMD_NEW_FLAG;

    //
    // Setup data transfer command
    //
    if (XfrInfo) {
        if (XfrInfo->BlockCount > USDHC_MAX_BLOCK_COUNT) {
            LOG_ERROR(
                "Provided %d block count while SDHC max block count is %d",
                XfrInfo->BlockCount,
                USDHC_MAX_BLOCK_COUNT);
            return EFI_INVALID_PARAMETER;
        }

        //
        // Set block size and count
        //
    }

    //
    // Set CMD parameters
    //
    switch (Cmd->ResponseType) {
    case SdResponseTypeNone:
		CmdReg |= SDHOST_CMD_RESPONSE_CMD_NO_RESP;
        break;

    case SdResponseTypeR1:
	case SdResponseTypeR3:
    case SdResponseTypeR4:
    case SdResponseTypeR5:
    case SdResponseTypeR6:
    case SdResponseTypeR5B:
        break;
		
	case SdResponseTypeR1B:
		CmdReg |= SDHOST_CMD_BUSY_CMD;
		break;

    case SdResponseTypeR2:
        CmdReg |= SDHOST_CMD_RESPONSE_CMD_LONG_RESP;
        break;

    default:
        LOG_ASSERT("SdhcSendCommand(): Invalid response type");
        return EFI_INVALID_PARAMETER;
    }

	if(Cmd->TransferDirection ==SdTransferDirectionRead)
	{
		CmdReg |= SDHOST_CMD_READ_CMD;
		Status = FlushReadFifo(SdhcCtx);
	}

	if(Cmd->TransferDirection ==SdTransferDirectionWrite)
	{
		CmdReg |= SDHOST_CMD_WRITE_CMD;
	}

	if(((Cmd->Index == 7) || (Cmd->Index == 12)) && (Cmd->Class != SdCommandClassApp))
	{
		//CmdReg |= SDHOST_CMD_BUSY_CMD;
	}

    //
    // Send command and wait for response
    //
    MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);
    MmioWrite32(SDHOST_ARG, Argument);
    MmioWrite32(SDHOST_CMD, CmdReg);

    Status = WaitForCmdResponse(SdhcCtx, Cmd, Argument);
    if (EFI_ERROR(Status)) {
        LOG_ERROR("WaitForCmdResponse() failed. %r", Status);
        return Status;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
SdhcReceiveResponse(
    IN EFI_SDHC_PROTOCOL *This,
    IN const SD_COMMAND *Cmd,
    OUT UINT32 *Buffer
    )
{

    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;

    if (Buffer == NULL) {
        LOG_ERROR("Input Buffer is NULL");
        return EFI_INVALID_PARAMETER;
    }

    switch (Cmd->ResponseType) {
    case SdResponseTypeNone:
        break;
    case SdResponseTypeR1:
    case SdResponseTypeR1B:
    case SdResponseTypeR3:
    case SdResponseTypeR4:
    case SdResponseTypeR5:
    case SdResponseTypeR5B:
    case SdResponseTypeR6:
        Buffer[0] = MmioRead32(SDHOST_RSP0);
        LOG_TRACE(
            "SdhcReceiveResponse(Type: %x), Buffer[0]: %08x",
            Cmd->ResponseType,
            Buffer[0]);
        break;
    case SdResponseTypeR2:
        Buffer[0] = MmioRead32(SDHOST_RSP0);
        Buffer[1] = MmioRead32(SDHOST_RSP1);
        Buffer[2] = MmioRead32(SDHOST_RSP2);
        Buffer[3] = MmioRead32(SDHOST_RSP3);

		//
        // Shift the whole response right 8-bits to strip down CRC. It is common for standard
        // SDHCs to not store in the RSP registers the first 8-bits for R2 responses CID[0:7] 
        // and CSD[0:7] since those 8-bits contain the CRC which is already handled by the SDHC HW FSM
        //
        UINT8 *BufferAsBytes = (UINT8*)Buffer;
        const UINT32 BufferSizeMax = sizeof(UINT32) * 4;
        UINT32 ByteIdx;
        for (ByteIdx = 0; ByteIdx < BufferSizeMax - 1; ++ByteIdx) {
            BufferAsBytes[ByteIdx] = BufferAsBytes[ByteIdx + 1];
        }
        BufferAsBytes[BufferSizeMax - 1] = 0;

        LOG_TRACE(
            "SdhcReceiveResponse(Type: %x), Buffer[0-3]: %08x, %08x, %08x, %08x",
            Cmd->ResponseType,
            Buffer[0],
            Buffer[1],
            Buffer[2],
            Buffer[3]);
        break;
    default:
        LOG_ASSERT("SdhcReceiveResponse(): Invalid response type");
        return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
SdhcReadBlockData(
    IN EFI_SDHC_PROTOCOL *This,
    IN UINTN LengthInBytes,
    OUT UINT32* Buffer
    )
{
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;

    LOG_TRACE(
        "SdhcReadBlockData(Len: 0x%x, Buffer: 0x%x)",
        LengthInBytes,
        Buffer);

    ASSERT(Buffer != NULL);
    ASSERT(LengthInBytes % sizeof(UINT32) == 0);

    EFI_STATUS Status = EFI_SUCCESS;

    LedSetOk(TRUE);
    {
        UINT32 NumWords = LengthInBytes / 4;
        UINT32 WordIdx;

        for (WordIdx = 0; WordIdx < NumWords; ++WordIdx) {
            UINT32 PollCount = 0;
            while (PollCount < USDHC_POLL_RETRY_COUNT) {
                if (MmioRead32(SDHOST_HSTS) & SDHOST_HSTS_DATA_FLAG) {
                    Buffer[WordIdx] = MmioRead32(SDHOST_DATA);
                    break;
                }

                ++PollCount;
            }
            
            if (PollCount == USDHC_POLL_RETRY_COUNT) {
                LOG_ERROR(
                        "SdHost: SdhcReadBlockData(): Block Word%d read poll timed-out\n",
                        WordIdx);
                MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);
                Status = EFI_TIMEOUT;
                break;
            }
        }
    }
    LedSetOk(FALSE);

    return Status;
}

//
// A temp hacky implementation for SdhcWriteBlockData that seem to work and
// doesn't cause any data corruption. Use for now until a cleaner implementation
// that works is in place.
//
EFI_STATUS
SdhcWriteBlockData(
    IN EFI_SDHC_PROTOCOL *This,
    IN UINTN LengthInBytes,
    IN const UINT32* Buffer
    )
{
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;

    LOG_TRACE(
        "SdhcWriteBlockData(LengthInBytes: 0x%x, Buffer: 0x%x)",
        LengthInBytes,
        Buffer);

    ASSERT(Buffer != NULL);
    ASSERT(LengthInBytes % USDHC_BLOCK_LENGTH_BYTES == 0);

    
    EFI_STATUS Status = EFI_SUCCESS;

    LedSetOk(TRUE);
    {
        UINT32 NumWords = LengthInBytes / 4;
        UINT32 WordIdx;

        for (WordIdx = 0; WordIdx < NumWords; ++WordIdx) {
            UINT32 PollCount = 0;
            while (PollCount < USDHC_POLL_RETRY_COUNT) {
                if (MmioRead32(SDHOST_HSTS) & SDHOST_HSTS_DATA_FLAG) {
                    MmioWrite32(SDHOST_DATA, Buffer[WordIdx]);
                    break;
                }
				
               ++PollCount;
            }

            if (PollCount == USDHC_POLL_RETRY_COUNT) {
                DEBUG((
                    DEBUG_ERROR,
                    "SdHost: SdWriteBlockData(): Block Word%d write poll timed-out\n",
                    WordIdx));
				DumpState(SdhcCtx);
                MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);
                Status = EFI_TIMEOUT;
                break;
            }
        }
    }
    LedSetOk(FALSE);    

    return Status;
}

EFI_STATUS
SdhcSoftwareReset(
    IN EFI_SDHC_PROTOCOL *This,
    IN SDHC_RESET_TYPE ResetType
    )
{
    USDHC_PRIVATE_CONTEXT *SdhcCtx = (USDHC_PRIVATE_CONTEXT*)This->PrivateContext;

    if (ResetType == SdhcResetTypeAll) {
        LOG_TRACE("SdhcSoftwareReset(ALL)");

		LOG_TRACE("Registers before reset");
		DumpState(SdhcCtx);
        //
        // Software reset for ALL
        //
        // Turn-off SD Card power
        MmioWrite32(SDHOST_VDD, 0);
        {
            // Reset command and arg
            MmioWrite32(SDHOST_CMD, 0);
            MmioWrite32(SDHOST_ARG, 0);
            // Reset clock divider
            MmioWrite32(SDHOST_CDIV, 0);
            // Clear status flags
            MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);
            // Reset controller configs
            MmioWrite32(SDHOST_HCFG, 0);
            MmioWrite32(SDHOST_HBCT, 0);
            MmioWrite32(SDHOST_HBLC, 0);

            gBS->Stall(STALL_TO_STABILIZE_US);
        }
        // Turn-on SD Card power
        MmioWrite32(SDHOST_VDD, 1);

        gBS->Stall(STALL_TO_STABILIZE_US);

        // Write controller configs
        UINT32 Hcfg = 0;
        Hcfg |= SDHOST_HCFG_WIDE_INT_BUS;
        Hcfg |= SDHOST_HCFG_SLOW_CARD; // Use all bits of CDIV in DataMode
        MmioWrite32(SDHOST_HCFG, Hcfg);

		UINT32 Edm = MmioRead32(SDHOST_EDM);
		LOG_TRACE("EDM %x", Edm);
        Edm &= 0xFF;
		Edm |= 0x10800;
        MmioWrite32(SDHOST_EDM, Edm);

		MmioWrite32(SDHOST_HBCT, 512);

        LOG_TRACE("Reset ALL complete");

    }else if (ResetType == SdhcResetTypeCmd) {
        LOG_TRACE("SdhcSoftwareReset(CMD)");
        //
        // Software reset for CMD
        //
        MmioWrite32(SDHOST_CMD, 0);
        MmioWrite32(SDHOST_ARG, 0);
        MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);

        LOG_TRACE("Reset CMD complete");

    } else if (ResetType == SdhcResetTypeData) {
        LOG_TRACE("SdhcSoftwareReset(DAT)");
        //
        // Software reset for DAT
        //
        MmioWrite32(SDHOST_HSTS, SDHOST_HSTS_CLEAR);
        MmioOr32(SDHOST_EDM, SDHOST_EDM_FIFO_CLEAR);

        LOG_TRACE("Reset DAT complete");

    } else {
        return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

VOID
SdhcCleanup(
    IN EFI_SDHC_PROTOCOL *This
    )
{
    if (This->PrivateContext != NULL) {
        FreePool(This->PrivateContext);
        This->PrivateContext = NULL;
    }

    FreePool(This);

    //
    // Any SDHC protocol call to this instance is illegal beyond this point
    //
}

VOID
SdhcGetCapabilities(
    IN EFI_SDHC_PROTOCOL *This,
    OUT SDHC_CAPABILITIES *Capabilities
  )
{
    Capabilities->MaximumBlockSize = (UINT32)(512);
    Capabilities->MaximumBlockCount = 0xFFFF; // UINT16_MAX
}


EFI_SDHC_PROTOCOL gSdhcProtocolTemplate =
{
    SDHC_PROTOCOL_INTERFACE_REVISION,   // Revision
    0,                                  // DeviceId
    NULL,                               // PrivateContext
    SdhcGetCapabilities,
    SdhcSoftwareReset,
    SdhcSetClock,
    SdhcSetBusWidth,
    SdhcIsCardPresent,
    SdhcIsReadOnly,
    SdhcSendCommand,
    SdhcReceiveResponse,
    SdhcReadBlockData,
    SdhcWriteBlockData,
    SdhcCleanup
};

EFI_STATUS
uSdhcDeviceRegister(
    IN EFI_HANDLE ImageHandle,
    IN UINT32 SdhcId,
    IN VOID* RegistersBase
    )
{
    EFI_STATUS Status;
    EFI_SDHC_PROTOCOL *SdhcProtocol = NULL;
    USDHC_PRIVATE_CONTEXT *SdhcCtx = NULL;

    if (ImageHandle == NULL ||
        RegistersBase == NULL) {
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
    }

    //
    // Allocate per-device SDHC protocol and private context storage
    //

    SdhcProtocol = AllocateCopyPool(sizeof(EFI_SDHC_PROTOCOL), &gSdhcProtocolTemplate);
    if (SdhcProtocol == NULL) {
        Status =  EFI_OUT_OF_RESOURCES;
        goto Exit;
    }
    SdhcProtocol->SdhcId = SdhcId;
    SdhcProtocol->PrivateContext = AllocateZeroPool(sizeof(USDHC_PRIVATE_CONTEXT));
    if (SdhcProtocol->PrivateContext == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }

    SdhcCtx = (USDHC_PRIVATE_CONTEXT*)SdhcProtocol->PrivateContext;
    SdhcCtx->SdhcId = SdhcId;
    SdhcCtx->RegistersBase = RegistersBase;
    

    LOG_INFO(
        "Initializing uSDHC%d @%p ",
        SdhcId,
        RegistersBase);

    Status = gBS->InstallMultipleProtocolInterfaces(
                                                &SdhcCtx->SdhcProtocolHandle,
                                                &gEfiSdhcProtocolGuid,
                                                SdhcProtocol,
                                                NULL);
    if (EFI_ERROR(Status)) {
        LOG_ERROR("InstallMultipleProtocolInterfaces failed. %r", Status);
        goto Exit;
    }

Exit:
    if (EFI_ERROR(Status)) {
        LOG_ERROR("Failed to register and initialize uSDHC%d", SdhcId);

        if (SdhcProtocol != NULL && SdhcProtocol->PrivateContext != NULL) {
            FreePool(SdhcProtocol->PrivateContext);
            SdhcProtocol->PrivateContext = NULL;
        }

        if (SdhcProtocol != NULL) {
            FreePool(SdhcProtocol);
            SdhcProtocol = NULL;
        }
    }

    return Status;
}

EFI_STATUS
SdhcInitialize(
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    EFI_STATUS Status = EFI_SUCCESS;
    UINT32 uSdhcRegisteredCount = 0;

    //
    // Register uSDHC1 
    //

    //
    // uSDHC1
    //  
    Status = uSdhcDeviceRegister(
        ImageHandle,
        1,
        (VOID*)SDHOST_BASE_ADDRESS);
    if (!EFI_ERROR(Status)) {
        ++uSdhcRegisteredCount;
    }
	
    //
    // Succeed driver loading if at least one enabled uSDHC got registered successfully
    //
    if ((Status != EFI_SUCCESS) && (uSdhcRegisteredCount > 0)) {
        Status = EFI_SUCCESS;
		
    }
	
	// Init the LED to use as a disk access indicator
    LedInit();

    return Status;
}
