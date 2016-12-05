#include "Iop_SifCmd.h"
#include "IopBios.h"
#include "../ee/SIF.h"
#include "../Log.h"
#include "../StructCollectionStateFile.h"

using namespace Iop;

#define LOG_NAME							("iop_sifcmd")
#define STATE_MODULES						("iop_sifcmd/modules.xml")
#define STATE_MODULE						("Module")
#define STATE_MODULE_SERVER_DATA_ADDRESS	("ServerDataAddress")

#define CUSTOM_FINISHEXECREQUEST    0x666
#define CUSTOM_FINISHEXECCMD        0x667
#define CUSTOM_SLEEPTHREAD          0x668

#define MODULE_NAME						"sifcmd"
#define MODULE_VERSION					0x101

#define FUNCTION_SIFGETSREG				"SifGetSreg"
#define FUNCTION_SIFSETCMDBUFFER		"SifSetCmdBuffer"
#define FUNCTION_SIFADDCMDHANDLER		"SifAddCmdHandler"
#define FUNCTION_SIFSENDCMD				"SifSendCmd"
#define FUNCTION_ISIFSENDCMD			"iSifSendCmd"
#define FUNCTION_SIFINITRPC				"SifInitRpc"
#define FUNCTION_SIFBINDRPC				"SifBindRpc"
#define FUNCTION_SIFCALLRPC				"SifCallRpc"
#define FUNCTION_SIFREGISTERRPC			"SifRegisterRpc"
#define FUNCTION_SIFCHECKSTATRPC		"SifCheckStatRpc"
#define FUNCTION_SIFSETRPCQUEUE			"SifSetRpcQueue"
#define FUNCTION_SIFGETNEXTREQUEST		"SifGetNextRequest"
#define FUNCTION_SIFEXECREQUEST			"SifExecRequest"
#define FUNCTION_SIFRPCLOOP				"SifRpcLoop"
#define FUNCTION_SIFGETOTHERDATA		"SifGetOtherData"
#define FUNCTION_FINISHEXECREQUEST		"FinishExecRequest"
#define FUNCTION_FINISHEXECCMD			"FinishExecCmd"
#define FUNCTION_SLEEPTHREAD			"SleepThread"

#define SYSTEM_COMMAND_ID 0x80000000

CSifCmd::CSifCmd(CIopBios& bios, CSifMan& sifMan, CSysmem& sysMem, uint8* ram) 
: m_sifMan(sifMan)
, m_bios(bios)
, m_sysMem(sysMem)
, m_ram(ram)
{
	m_moduleDataAddr = m_sysMem.AllocateMemory(sizeof(MODULEDATA), 0, 0);
	m_trampolineAddr           = m_moduleDataAddr + offsetof(MODULEDATA, trampoline);
	m_sendCmdExtraStructAddr   = m_moduleDataAddr + offsetof(MODULEDATA, sendCmdExtraStruct);
	m_sregAddr                 = m_moduleDataAddr + offsetof(MODULEDATA, sreg);
	m_sysCmdBuffer             = m_moduleDataAddr + offsetof(MODULEDATA, sysCmdBuffer);
	m_pendingCmdBufferAddr     = m_moduleDataAddr + offsetof(MODULEDATA, pendingCmdBuffer);
	m_pendingCmdBufferSizeAddr = m_moduleDataAddr + offsetof(MODULEDATA, pendingCmdBufferSize);
	sifMan.SetModuleResetHandler([&] (const std::string& path) { bios.ProcessModuleReset(path); });
	sifMan.SetCustomCommandHandler([&] (uint32 commandHeaderAddr) { ProcessCustomCommand(commandHeaderAddr); });
	BuildExportTable();
}

CSifCmd::~CSifCmd()
{
	ClearServers();
}

void CSifCmd::LoadState(Framework::CZipArchiveReader& archive)
{
	ClearServers();

	auto modulesFile = CStructCollectionStateFile(*archive.BeginReadFile(STATE_MODULES));
	{
		for(CStructCollectionStateFile::StructIterator structIterator(modulesFile.GetStructBegin());
			structIterator != modulesFile.GetStructEnd(); structIterator++)
		{
			const auto& structFile(structIterator->second);
			uint32 serverDataAddress = structFile.GetRegister32(STATE_MODULE_SERVER_DATA_ADDRESS);
			auto serverData = reinterpret_cast<SIFRPCSERVERDATA*>(m_ram + serverDataAddress);
			auto module = new CSifDynamic(*this, serverDataAddress);
			m_servers.push_back(module);
			m_sifMan.RegisterModule(serverData->serverId, module);
		}
	}
}

void CSifCmd::SaveState(Framework::CZipArchiveWriter& archive)
{
	auto modulesFile = new CStructCollectionStateFile(STATE_MODULES);
	{
		int moduleIndex = 0;
		for(const auto& module : m_servers)
		{
			auto moduleName = std::string(STATE_MODULE) + std::to_string(moduleIndex++);
			CStructFile moduleStruct;
			{
				uint32 serverDataAddress = module->GetServerDataAddress();
				moduleStruct.SetRegister32(STATE_MODULE_SERVER_DATA_ADDRESS, serverDataAddress); 
			}
			modulesFile->InsertStruct(moduleName.c_str(), moduleStruct);
		}
	}
	archive.InsertFile(modulesFile);
}

std::string CSifCmd::GetId() const
{
	return MODULE_NAME;
}

std::string CSifCmd::GetFunctionName(unsigned int functionId) const
{
	switch(functionId)
	{
	case 6:
		return FUNCTION_SIFGETSREG;
		break;
	case 8:
		return FUNCTION_SIFSETCMDBUFFER;
		break;
	case 10:
		return FUNCTION_SIFADDCMDHANDLER;
		break;
	case 12:
		return FUNCTION_SIFSENDCMD;
		break;
	case 13:
		return FUNCTION_ISIFSENDCMD;
		break;
	case 14:
		return FUNCTION_SIFINITRPC;
		break;
	case 15:
		return FUNCTION_SIFBINDRPC;
		break;
	case 16:
		return FUNCTION_SIFCALLRPC;
		break;
	case 17:
		return FUNCTION_SIFREGISTERRPC;
		break;
	case 18:
		return FUNCTION_SIFCHECKSTATRPC;
		break;
	case 19:
		return FUNCTION_SIFSETRPCQUEUE;
		break;
	case 20:
		return FUNCTION_SIFGETNEXTREQUEST;
		break;
	case 21:
		return FUNCTION_SIFEXECREQUEST;
		break;
	case 22:
		return FUNCTION_SIFRPCLOOP;
		break;
	case 23:
		return FUNCTION_SIFGETOTHERDATA;
		break;
	case CUSTOM_FINISHEXECREQUEST:
		return FUNCTION_FINISHEXECREQUEST;
		break;
	case CUSTOM_FINISHEXECCMD:
		return FUNCTION_FINISHEXECCMD;
		break;
	case CUSTOM_SLEEPTHREAD:
		return FUNCTION_SLEEPTHREAD;
		break;
	default:
		return "unknown";
		break;
	}
}

void CSifCmd::Invoke(CMIPS& context, unsigned int functionId)
{
	switch(functionId)
	{
	case 6:
		context.m_State.nGPR[CMIPS::V0].nV0 = SifGetSreg(
			context.m_State.nGPR[CMIPS::A0].nV0
		);
		break;
	case 8:
		context.m_State.nGPR[CMIPS::V0].nV0 = SifSetCmdBuffer(
			context.m_State.nGPR[CMIPS::A0].nV0, 
			context.m_State.nGPR[CMIPS::A1].nV0);
		break;
	case 10:
		SifAddCmdHandler(
			context.m_State.nGPR[CMIPS::A0].nV0,
			context.m_State.nGPR[CMIPS::A1].nV0,
			context.m_State.nGPR[CMIPS::A2].nV0);
		break;
	case 12:
	case 13:
		context.m_State.nGPR[CMIPS::V0].nV0 = SifSendCmd(
			context.m_State.nGPR[CMIPS::A0].nV0,
			context.m_State.nGPR[CMIPS::A1].nV0,
			context.m_State.nGPR[CMIPS::A2].nV0,
			context.m_State.nGPR[CMIPS::A3].nV0,
			context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x10),
			context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x14));
		break;
	case 14:
		CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFINITRPC "();\r\n");
		break;
	case 15:
		context.m_State.nGPR[CMIPS::V0].nV0 = SifBindRpc(
			context.m_State.nGPR[CMIPS::A0].nV0,
			context.m_State.nGPR[CMIPS::A1].nV0,
			context.m_State.nGPR[CMIPS::A2].nV0);
		break;
	case 16:
		SifCallRpc(context);
		break;
	case 17:
		SifRegisterRpc(context);
		break;
	case 18:
		context.m_State.nGPR[CMIPS::V0].nV0 = SifCheckStatRpc(
			context.m_State.nGPR[CMIPS::A0].nV0);
		break;
	case 19:
		SifSetRpcQueue(context.m_State.nGPR[CMIPS::A0].nV0,
			context.m_State.nGPR[CMIPS::A1].nV0);
		break;
	case 20:
		context.m_State.nGPR[CMIPS::V0].nD0 = static_cast<int32>(SifGetNextRequest(
			context.m_State.nGPR[CMIPS::A0].nV0
		));
		break;
	case 21:
		SifExecRequest(context);
		break;
	case 22:
		SifRpcLoop(context);
		break;
	case 23:
		context.m_State.nGPR[CMIPS::V0].nV0 = SifGetOtherData(
			context.m_State.nGPR[CMIPS::A0].nV0,
			context.m_State.nGPR[CMIPS::A1].nV0,
			context.m_State.nGPR[CMIPS::A2].nV0,
			context.m_State.nGPR[CMIPS::A3].nV0,
			context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x10));
		break;
	case CUSTOM_FINISHEXECREQUEST:
		FinishExecRequest(
			context.m_State.nGPR[CMIPS::A0].nV0,
			context.m_State.nGPR[CMIPS::A1].nV0
		);
		break;
	case CUSTOM_FINISHEXECCMD:
		FinishExecCmd();
		break;
	case CUSTOM_SLEEPTHREAD:
		SleepThread();
		break;
	default:
		CLog::GetInstance().Print(LOG_NAME, "Unknown function called (%d).\r\n", 
			functionId);
		break;
	}
}

void CSifCmd::ClearServers()
{
	for(const auto& server : m_servers)
	{
		auto serverData = reinterpret_cast<SIFRPCSERVERDATA*>(m_ram + server->GetServerDataAddress());
		m_sifMan.UnregisterModule(serverData->serverId);
		delete server;
	}
	m_servers.clear();
}

void CSifCmd::BuildExportTable()
{
	uint32* exportTable = reinterpret_cast<uint32*>(m_ram + m_trampolineAddr);
	*(exportTable++) = 0x41E00000;
	*(exportTable++) = 0;
	*(exportTable++) = MODULE_VERSION;
	strcpy(reinterpret_cast<char*>(exportTable), MODULE_NAME);
	exportTable += (strlen(MODULE_NAME) + 3) / 4;

	{
		CMIPSAssembler assembler(exportTable);

		//Trampoline for SifGetNextRequest
		uint32 sifGetNextRequestAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);
		assembler.JR(CMIPS::RA);
		assembler.ADDIU(CMIPS::R0, CMIPS::R0, 20);

		//Trampoline for SifExecRequest
		uint32 sifExecRequestAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);
		assembler.JR(CMIPS::RA);
		assembler.ADDIU(CMIPS::R0, CMIPS::R0, 21);

		uint32 finishExecRequestAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);
		assembler.JR(CMIPS::RA);
		assembler.ADDIU(CMIPS::R0, CMIPS::R0, CUSTOM_FINISHEXECREQUEST);

		uint32 finishExecCmdAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);
		assembler.JR(CMIPS::RA);
		assembler.ADDIU(CMIPS::R0, CMIPS::R0, CUSTOM_FINISHEXECCMD);

		uint32 sleepThreadAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);
		assembler.JR(CMIPS::RA);
		assembler.ADDIU(CMIPS::R0, CMIPS::R0, CUSTOM_SLEEPTHREAD);

		//Assemble SifRpcLoop
		{
			static const int16 stackAlloc = 0x10;

			m_sifRpcLoopAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);
			auto checkNextRequestLabel = assembler.CreateLabel();
			auto sleepThreadLabel = assembler.CreateLabel();

			assembler.ADDIU(CMIPS::SP, CMIPS::SP, -stackAlloc);
			assembler.SW(CMIPS::RA, 0x00, CMIPS::SP);
			assembler.SW(CMIPS::S0, 0x04, CMIPS::SP);
			assembler.ADDU(CMIPS::S0, CMIPS::A0, CMIPS::R0);

			//checkNextRequest:
			assembler.MarkLabel(checkNextRequestLabel);
			assembler.JAL(sifGetNextRequestAddr);
			assembler.ADDU(CMIPS::A0, CMIPS::S0, CMIPS::R0);
			assembler.BEQ(CMIPS::V0, CMIPS::R0, sleepThreadLabel);
			assembler.NOP();

			assembler.JAL(sifExecRequestAddr);
			assembler.ADDU(CMIPS::A0, CMIPS::V0, CMIPS::R0);

			//sleepThread:
			assembler.MarkLabel(sleepThreadLabel);
			assembler.JAL(sleepThreadAddr);
			assembler.NOP();
			assembler.BEQ(CMIPS::R0, CMIPS::R0, checkNextRequestLabel);
			assembler.NOP();

			assembler.LW(CMIPS::S0, 0x04, CMIPS::SP);
			assembler.LW(CMIPS::RA, 0x00, CMIPS::SP);
			assembler.JR(CMIPS::RA);
			assembler.ADDIU(CMIPS::SP, CMIPS::SP, stackAlloc);
		}

		//Assemble SifExecRequest
		{
			static const int16 stackAlloc = 0x20;

			m_sifExecRequestAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);

			assembler.ADDIU(CMIPS::SP, CMIPS::SP, -stackAlloc);
			assembler.SW(CMIPS::RA, 0x1C, CMIPS::SP);
			assembler.SW(CMIPS::S0, 0x18, CMIPS::SP);
			assembler.ADDU(CMIPS::S0, CMIPS::A0, CMIPS::R0);

			assembler.LW(CMIPS::A0, offsetof(SIFRPCSERVERDATA, rid), CMIPS::S0);
			assembler.LW(CMIPS::A1, offsetof(SIFRPCSERVERDATA, buffer), CMIPS::S0);
			assembler.LW(CMIPS::A2, offsetof(SIFRPCSERVERDATA, rsize), CMIPS::S0);
			assembler.LW(CMIPS::T0, offsetof(SIFRPCSERVERDATA, function), CMIPS::S0);
			assembler.JALR(CMIPS::T0);
			assembler.NOP();

			assembler.ADDU(CMIPS::A0, CMIPS::S0, CMIPS::R0);
			assembler.JAL(finishExecRequestAddr);
			assembler.ADDU(CMIPS::A1, CMIPS::V0, CMIPS::R0);

			assembler.LW(CMIPS::S0, 0x18, CMIPS::SP);
			assembler.LW(CMIPS::RA, 0x1C, CMIPS::SP);
			assembler.JR(CMIPS::RA);
			assembler.ADDIU(CMIPS::SP, CMIPS::SP, stackAlloc);
		}

		//Assemble SifExecCmdHandler
		{
			static const int16 stackAlloc = 0x20;

			m_sifExecCmdHandlerAddr = (reinterpret_cast<uint8*>(exportTable) - m_ram) + (assembler.GetProgramSize() * 4);

			assembler.ADDIU(CMIPS::SP, CMIPS::SP, -stackAlloc);
			assembler.SW(CMIPS::RA, 0x1C, CMIPS::SP);
			assembler.SW(CMIPS::S0, 0x18, CMIPS::SP);
			assembler.ADDU(CMIPS::S0, CMIPS::A0, CMIPS::R0);

			assembler.ADDU(CMIPS::A0, CMIPS::A1, CMIPS::R0);    //A0 = Packet Address
			assembler.LW(CMIPS::A1, offsetof(SIFCMDDATA, data), CMIPS::S0);
			assembler.LW(CMIPS::T0, offsetof(SIFCMDDATA, sifCmdHandler), CMIPS::S0);
			assembler.JALR(CMIPS::T0);
			assembler.NOP();

			assembler.JAL(finishExecCmdAddr);
			assembler.NOP();

			assembler.LW(CMIPS::S0, 0x18, CMIPS::SP);
			assembler.LW(CMIPS::RA, 0x1C, CMIPS::SP);
			assembler.JR(CMIPS::RA);
			assembler.ADDIU(CMIPS::SP, CMIPS::SP, stackAlloc);
		}
	}
}

void CSifCmd::ProcessInvocation(uint32 serverDataAddr, uint32 methodId, uint32* params, uint32 size)
{
	auto serverData = reinterpret_cast<SIFRPCSERVERDATA*>(m_ram + serverDataAddr);
	auto queueData = reinterpret_cast<SIFRPCQUEUEDATA*>(m_ram + serverData->queueAddr);

	//Copy params
	if(serverData->buffer != 0)
	{
		memcpy(&m_ram[serverData->buffer], params, size);
	}
	serverData->rid = methodId;
	serverData->rsize = size;

	assert(queueData->serverDataLink == 0);
	assert(queueData->serverDataStart == serverDataAddr);
	queueData->serverDataLink = serverDataAddr;

	auto thread = m_bios.GetThread(queueData->threadId);
	assert(thread->status == CIopBios::THREAD_STATUS_SLEEPING);
	m_bios.WakeupThread(queueData->threadId, true);
	m_bios.Reschedule();
}

void CSifCmd::FinishExecRequest(uint32 serverDataAddr, uint32 returnDataAddr)
{
	auto serverData = reinterpret_cast<SIFRPCSERVERDATA*>(m_ram + serverDataAddr);
	auto returnData = m_ram + returnDataAddr;
	m_sifMan.SendCallReply(serverData->serverId, returnData);
}

void CSifCmd::FinishExecCmd()
{
	assert(m_executingCmd);
	m_executingCmd = false;

	uint32 commandHeaderAddr = m_pendingCmdBufferAddr;
	auto commandHeader = reinterpret_cast<const SIFCMDHEADER*>(m_ram + commandHeaderAddr);

	uint8 commandPacketSize = static_cast<uint8>(commandHeader->size & 0xFF);
	auto pendingCmdBuffer = m_ram + m_pendingCmdBufferAddr;
	auto pendingCmdBufferSize = reinterpret_cast<uint32*>(m_ram + m_pendingCmdBufferSizeAddr);
	assert(*pendingCmdBufferSize >= commandPacketSize);
	memmove(pendingCmdBuffer, pendingCmdBuffer + commandPacketSize, PENDING_CMD_BUFFER_SIZE - *pendingCmdBufferSize);
	(*pendingCmdBufferSize) -= commandPacketSize;

	if(*pendingCmdBufferSize > 0)
	{
		ProcessNextDynamicCommand();
	}
}

void CSifCmd::ProcessCustomCommand(uint32 commandHeaderAddr)
{
	auto commandHeader = reinterpret_cast<const SIFCMDHEADER*>(m_ram + commandHeaderAddr);
	switch(commandHeader->commandId)
	{
	case SIF_CMD_SETSREG:
		ProcessSetSreg(commandHeaderAddr);
		break;
	case 0x80000004:
		//No clue what this is used for, but seems to be used after "WriteToIop" is used.
		//Could be FlushCache or something like that
		break;
	case SIF_CMD_REND:
		ProcessRpcRequestEnd(commandHeaderAddr);
		break;
	default:
		ProcessDynamicCommand(commandHeaderAddr);
		break;
	}
}

void CSifCmd::ProcessSetSreg(uint32 commandHeaderAddr)
{
	auto setSreg = reinterpret_cast<const SIFSETSREG*>(m_ram + commandHeaderAddr);
	assert(setSreg->header.size == sizeof(SIFSETSREG));
	assert(setSreg->index < MAX_SREG);
	if(setSreg->index >= MAX_SREG) return;
	reinterpret_cast<uint32*>(m_ram + m_sregAddr)[setSreg->index] = setSreg->value;
}

void CSifCmd::ProcessRpcRequestEnd(uint32 commandHeaderAddr)
{
	auto requestEnd = reinterpret_cast<const SIFRPCREQUESTEND*>(m_ram + commandHeaderAddr);
	assert(requestEnd->clientDataAddr != 0);
	auto clientData = reinterpret_cast<SIFRPCCLIENTDATA*>(m_ram + requestEnd->clientDataAddr);
	if(requestEnd->commandId == SIF_CMD_BIND)
	{
		//When serverDataAddr is 0, EE failed to find requested server ID
		assert(requestEnd->serverDataAddr != 0);
		clientData->serverDataAddr = requestEnd->serverDataAddr;
		clientData->buffPtr = requestEnd->buffer;
		clientData->cbuffPtr = requestEnd->cbuffer;
	}
	else if(requestEnd->commandId == SIF_CMD_CALL)
	{
		assert(clientData->endFctPtr == 0);
	}
	else
	{
		assert(0);
	}
	//Unlock/delete semaphore
	{
		assert(clientData->header.semaId != 0);
		int32 result = 0;
		result = m_bios.SignalSemaphore(clientData->header.semaId, true);
		assert(result == 0);
		result = m_bios.DeleteSemaphore(clientData->header.semaId);
		assert(result == 0);
		clientData->header.semaId = 0;
	}
}

void CSifCmd::ProcessDynamicCommand(uint32 commandHeaderAddr)
{
	auto commandHeader = reinterpret_cast<const SIFCMDHEADER*>(m_ram + commandHeaderAddr);

	uint8 commandPacketSize = static_cast<uint8>(commandHeader->size & 0xFF);
	auto pendingCmdBuffer = m_ram + m_pendingCmdBufferAddr;
	auto pendingCmdBufferSize = reinterpret_cast<uint32*>(m_ram + m_pendingCmdBufferSizeAddr);
	assert((*pendingCmdBufferSize + commandPacketSize) <= PENDING_CMD_BUFFER_SIZE);

	if((*pendingCmdBufferSize + commandPacketSize) <= PENDING_CMD_BUFFER_SIZE)
	{
		memcpy(pendingCmdBuffer + *pendingCmdBufferSize, commandHeader, commandPacketSize);
		(*pendingCmdBufferSize) += commandPacketSize;

		if(!m_executingCmd)
		{
			ProcessNextDynamicCommand();
		}
	}
}

void CSifCmd::ProcessNextDynamicCommand()
{
	assert(!m_executingCmd);
	m_executingCmd = true;

	uint32 commandHeaderAddr = m_pendingCmdBufferAddr;
	auto commandHeader = reinterpret_cast<const SIFCMDHEADER*>(m_ram + commandHeaderAddr);
	bool isSystemCommand = (commandHeader->commandId & SYSTEM_COMMAND_ID) != 0;
	uint32 cmd = commandHeader->commandId & ~SYSTEM_COMMAND_ID;
	uint32 cmdBuffer = isSystemCommand ? m_sysCmdBuffer : m_usrCmdBuffer;
	uint32 cmdBufferLen = isSystemCommand ? MAX_SYSTEM_COMMAND : m_usrCmdBufferLen;

	if((cmdBuffer != 0) && (cmd < cmdBufferLen))
	{
		const auto& cmdDataEntry = reinterpret_cast<SIFCMDDATA*>(m_ram + cmdBuffer)[cmd];

		CLog::GetInstance().Print(LOG_NAME, "Calling SIF command handler for command 0x%0.8X at 0x%0.8X with data 0x%0.8X.\r\n", 
			commandHeader->commandId, cmdDataEntry.sifCmdHandler, cmdDataEntry.data);

		assert(cmdDataEntry.sifCmdHandler != 0);
		if(cmdDataEntry.sifCmdHandler != 0)
		{
			//This expects to be in an interrupt and the handler is called in the interrupt.
			//That's not the case here though, so we try for the same effect by calling the handler outside of an interrupt.
			uint32 cmdDataEntryAddr = reinterpret_cast<const uint8*>(&cmdDataEntry) - m_ram;
			m_bios.TriggerCallback(m_sifExecCmdHandlerAddr, cmdDataEntryAddr, commandHeaderAddr);
			m_bios.Reschedule();
		}
		else
		{
			FinishExecCmd();
		}
	}
	else
	{
		assert(false);
		FinishExecCmd();
	}
}

int32 CSifCmd::SifGetSreg(uint32 regIndex)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFGETSREG "(regIndex = %d);\r\n",
		regIndex);
	assert(regIndex < MAX_SREG);
	if(regIndex >= MAX_SREG)
	{
		return 0;
	}
	uint32 result = reinterpret_cast<uint32*>(m_ram + m_sregAddr)[regIndex];
	return result;
}

uint32 CSifCmd::SifSetCmdBuffer(uint32 data, uint32 length)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFSETCMDBUFFER "(data = 0x%0.8X, length = %d);\r\n",
		data, length);

	uint32 originalBuffer = m_usrCmdBuffer;
	m_usrCmdBuffer = data;
	m_usrCmdBufferLen = length;

	return originalBuffer;
}

void CSifCmd::SifAddCmdHandler(uint32 pos, uint32 handler, uint32 data)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFADDCMDHANDLER "(pos = 0x%0.8X, handler = 0x%0.8X, data = 0x%0.8X);\r\n",
		pos, handler, data);

	bool isSystemCommand = (pos & SYSTEM_COMMAND_ID) != 0;
	uint32 cmd = pos & ~SYSTEM_COMMAND_ID;
	uint32 cmdBuffer = isSystemCommand ? m_sysCmdBuffer : m_usrCmdBuffer;
	uint32 cmdBufferLen = isSystemCommand ? MAX_SYSTEM_COMMAND : m_usrCmdBufferLen;

	if((cmdBuffer != 0) && (cmd < cmdBufferLen))
	{
		auto& cmdDataEntry = reinterpret_cast<SIFCMDDATA*>(m_ram + cmdBuffer)[cmd];
		cmdDataEntry.sifCmdHandler = handler;
		cmdDataEntry.data = data;
	}
	else
	{
		CLog::GetInstance().Print(LOG_NAME, "SifAddCmdHandler - error command buffer too small or not set.\r\n");
	}
}

uint32 CSifCmd::SifSendCmd(uint32 commandId, uint32 packetPtr, uint32 packetSize, uint32 srcExtraPtr, uint32 dstExtraPtr, uint32 sizeExtra)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFSENDCMD "(commandId = 0x%0.8X, packetPtr = 0x%0.8X, packetSize = 0x%0.8X, srcExtraPtr = 0x%0.8X, dstExtraPtr = 0x%0.8X, sizeExtra = 0x%0.8X);\r\n",
		commandId, packetPtr, packetSize, srcExtraPtr, dstExtraPtr, sizeExtra);

	assert(packetSize >= 0x10);

	uint8* packetData = m_ram + packetPtr;
	auto header = reinterpret_cast<SIFCMDHEADER*>(packetData);
	header->commandId = commandId;
	header->size = packetSize;
	header->dest = 0;
	m_sifMan.SendPacket(packetData, packetSize);

	if(sizeExtra != 0 && srcExtraPtr != 0 && dstExtraPtr != 0)
	{
		auto dmaReg = reinterpret_cast<SIFDMAREG*>(m_ram + m_sendCmdExtraStructAddr);
		dmaReg->srcAddr = srcExtraPtr;
		dmaReg->dstAddr = dstExtraPtr;
		dmaReg->size = sizeExtra;
		dmaReg->flags = 0;

		m_sifMan.SifSetDma(m_sendCmdExtraStructAddr, 1);
	}

	return 1;
}

uint32 CSifCmd::SifBindRpc(uint32 clientDataAddr, uint32 serverId, uint32 mode)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFBINDRPC "(clientDataAddr = 0x%0.8X, serverId = 0x%0.8X, mode = 0x%0.8X);\r\n",
		clientDataAddr, serverId, mode);

	//Could be in non waiting mode
	assert(mode == 0);

	auto clientData = reinterpret_cast<SIFRPCCLIENTDATA*>(m_ram + clientDataAddr);
	clientData->serverDataAddr = serverId;
	clientData->header.semaId = m_bios.CreateSemaphore(0, 1);
	m_bios.WaitSemaphore(clientData->header.semaId);

	SIFRPCBIND bindPacket;
	memset(&bindPacket, 0, sizeof(SIFRPCBIND));
	bindPacket.header.commandId	= SIF_CMD_BIND;
	bindPacket.header.size		= sizeof(SIFRPCBIND);
	bindPacket.serverId			= serverId;
	bindPacket.clientDataAddr	= clientDataAddr;
	m_sifMan.SendPacket(&bindPacket, sizeof(bindPacket));

	return 0;
}

void CSifCmd::SifCallRpc(CMIPS& context)
{
	uint32 clientDataAddr	= context.m_State.nGPR[CMIPS::A0].nV0;
	uint32 rpcNumber		= context.m_State.nGPR[CMIPS::A1].nV0;
	uint32 mode				= context.m_State.nGPR[CMIPS::A2].nV0;
	uint32 sendAddr			= context.m_State.nGPR[CMIPS::A3].nV0;
	uint32 sendSize			= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x10);
	uint32 recvAddr			= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x14);
	uint32 recvSize			= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x18);
	uint32 endFctAddr		= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x1C);
	uint32 endParam			= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x20);

	assert(mode == 0);

	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFCALLRPC 
		"(clientDataAddr = 0x%0.8X, rpcNumber = 0x%0.8X, mode = 0x%0.8X, sendAddr = 0x%0.8X, sendSize = 0x%0.8X, "
		"recvAddr = 0x%0.8X, recvSize = 0x%0.8X, endFctAddr = 0x%0.8X, endParam = 0x%0.8X);\r\n",
		clientDataAddr, rpcNumber, mode, sendAddr, sendSize, recvAddr, recvSize, endFctAddr, endParam);

	auto clientData = reinterpret_cast<SIFRPCCLIENTDATA*>(m_ram + clientDataAddr);
	assert(clientData->serverDataAddr != 0);
	clientData->endFctPtr = endFctAddr;
	clientData->endParam = endParam;
	clientData->header.semaId = m_bios.CreateSemaphore(0, 1);
	m_bios.WaitSemaphore(clientData->header.semaId);

	{
		auto dmaReg = reinterpret_cast<SIFDMAREG*>(m_ram + m_sendCmdExtraStructAddr);
		dmaReg->srcAddr = sendAddr;
		dmaReg->dstAddr = clientData->buffPtr;
		dmaReg->size = sendSize;
		dmaReg->flags = 0;

		m_sifMan.SifSetDma(m_sendCmdExtraStructAddr, 1);
	}

	SIFRPCCALL callPacket;
	memset(&callPacket, 0, sizeof(SIFRPCCALL));
	callPacket.header.commandId	= SIF_CMD_CALL;
	callPacket.header.size		= sizeof(SIFRPCCALL);
	callPacket.rpcNumber		= rpcNumber;
	callPacket.sendSize			= sendSize;
	callPacket.recv				= recvAddr;
	callPacket.recvSize			= recvSize;
	callPacket.recvMode			= 1;
	callPacket.clientDataAddr	= clientDataAddr;
	callPacket.serverDataAddr	= clientData->serverDataAddr;

	m_sifMan.SendPacket(&callPacket, sizeof(callPacket));

	context.m_State.nGPR[CMIPS::V0].nD0 = 0;
}

void CSifCmd::SifRegisterRpc(CMIPS& context)
{
	uint32 serverDataAddr	= context.m_State.nGPR[CMIPS::A0].nV0;
	uint32 serverId			= context.m_State.nGPR[CMIPS::A1].nV0;
	uint32 function			= context.m_State.nGPR[CMIPS::A2].nV0;
	uint32 buffer			= context.m_State.nGPR[CMIPS::A3].nV0;
	uint32 cfunction		= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x10);
	uint32 cbuffer			= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x14);
	uint32 queueAddr		= context.m_pMemoryMap->GetWord(context.m_State.nGPR[CMIPS::SP].nV0 + 0x18);

	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFREGISTERRPC "(serverData = 0x%0.8X, serverId = 0x%0.8X, function = 0x%0.8X, buffer = 0x%0.8X, cfunction = 0x%0.8X, cbuffer = 0x%0.8X, queue = 0x%0.8X);\r\n",
		serverDataAddr, serverId, function, buffer, cfunction, cbuffer, queueAddr);

	bool moduleRegistered = m_sifMan.IsModuleRegistered(serverId);
	if(!moduleRegistered)
	{
		CSifDynamic* module = new CSifDynamic(*this, serverDataAddr);
		m_servers.push_back(module);
		m_sifMan.RegisterModule(serverId, module);
	}

	if(serverDataAddr != 0)
	{
		SIFRPCSERVERDATA* serverData = reinterpret_cast<SIFRPCSERVERDATA*>(&m_ram[serverDataAddr]);
		serverData->serverId	= serverId;
		serverData->function	= function;
		serverData->buffer		= buffer;
		serverData->cfunction	= cfunction;
		serverData->cbuffer		= cbuffer;
		serverData->queueAddr	= queueAddr;
	}

	if(queueAddr != 0)
	{
		auto queueData = reinterpret_cast<SIFRPCQUEUEDATA*>(m_ram + queueAddr);
		assert(queueData->serverDataStart == 0);
		queueData->serverDataStart = serverDataAddr;
	}

	context.m_State.nGPR[CMIPS::V0].nD0 = 0;
}

void CSifCmd::SifSetRpcQueue(uint32 queueDataAddr, uint32 threadId)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFSETRPCQUEUE "(queueData = 0x%0.8X, threadId = %d);\r\n",
		queueDataAddr, threadId);

	if(queueDataAddr != 0)
	{
		auto queueData = reinterpret_cast<SIFRPCQUEUEDATA*>(m_ram + queueDataAddr);
		queueData->threadId = threadId;
	}
}

uint32 CSifCmd::SifGetNextRequest(uint32 queueDataAddr)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFGETNEXTREQUEST "(queueData = 0x%0.8X);\r\n",
		queueDataAddr);

	uint32 result = 0;
	if(queueDataAddr != 0)
	{
		auto queueData = reinterpret_cast<SIFRPCQUEUEDATA*>(m_ram + queueDataAddr);
		result = queueData->serverDataLink;
		queueData->serverDataLink = 0;
	}
	return result;
}

void CSifCmd::SifExecRequest(CMIPS& context)
{
	uint32 serverDataAddr = context.m_State.nGPR[CMIPS::A0].nV0;
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFEXECREQUEST "(serverData = 0x%0.8X);\r\n",
		serverDataAddr);
	context.m_State.nPC = m_sifExecRequestAddr;
}

uint32 CSifCmd::SifCheckStatRpc(uint32 clientDataAddress)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFCHECKSTATRPC "(clientData = 0x%0.8X);\r\n",
		clientDataAddress);
	return 0;
}

void CSifCmd::SifRpcLoop(CMIPS& context)
{
	uint32 queueAddr = context.m_State.nGPR[CMIPS::A0].nV0;
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFRPCLOOP "(queue = 0x%0.8X);\r\n",
		queueAddr);
	context.m_State.nPC = m_sifRpcLoopAddr;
}

uint32 CSifCmd::SifGetOtherData(uint32 packetPtr, uint32 src, uint32 dst, uint32 size, uint32 mode)
{
	CLog::GetInstance().Print(LOG_NAME, FUNCTION_SIFGETOTHERDATA "(packetPtr = 0x%0.8X, src = 0x%0.8X, dst = 0x%0.8X, size = 0x%0.8X, mode = %d);\r\n",
		packetPtr, src, dst, size, mode);
	m_sifMan.GetOtherData(dst, src, size);
	return 0;
}

void CSifCmd::SleepThread()
{
	m_bios.SleepThread();
}
