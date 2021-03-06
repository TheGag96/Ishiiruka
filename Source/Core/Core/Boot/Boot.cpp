// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/Boot/Boot.h"

#include <string>
#include <vector>

#include <zlib.h>

#include "Common/Align.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/Boot/Boot_DOL.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/IOS/IPC.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"

#include "DiscIO/Enums.h"
#include "DiscIO/NANDContentLoader.h"
#include "DiscIO/Volume.h"

bool CBoot::DVDRead(u64 dvd_offset, u32 output_address, u32 length, bool decrypt)
{
	std::vector<u8> buffer(length);
	if (!DVDInterface::GetVolume().Read(dvd_offset, length, buffer.data(), decrypt))
		return false;
	Memory::CopyToEmu(output_address, buffer.data(), length);
	return true;
}

void CBoot::Load_FST(bool is_wii)
{
	if (!DVDInterface::IsDiscInside())
		return;

	const DiscIO::IVolume& volume = DVDInterface::GetVolume();

	// copy first 32 bytes of disc to start of Mem 1
	DVDRead(/*offset*/ 0, /*address*/ 0, /*length*/ 0x20, false);

	// copy of game id
	Memory::Write_U32(Memory::Read_U32(0x0000), 0x3180);

	u32 shift = 0;
	if (is_wii)
		shift = 2;

	u32 fst_offset = 0;
	u32 fst_size = 0;
	u32 max_fst_size = 0;

	volume.ReadSwapped(0x0424, &fst_offset, is_wii);
	volume.ReadSwapped(0x0428, &fst_size, is_wii);
	volume.ReadSwapped(0x042c, &max_fst_size, is_wii);

	u32 arena_high = Common::AlignDown(0x817FFFFF - (max_fst_size << shift), 0x20);
	Memory::Write_U32(arena_high, 0x00000034);

	// load FST
	DVDRead(fst_offset << shift, arena_high, fst_size << shift, is_wii);
	Memory::Write_U32(arena_high, 0x00000038);
	Memory::Write_U32(max_fst_size << shift, 0x0000003c);
}

void CBoot::UpdateDebugger_MapLoaded()
{
	Host_NotifyMapLoaded();
}

bool CBoot::FindMapFile(std::string* existing_map_file, std::string* writable_map_file,
	std::string* title_id)
{
	std::string title_id_str;
	size_t name_begin_index;

	SConfig& _StartupPara = SConfig::GetInstance();
	switch (_StartupPara.m_BootType)
	{
	case SConfig::BOOT_WII_NAND:
	{
		const DiscIO::CNANDContentLoader& Loader =
			DiscIO::CNANDContentManager::Access().GetNANDLoader(_StartupPara.m_strFilename);
		if (Loader.IsValid())
		{
			u64 TitleID = Loader.GetTMD().GetTitleId();
			title_id_str = StringFromFormat("%08X_%08X", (u32)(TitleID >> 32) & 0xFFFFFFFF,
				(u32)TitleID & 0xFFFFFFFF);
		}
		break;
	}

	case SConfig::BOOT_ELF:
	case SConfig::BOOT_DOL:
		// Strip the .elf/.dol file extension and directories before the name
		name_begin_index = _StartupPara.m_strFilename.find_last_of("/") + 1;
		if ((_StartupPara.m_strFilename.find_last_of("\\") + 1) > name_begin_index)
		{
			name_begin_index = _StartupPara.m_strFilename.find_last_of("\\") + 1;
		}
		title_id_str = _StartupPara.m_strFilename.substr(
			name_begin_index, _StartupPara.m_strFilename.size() - 4 - name_begin_index);
		break;

	default:
		title_id_str = _StartupPara.GetGameID();
		break;
	}

	if (writable_map_file)
		*writable_map_file = File::GetUserPath(D_MAPS_IDX) + title_id_str + ".map";

	if (title_id)
		*title_id = title_id_str;

	bool found = false;
	static const std::string maps_directories[] = { File::GetUserPath(D_MAPS_IDX),
		File::GetSysDirectory() + MAPS_DIR DIR_SEP };
	for (size_t i = 0; !found && i < ArraySize(maps_directories); ++i)
	{
		std::string path = maps_directories[i] + title_id_str + ".map";
		if (File::Exists(path))
		{
			found = true;
			if (existing_map_file)
				*existing_map_file = path;
		}
	}

	return found;
}

bool CBoot::LoadMapFromFilename()
{
	std::string strMapFilename;
	bool found = FindMapFile(&strMapFilename, nullptr);
	if (found && g_symbolDB.LoadMap(strMapFilename))
	{
		UpdateDebugger_MapLoaded();
		return true;
	}

	return false;
}

// If ipl.bin is not found, this function does *some* of what BS1 does:
// loading IPL(BS2) and jumping to it.
// It does not initialize the hardware or anything else like BS1 does.
bool CBoot::Load_BS2(const std::string& boot_rom_filename)
{
	// CRC32 hashes of the IPL file; including source where known
	// https://forums.dolphin-emu.org/Thread-unknown-hash-on-ipl-bin?pid=385344#pid385344
	constexpr u32 USA_v1_0 = 0x6D740AE7;
	// https://forums.dolphin-emu.org/Thread-unknown-hash-on-ipl-bin?pid=385334#pid385334
	constexpr u32 USA_v1_1 = 0xD5E6FEEA;
	// https://forums.dolphin-emu.org/Thread-unknown-hash-on-ipl-bin?pid=385399#pid385399
	constexpr u32 USA_v1_2 = 0x86573808;
	// GameCubes sold in Brazil have this IPL. Same as USA v1.2 but localized
	constexpr u32 BRA_v1_0 = 0x667D0B64;
	// Redump
	constexpr u32 JAP_v1_0 = 0x6DAC1F2A;
	// https://bugs.dolphin-emu.org/issues/8936
	constexpr u32 JAP_v1_1 = 0xD235E3F9;
	// Redump
	constexpr u32 PAL_v1_0 = 0x4F319F43;
	// https://forums.dolphin-emu.org/Thread-ipl-with-unknown-hash-dd8cab7c-problem-caused-by-my-pal-gamecube-bios?pid=435463#pid435463
	constexpr u32 PAL_v1_1 = 0xDD8CAB7C;
	// Redump
	constexpr u32 PAL_v1_2 = 0xAD1B7F16;

	// Load the whole ROM dump
	std::string data;
	if (!File::ReadFileToString(boot_rom_filename, data))
		return false;

	// Use zlibs crc32 implementation to compute the hash
	u32 ipl_hash = crc32(0L, Z_NULL, 0);
	ipl_hash = crc32(ipl_hash, (const Bytef*)data.data(), (u32)data.size());
	DiscIO::Region ipl_region;
	switch (ipl_hash)
	{
	case USA_v1_0:
	case USA_v1_1:
	case USA_v1_2:
	case BRA_v1_0:
		ipl_region = DiscIO::Region::NTSC_U;
		break;
	case JAP_v1_0:
	case JAP_v1_1:
		ipl_region = DiscIO::Region::NTSC_J;
		break;
	case PAL_v1_0:
	case PAL_v1_1:
	case PAL_v1_2:
		ipl_region = DiscIO::Region::PAL;
		break;
	default:
		PanicAlertT("IPL with unknown hash %x", ipl_hash);
		ipl_region = DiscIO::Region::UNKNOWN_REGION;
		break;
	}

	const DiscIO::Region boot_region = SConfig::GetInstance().m_region;
	if (ipl_region != DiscIO::Region::UNKNOWN_REGION && boot_region != ipl_region)
		PanicAlertT("%s IPL found in %s directory. The disc might not be recognized",
			SConfig::GetDirectoryForRegion(ipl_region),
			SConfig::GetDirectoryForRegion(boot_region));

	// Run the descrambler over the encrypted section containing BS1/BS2
	ExpansionInterface::CEXIIPL::Descrambler((u8*)data.data() + 0x100, 0x1AFE00);

	// TODO: Execution is supposed to start at 0xFFF00000, not 0x81200000;
	// copying the initial boot code to 0x81200000 is a hack.
	// For now, HLE the first few instructions and start at 0x81200150
	// to work around this.
	Memory::CopyToEmu(0x01200000, data.data() + 0x100, 0x700);
	Memory::CopyToEmu(0x01300000, data.data() + 0x820, 0x1AFE00);
	PowerPC::ppcState.gpr[3] = 0xfff0001f;
	PowerPC::ppcState.gpr[4] = 0x00002030;
	PowerPC::ppcState.gpr[5] = 0x0000009c;
	PowerPC::ppcState.msr = 0x00002030;
	PowerPC::ppcState.spr[SPR_HID0] = 0x0011c464;
	PowerPC::ppcState.spr[SPR_IBAT0U] = 0x80001fff;
	PowerPC::ppcState.spr[SPR_IBAT0L] = 0x00000002;
	PowerPC::ppcState.spr[SPR_IBAT3U] = 0xfff0001f;
	PowerPC::ppcState.spr[SPR_IBAT3L] = 0xfff00001;
	PowerPC::ppcState.spr[SPR_DBAT0U] = 0x80001fff;
	PowerPC::ppcState.spr[SPR_DBAT0L] = 0x00000002;
	PowerPC::ppcState.spr[SPR_DBAT1U] = 0xc0001fff;
	PowerPC::ppcState.spr[SPR_DBAT1L] = 0x0000002a;
	PowerPC::ppcState.spr[SPR_DBAT3U] = 0xfff0001f;
	PowerPC::ppcState.spr[SPR_DBAT3L] = 0xfff00001;
	PowerPC::DBATUpdated();
	PowerPC::IBATUpdated();
	PC = 0x81200150;
	return true;
}

// Third boot step after BootManager and Core. See Call schedule in BootManager.cpp
bool CBoot::BootUp()
{
	SConfig& _StartupPara = SConfig::GetInstance();

	NOTICE_LOG(BOOT, "Booting %s", _StartupPara.m_strFilename.c_str());

	g_symbolDB.Clear();

	// PAL Wii uses NTSC framerate and linecount in 60Hz modes
	VideoInterface::Preset(DiscIO::IsNTSC(_StartupPara.m_region) ||
		(_StartupPara.bWii && _StartupPara.bPAL60));

	switch (_StartupPara.m_BootType)
	{
		// GCM and Wii
	case SConfig::BOOT_ISO:
	{
		DVDInterface::SetVolumeName(_StartupPara.m_strFilename);
		if (!DVDInterface::IsDiscInside())
			return false;

		const DiscIO::IVolume& pVolume = DVDInterface::GetVolume();

		if ((pVolume.GetVolumeType() == DiscIO::Platform::WII_DISC) != _StartupPara.bWii)
		{
			PanicAlertT("Warning - starting ISO in wrong console mode!");
		}

		_StartupPara.bWii = pVolume.GetVolumeType() == DiscIO::Platform::WII_DISC;

		// HLE BS2 or not
		if (_StartupPara.bHLE_BS2)
		{
			EmulatedBS2(_StartupPara.bWii);
		}
		else if (!Load_BS2(_StartupPara.m_strBootROM))
		{
			// If we can't load the bootrom file we HLE it instead
			EmulatedBS2(_StartupPara.bWii);
		}

		PatchEngine::LoadPatches();

		// Scan for common HLE functions
		if (_StartupPara.bHLE_BS2 && !_StartupPara.bEnableDebugging)
		{
			PPCAnalyst::FindFunctions(0x80004000, 0x811fffff, &g_symbolDB);
			SignatureDB db;
			if (db.Load(File::GetSysDirectory() + TOTALDB))
			{
				db.Apply(&g_symbolDB);
				HLE::PatchFunctions();
				db.Clear();
			}
		}

		// Try to load the symbol map if there is one, and then scan it for
		// and eventually replace code
		if (LoadMapFromFilename())
			HLE::PatchFunctions();

		break;
	}

	// DOL
	case SConfig::BOOT_DOL:
	{
		CDolLoader dolLoader(_StartupPara.m_strFilename);
		if (!dolLoader.IsValid())
			return false;

		// Check if we have gotten a Wii file or not
		bool dolWii = dolLoader.IsWii();
		if (dolWii != _StartupPara.bWii)
		{
			PanicAlertT("Warning - starting DOL in wrong console mode!");
		}

		if (!_StartupPara.m_strDVDRoot.empty())
		{
			NOTICE_LOG(BOOT, "Setting DVDRoot %s", _StartupPara.m_strDVDRoot.c_str());
			DVDInterface::SetVolumeDirectory(_StartupPara.m_strDVDRoot, dolWii,
				_StartupPara.m_strApploader, _StartupPara.m_strFilename);
		}
		else if (!_StartupPara.m_strDefaultISO.empty())
		{
			NOTICE_LOG(BOOT, "Loading default ISO %s", _StartupPara.m_strDefaultISO.c_str());
			DVDInterface::SetVolumeName(_StartupPara.m_strDefaultISO);
		}

		if (!EmulatedBS2(dolWii))
		{
			// Set up MSR and the BAT SPR registers.
			UReg_MSR& m_MSR = ((UReg_MSR&)PowerPC::ppcState.msr);
			m_MSR.FP = 1;
			m_MSR.DR = 1;
			m_MSR.IR = 1;
			m_MSR.EE = 1;
			PowerPC::ppcState.spr[SPR_IBAT0U] = 0x80001fff;
			PowerPC::ppcState.spr[SPR_IBAT0L] = 0x00000002;
			PowerPC::ppcState.spr[SPR_IBAT4U] = 0x90001fff;
			PowerPC::ppcState.spr[SPR_IBAT4L] = 0x10000002;
			PowerPC::ppcState.spr[SPR_DBAT0U] = 0x80001fff;
			PowerPC::ppcState.spr[SPR_DBAT0L] = 0x00000002;
			PowerPC::ppcState.spr[SPR_DBAT1U] = 0xc0001fff;
			PowerPC::ppcState.spr[SPR_DBAT1L] = 0x0000002a;
			PowerPC::ppcState.spr[SPR_DBAT4U] = 0x90001fff;
			PowerPC::ppcState.spr[SPR_DBAT4L] = 0x10000002;
			PowerPC::ppcState.spr[SPR_DBAT5U] = 0xd0001fff;
			PowerPC::ppcState.spr[SPR_DBAT5L] = 0x1000002a;
			if (dolLoader.IsWii())
				HID4.SBE = 1;
			PowerPC::DBATUpdated();
			PowerPC::IBATUpdated();

			// Because there is no TMD to get the requested system (IOS) version from,
			// we default to IOS58, which is the version used by the Homebrew Channel.
			if (dolLoader.IsWii())
				SetupWiiMemory(0x000000010000003a);

			dolLoader.Load();
			PC = dolLoader.GetEntryPoint();
		}

		if (LoadMapFromFilename())
			HLE::PatchFunctions();

		break;
	}

	// ELF
	case SConfig::BOOT_ELF:
	{
		// load image or create virtual drive from directory
		if (!_StartupPara.m_strDVDRoot.empty())
		{
			NOTICE_LOG(BOOT, "Setting DVDRoot %s", _StartupPara.m_strDVDRoot.c_str());
			DVDInterface::SetVolumeDirectory(_StartupPara.m_strDVDRoot, _StartupPara.bWii);
		}
		else if (!_StartupPara.m_strDefaultISO.empty())
		{
			NOTICE_LOG(BOOT, "Loading default ISO %s", _StartupPara.m_strDefaultISO.c_str());
			DVDInterface::SetVolumeName(_StartupPara.m_strDefaultISO);
		}

		// Poor man's bootup
		if (_StartupPara.bWii)
		{
			// Because there is no TMD to get the requested system (IOS) version from,
			// we default to IOS58, which is the version used by the Homebrew Channel.
			SetupWiiMemory(0x000000010000003a);
		}
		else
		{
			EmulatedBS2_GC(true);
		}

		Load_FST(_StartupPara.bWii);
		if (!Boot_ELF(_StartupPara.m_strFilename))
			return false;

		UpdateDebugger_MapLoaded();
		Dolphin_Debugger::AddAutoBreakpoints();
		break;
	}

	// Wii WAD
	case SConfig::BOOT_WII_NAND:
		Boot_WiiWAD(_StartupPara.m_strFilename);

		PatchEngine::LoadPatches();

		if (LoadMapFromFilename())
			HLE::PatchFunctions();

		// load default image or create virtual drive from directory
		if (!_StartupPara.m_strDVDRoot.empty())
			DVDInterface::SetVolumeDirectory(_StartupPara.m_strDVDRoot, true);
		else if (!_StartupPara.m_strDefaultISO.empty())
			DVDInterface::SetVolumeName(_StartupPara.m_strDefaultISO);

		break;

		// Bootstrap 2 (AKA: Initial Program Loader, "BIOS")
	case SConfig::BOOT_BS2:
	{
		if (Load_BS2(_StartupPara.m_strBootROM))
		{
			if (LoadMapFromFilename())
				HLE::PatchFunctions();
		}
		else
		{
			return false;
		}
		break;
	}

	case SConfig::BOOT_DFF:
		// do nothing
		break;

	default:
	{
		PanicAlertT("Tried to load an unknown file type.");
		return false;
	}
	}

	HLE::PatchFixedFunctions();
	return true;
}
