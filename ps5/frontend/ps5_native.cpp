// Crux2 - native modules (ps5_native.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_native.h"
#include "ps5_priority.h"
#include "ps5_native_loader.h"
#include "ps5_platform.h"
#include "../recomp/runtime/crux2_native_abi.h"

#include "Config.h"
#include "Host.h"
#include "R5900.h"
#include "VMManager.h"
#include "vtlb.h"
#include "CDVD/IsoReader.h"
#include "ps5_backup.h"
#include "vfs/ps5_vfs.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/FastJmp.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>

// iR5900.cpp: watches a range's pages for writes the way it protects pages it compiled code from.
extern int recNativeWatch(u32 start, u32 bytes);
// Interpreter.cpp: PCSX2's interpreter from cpuRegs.pc to stop_pc, with no events (the check).
extern bool intCrux2Replay(u32 stop_pc, u64 target, u64 slack, u64 cap, u32 lo, u32 hi, u64* steps);

namespace PS5Native
{
	namespace
	{
		struct Api
		{
			Crux2NativeInitFn init = nullptr;
			Crux2NativeHasFn has = nullptr;
			Crux2NativeRunFn run = nullptr;
			Crux2NativeInvalidateFn invalidate = nullptr;
			Crux2NativeStatsFn stats = nullptr;
		};

		std::unique_ptr<PS5NativeLoader::Module> s_module;
		std::string s_module_name; // the file it came from
		Api s_api;
		const Crux2NativeInfo* s_info = nullptr;
		bool s_active = false;
		Crux2NativeHost s_host{};
		alignas(16) Crux2NativeState s_state{};

		// Addresses where the module's code gave control straight back (its first instruction is
		// the emulator's): compiled the usual way from then on, until PCSX2's recompiler resets.
		std::unordered_set<u32> s_refused;

		// Counts for the log.
		u64 s_runs = 0, s_instructions = 0, s_budget_exits = 0, s_host_exits = 0, s_traps = 0;

		// ---- the check ([PS5] NativeCheck): every run of native code is made again by PCSX2's
		// interpreter from the same registers and memory, and the two compared. The native run
		// writes into a shadow of memory (s_shadow) instead of memory, so the interpreter starts
		// from memory as it was; the interpreter's result is the one kept, so a game runs as PCSX2
		// runs it while the log names each function whose native code came out different. A run
		// that touches a hardware register is not checked (the interpreter would touch it twice):
		// that address is left to PCSX2 from then on (v1.3.6; it ran natively unchecked before, so
		// a fault in such code could not show). Slow: for finding what is wrong, not for playing.
		bool s_check = false;
		std::vector<uintptr_t> s_check_vmap; // every page a handler's: the module asks for each access
		struct ShadowByte
		{
			u8 value;
			u32 address;
		};
		std::unordered_map<uintptr_t, ShadowByte> s_shadow; // by the byte's address in the host
		fastjmp_buf s_check_jump;
		std::unordered_set<u32> s_reported; // functions already named in the log
		u64 s_checked = 0, s_differed = 0, s_unchecked_io = 0, s_unchecked_long = 0, s_lost = 0;
		constexpr int64_t kCheckBudget = 200000;
		// Each address native code is entered at is checked this many times, then trusted: every run
		// checked made the game too slow to reach its menu.
		constexpr u32 kChecksPerEntry = 8;
		std::unordered_map<u32, u32> s_check_counts;

		// ---- [PS5] NativeHardware: false leaves each address whose native run touched a hardware
		// register (the EE's and GS's registers, the IOP's memory, the BIOS) to PCSX2 from then on.
		// The run that found it out is kept; the next one at that address is PCSX2's.
		bool s_native_hardware = true;
		bool s_touched_hardware = false;
		u64 s_hardware_refused = 0;
		std::unordered_set<u32> s_hardware_pcs; // kept over a reset (a state loaded): the same code

		inline void NoteAddress(u32 a)
		{
			const u32 segment = a >> 28;
			if ((a & 0x1FFFFFFFu) >= 0x10000000u && segment != 3 && segment != 7)
				s_touched_hardware = true;
		}

		// ---- what the module may call: PCSX2's own memory functions

		u8 Read8(u32 a) { NoteAddress(a); return vtlb_memRead<mem8_t>(a); }
		u16 Read16(u32 a) { NoteAddress(a); return vtlb_memRead<mem16_t>(a); }
		u32 Read32(u32 a) { NoteAddress(a); return vtlb_memRead<mem32_t>(a); }
		u64 Read64(u32 a) { NoteAddress(a); return vtlb_memRead<mem64_t>(a); }
		void Read128(u32 a, void* out)
		{
			NoteAddress(a);
			const r128 v = vtlb_memRead128(a);
			std::memcpy(out, &v, 16);
		}
		void Write8(u32 a, u8 v) { NoteAddress(a); vtlb_memWrite<mem8_t>(a, v); }
		void Write16(u32 a, u16 v) { NoteAddress(a); vtlb_memWrite<mem16_t>(a, v); }
		void Write32(u32 a, u32 v) { NoteAddress(a); vtlb_memWrite<mem32_t>(a, v); }
		void Write64(u32 a, u64 v) { NoteAddress(a); vtlb_memWrite<mem64_t>(a, v); }
		void Write128(u32 a, const void* in)
		{
			NoteAddress(a);
			r128 v;
			std::memcpy(&v, in, 16);
			vtlb_memWrite128(a, v);
		}
		void Log(const char* line) { ps5px_log("native: %s", line); }
		int Watch(u32 start, u32 bytes) { return recNativeWatch(start, bytes); }

		// The check's memory: what the native run wrote, over memory as it is. A hardware page ends
		// the run at once (back in Run, which runs it natively instead).
		uintptr_t HostByte(u32 a)
		{
			const uintptr_t* map = reinterpret_cast<const uintptr_t*>(vtlb_private::vtlbdata.vmap);
			const intptr_t p = static_cast<intptr_t>(map[a >> 12] + a);
			if (p < 0)
				fastjmp_jmp(&s_check_jump, 1);
			return static_cast<uintptr_t>(p);
		}
		void ShadowRead(u32 a, void* out, int n)
		{
			u8* o = static_cast<u8*>(out);
			for (int i = 0; i < n; i++)
			{
				const uintptr_t h = HostByte(a + i);
				const auto it = s_shadow.find(h);
				o[i] = (it != s_shadow.end()) ? it->second.value : *reinterpret_cast<const u8*>(h);
			}
		}
		void ShadowWrite(u32 a, const void* in, int n)
		{
			const u8* v = static_cast<const u8*>(in);
			for (int i = 0; i < n; i++)
				s_shadow[HostByte(a + i)] = ShadowByte{v[i], a + static_cast<u32>(i)};
		}
		template <typename T>
		T SRead(u32 a)
		{
			T v;
			ShadowRead(a, &v, sizeof(T));
			return v;
		}
		template <typename T>
		void SWrite(u32 a, T v)
		{
			ShadowWrite(a, &v, sizeof(T));
		}
		void SRead128(u32 a, void* out) { ShadowRead(a, out, 16); }
		void SWrite128(u32 a, const void* in) { ShadowWrite(a, in, 16); }

		void UseShadow(bool shadow)
		{
			if (shadow)
			{
				s_host.vmap = s_check_vmap.data();
				s_host.read8 = SRead<u8>;
				s_host.read16 = SRead<u16>;
				s_host.read32 = SRead<u32>;
				s_host.read64 = SRead<u64>;
				s_host.read128 = SRead128;
				s_host.write8 = SWrite<u8>;
				s_host.write16 = SWrite<u16>;
				s_host.write32 = SWrite<u32>;
				s_host.write64 = SWrite<u64>;
				s_host.write128 = SWrite128;
			}
			else
			{
				s_host.vmap = reinterpret_cast<const uintptr_t*>(vtlb_private::vtlbdata.vmap);
				s_host.read8 = Read8;
				s_host.read16 = Read16;
				s_host.read32 = Read32;
				s_host.read64 = Read64;
				s_host.read128 = Read128;
				s_host.write8 = Write8;
				s_host.write16 = Write16;
				s_host.write32 = Write32;
				s_host.write64 = Write64;
				s_host.write128 = Write128;
			}
		}

		// ---- cycles: what PCSX2's recompiler would add for a block of `count` plain instructions

		// (PCSX2 counts eighths of a cycle, 9 for most instructions, twice that with the
		// instruction cache off, and scales a block's total by the EE cycle rate; scaleblockcycles
		// in iR5900.cpp. The module counts instructions only, so loads, multiplies and divisions
		// count as plain ones: the game sees a slightly faster processor. Needs proper testing
		// against games' timing.)
		u64 Cycles(u64 count)
		{
			const u64 eighths = count * 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
			const s8 rate = EmuConfig.Speedhacks.EECycleRate;
			u64 cycles;
			if (rate == 0 || eighths <= 40 || rate < -99 || rate > 3)
				cycles = eighths >> 3;
			else if (rate > 1)
				cycles = eighths >> (2 + rate);
			else if (rate == 1)
				cycles = static_cast<u64>(static_cast<double>(eighths >> 3) / 1.3);
			else if (rate == -1)
				cycles = ((eighths <= 80 || eighths > 168) ? 5 : 7) * eighths / 32;
			else
				cycles = ((5 + (-2 * (rate + 1))) * eighths) >> 5;
			return std::max<u64>(cycles, 1);
		}

		void Unload()
		{
			if (s_module && s_api.stats)
				ps5px_log("native: %s", Summary().c_str());
			s_active = false;
			s_info = nullptr;
			s_api = {};
			s_module.reset();
			s_module_name.clear();
			s_refused.clear();
			s_runs = s_instructions = s_budget_exits = s_host_exits = s_traps = 0;
			s_checked = s_differed = s_unchecked_io = s_unchecked_long = s_lost = 0;
			s_hardware_refused = 0;
			s_hardware_pcs.clear();
			s_reported.clear();
			s_shadow.clear();
			s_check_counts.clear();
		}

		// "cdrom0:\SLUS_213.76;1" -> "SLUS_213.76"
		std::string ExecutableName(std::string_view elf)
		{
			const size_t slash = elf.find_last_of("\\/:");
			if (slash != std::string_view::npos)
				elf = elf.substr(slash + 1);
			const size_t version = elf.find(';');
			if (version != std::string_view::npos)
				elf = elf.substr(0, version);
			return std::string(elf);
		}

		bool Load(const std::string& path)
		{
			std::string error;
			std::unique_ptr<PS5NativeLoader::Module> module = PS5NativeLoader::LoadFile(path, &error);
			if (!module)
			{
				ps5px_log("native: %s was not loaded: %s", path.c_str(), error.c_str());
				return false;
			}
			Api api{reinterpret_cast<Crux2NativeInitFn>(module->Symbol(CRUX2_NATIVE_INIT_SYMBOL)),
				reinterpret_cast<Crux2NativeHasFn>(module->Symbol(CRUX2_NATIVE_HAS_SYMBOL)),
				reinterpret_cast<Crux2NativeRunFn>(module->Symbol(CRUX2_NATIVE_RUN_SYMBOL)),
				reinterpret_cast<Crux2NativeInvalidateFn>(module->Symbol(CRUX2_NATIVE_INVALIDATE_SYMBOL)),
				reinterpret_cast<Crux2NativeStatsFn>(module->Symbol(CRUX2_NATIVE_STATS_SYMBOL))};
			if (!api.init || !api.has || !api.run || !api.invalidate || !api.stats)
			{
				ps5px_log("native: %s is not a Crux2 native module: of its five functions it has%s%s%s%s%s (%zu KiB loaded)", path.c_str(),
					api.init ? " init" : "", api.has ? " has" : "", api.run ? " run" : "", api.invalidate ? " invalidate" : "",
					api.stats ? " stats" : "", module->Size() / 1024);
				return false;
			}

			s_host = {};
			s_host.abi = CRUX2_NATIVE_ABI;
			s_host.size = sizeof(s_host);
			s_host.vmap = reinterpret_cast<const uintptr_t*>(vtlb_private::vtlbdata.vmap);
			s_host.read8 = Read8;
			s_host.read16 = Read16;
			s_host.read32 = Read32;
			s_host.read64 = Read64;
			s_host.read128 = Read128;
			s_host.write8 = Write8;
			s_host.write16 = Write16;
			s_host.write32 = Write32;
			s_host.write64 = Write64;
			s_host.write128 = Write128;
			s_host.log = Log;
			s_host.watch = Watch;
			const Crux2NativeInfo* info = api.init(&s_host);
			if (!info)
			{
				ps5px_log("native: %s was made for another version of Crux2 (rebuild it with this version's recomp-game.sh)", path.c_str());
				return false;
			}

			s_module = std::move(module);
			s_api = api;
			s_info = info;
			s_module_name = std::string(Path::GetFileName(path));
			ps5px_log("native: %s loaded: %s, %u functions, %u entries, 0x%08x-0x%08x, %s; %zu KiB", s_module_name.c_str(),
				info->executable ? info->executable : "?", info->functions, info->entries, info->text_start, info->text_end,
				info->made_by ? info->made_by : "?", s_module->Size() / 1024);
			return true;
		}
		// ---- the share: <share>/Crux2-native/executables/ and modules/ ([PS5] NativeShare)
		//
		// A game started in Crux2 puts its executable, read from the disc image, in executables/; the
		// kit's native-batch.sh on a PC makes a module for each one there and puts it in modules/;
		// the next start of the game copies a new module from there into the data folder's native/
		// and loads it. [PS5] NativeShare is a folder ("/smb/ps2", say), "off", or empty: then the
		// share the game itself is on, or else the first share network.cfg mounts.

		std::string ShareFolder()
		{
			const std::string set = Host::GetBaseStringSettingValue("PS5", "NativeShare", "");
			if (set == "off")
				return std::string();
			std::string root = set;
			if (root.empty())
			{
				const std::string disc = VMManager::GetDiscPath();
				for (unsigned i = 0;; i++)
				{
					const char* const point = ps5vfs_mount_point(i);
					if (!point)
						break;
					if (std::strncmp(point, "/smb/", 5) != 0)
						continue;
					const size_t n = std::strlen(point);
					if (disc.compare(0, n, point) == 0 && (disc.size() == n || disc[n] == '/'))
					{
						root = point; // the share the game is on
						break;
					}
					if (root.empty())
						root = point; // else the first share
				}
			}
			if (root.empty())
				return std::string();
			while (root.size() > 1 && root.back() == '/')
				root.pop_back();
			return root + "/Crux2-native";
		}

		// A module in the share's modules/ that the data folder does not have, or has older: copied
		// into the data folder's native/ before it is looked for (a module is read whole at the start).
		// (A share's path goes through Crux2's network file layer, the console's own through stat.)
		bool StatAny(const std::string& path, struct stat* st)
		{
			return (ps5vfs_is_virtual(path.c_str()) ? ps5vfs_stat(path.c_str(), st) : stat(path.c_str(), st)) == 0;
		}

		void FetchFromShare(const std::string& share, const std::string& name, const std::string& folder)
		{
			const std::string remote = share + "/modules/" + name + ".crux2native";
			struct stat rs;
			if (!StatAny(remote, &rs) || !S_ISREG(rs.st_mode))
				return;
			const std::string local = Path::Combine(folder, name + ".crux2native");
			struct stat ls;
			if (StatAny(local, &ls) && ls.st_size == rs.st_size && ls.st_mtime >= rs.st_mtime)
				return;
			std::string data;
			if (!PS5Backup::ReadWholeFile(remote, &data) || static_cast<off_t>(data.size()) != rs.st_size)
			{
				ps5px_log("native: %s could not be read from the share", remote.c_str());
				return;
			}
			FileSystem::CreateDirectoryPath(folder.c_str(), false);
			const std::string partial = local + ".partial";
			if (!PS5Backup::WriteWholeFile(partial, data) || !FileSystem::RenamePath(partial.c_str(), local.c_str()))
			{
				ps5px_log("native: %s could not be written to %s", remote.c_str(), local.c_str());
				FileSystem::DeleteFilePath(partial.c_str());
				return;
			}
			ps5px_log("native: %s.crux2native taken from %s/modules (%zu KiB)", name.c_str(), share.c_str(), data.size() / 1024);
		}

		// The game's executable, read from the disc image, into the share's executables/ for
		// native-batch.sh, when the share has no module for it and no copy of it yet. Read here, on
		// the emulation thread (the disc is PCSX2's); written on a thread of its own.
		void ExportToShare(const std::string& share, const std::string& name, const std::string& elf_path)
		{
			if (elf_path.compare(0, 6, "cdrom0") != 0 && elf_path.compare(0, 6, "cdrom:") != 0)
				return; // (a host: or BIOS executable: not from a disc)
			const std::string target = share + "/executables/" + name;
			if (PS5Backup::PathExists(share + "/modules/" + name + ".crux2native") || PS5Backup::PathExists(target + ".txt"))
				return;

			// "cdrom0:\SLUS_213.76;1" -> "SLUS_213.76" in the image's file system
			std::string iso_name = elf_path.substr(elf_path.find(':') + 1);
			while (!iso_name.empty() && (iso_name[0] == '\\' || iso_name[0] == '/'))
				iso_name.erase(0, 1);
			if (const size_t semi = iso_name.find(';'); semi != std::string::npos)
				iso_name.resize(semi);
			std::replace(iso_name.begin(), iso_name.end(), '\\', '/');

			IsoReader iso;
			std::vector<u8> bytes;
			Error error;
			if (!iso.Open(&error) || !iso.ReadFile(iso_name, &bytes, &error) || bytes.size() < 4 || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0)
			{
				ps5px_log("native: %s could not be read from the disc for the share: %s", iso_name.c_str(), error.GetDescription().c_str());
				return;
			}
			const std::string note = fmt::format("{}\t{}\t{:08X}\t{}\n", name, VMManager::GetDiscSerial(), VMManager::GetDiscCRC(),
				VMManager::GetTitle(true));
			// (The note is written last: native-batch.sh takes an executable only once its note is there.)
			std::thread([share, name, target, note, data = std::string(bytes.begin(), bytes.end())]() {
				PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
				const std::string folder = share + "/executables";
				if (!PS5Backup::MakeAllFolders(folder) || !PS5Backup::MakeAllFolders(share + "/modules") ||
					!PS5Backup::WriteWholeFile(target, data) || !PS5Backup::WriteWholeFile(target + ".txt", note))
				{
					ps5px_log("native: %s could not be put on the share: %s", name.c_str(), PS5Backup::WhyNot(errno, share).c_str());
					return;
				}
				ps5px_log("native: %s put in %s for native-batch.sh (%zu KiB)", name.c_str(), folder.c_str(), data.size() / 1024);
			}).detach();
		}
	} // namespace

	void GameChanged()
	{
		const std::string executable = ExecutableName(VMManager::GetCurrentELF());
		const std::string serial = VMManager::GetDiscSerial();
		if (!Host::GetBaseBoolSettingValue("PS5", "NativeModules", true) || (executable.empty() && serial.empty()))
		{
			if (s_module)
				Unload();
			return;
		}

		s_check = Host::GetBaseBoolSettingValue("PS5", "NativeCheck", false);
		s_native_hardware = Host::GetBaseBoolSettingValue("PS5", "NativeHardware", true);
		if (s_check && s_check_vmap.empty())
			s_check_vmap.assign(size_t{1} << 20, uintptr_t{1} << 63);

		// <data folder>/native/<executable>.crux2native, or <serial>.crux2native; a newer one from the share first.
		const std::string folder = Path::Combine(EmuFolders::DataRoot, "native");
		const std::string share = ShareFolder();
		if (!share.empty() && !executable.empty())
			FetchFromShare(share, executable, folder);
		std::string path;
		for (const std::string& name : {executable, serial})
		{
			if (name.empty())
				continue;
			const std::string candidate = Path::Combine(folder, name + ".crux2native");
			if (FileSystem::FileExists(candidate.c_str()))
			{
				path = candidate;
				break;
			}
		}

		if (s_module && Path::GetFileName(path) == s_module_name)
		{
			s_active = true; // (the same executable again: its module stays, every function checked anew)
			Reset();
			return;
		}
		if (s_module)
			Unload();
		if (path.empty())
		{
			if (!share.empty() && !executable.empty() && Host::GetBaseBoolSettingValue("PS5", "NativeExport", true))
				ExportToShare(share, executable, VMManager::GetCurrentELF());
			return;
		}
		if (!Load(path))
			return;
		if (s_info->executable && !executable.empty() && !StringUtil::compareNoCase(s_info->executable, executable))
			ps5px_log("native: made from %s, running %s: only the functions whose bytes are the same run natively", s_info->executable,
				executable.c_str());
		s_active = true;
		if (s_check)
			ps5px_log("native: checking every native run against PCSX2's interpreter ([PS5] NativeCheck): slow, and the interpreter's results are kept");
	}

	void VMDestroyed()
	{
		if (s_module)
			Unload();
	}

	bool Active() { return s_active; }

	bool Has(u32 pc)
	{
		if (!s_active || pc < s_info->text_start || pc >= s_info->text_end)
			return false;
		if (!s_refused.empty() && s_refused.count(pc))
			return false;
		if (!s_hardware_pcs.empty() && s_hardware_pcs.count(pc))
			return false;
		return s_api.has(pc) != 0;
	}

	namespace
	{
		void StateIn(Crux2NativeState& st, u32 pc)
		{
			static_assert(sizeof(cpuRegs.GPR.r) == sizeof(st.gpr));
			std::memcpy(st.gpr, cpuRegs.GPR.r, sizeof(st.gpr));
			st.hi[0] = cpuRegs.HI.UD[0];
			st.hi[1] = cpuRegs.HI.UD[1];
			st.lo[0] = cpuRegs.LO.UD[0];
			st.lo[1] = cpuRegs.LO.UD[1];
			// SA as it is: the module runs PCSX2's own code for every instruction that touches it
			// (ABI 3), so both sides mean the same by it.
			st.sa = cpuRegs.sa;
			st.pc = pc;
			for (int i = 0; i < 32; i++)
				st.fpr[i] = fpuRegs.fpr[i].UL;
			st.fcr31 = fpuRegs.fprc[31];
			st.facc = fpuRegs.ACC.UL;
			st.count = 0;
			st.exit_reason = 0;
			st.traps = 0;
		}

		void StateOut(const Crux2NativeState& st)
		{
			std::memcpy(cpuRegs.GPR.r, st.gpr, sizeof(st.gpr));
			cpuRegs.GPR.r[0].UD[0] = cpuRegs.GPR.r[0].UD[1] = 0;
			cpuRegs.HI.UD[0] = st.hi[0];
			cpuRegs.HI.UD[1] = st.hi[1];
			cpuRegs.LO.UD[0] = st.lo[0];
			cpuRegs.LO.UD[1] = st.lo[1];
			cpuRegs.sa = st.sa;
			for (int i = 0; i < 32; i++)
				fpuRegs.fpr[i].UL = st.fpr[i];
			fpuRegs.fprc[31] = st.fcr31;
			fpuRegs.ACC.UL = st.facc;
			cpuRegs.pc = st.pc;
		}

		void Count(const Crux2NativeState& st)
		{
			s_runs++;
			s_instructions += static_cast<u64>(st.count);
			s_budget_exits += (st.exit_reason == CRUX2_NATIVE_EXIT_BUDGET);
			s_host_exits += (st.exit_reason == CRUX2_NATIVE_EXIT_HOST);
			s_traps += st.traps;
		}

		// Until PCSX2's next event: as many instructions as fit in the cycles left.
		int64_t Budget()
		{
			const s64 left = static_cast<s64>(cpuRegs.nextEventCycle - cpuRegs.cycle);
			const u64 per_1024 = Cycles(1024);
			return (left > 0) ? std::max<s64>(1, static_cast<s64>((static_cast<u64>(left) * 1024) / per_1024)) : 1;
		}

		bool RunNative(u32 pc)
		{
			Crux2NativeState& st = s_state;
			StateIn(st, pc);
			st.budget = Budget();
			s_touched_hardware = false;
			s_api.run(&st);
			if (st.count == 0 && st.pc == pc)
				return false;
			if (s_touched_hardware && !s_native_hardware && s_hardware_pcs.insert(pc).second)
				s_hardware_refused++;
			StateOut(st);
			cpuRegs.cycle += Cycles(static_cast<u64>(st.count));
			Count(st);
			return true;
		}

		std::string Hex128(const u64* v) { return fmt::format("{:016x}{:016x}", v[1], v[0]); }

		// The native result against the interpreter's (now in cpuRegs and memory): what differs.
		std::string Differences(const Crux2NativeState& native, bool* integer)
		{
			std::string out;
			int shown = 0;
			auto note = [&](const std::string& what, bool is_integer) {
				*integer |= is_integer;
				if (shown++ < 4)
					out += (out.empty() ? "" : "; ") + what;
			};
			for (int i = 1; i < 32; i++)
			{
				if (std::memcmp(native.gpr[i], cpuRegs.GPR.r[i].UD, 16) != 0)
					note(fmt::format("r{} native {} interpreter {}", i, Hex128(native.gpr[i]), Hex128(cpuRegs.GPR.r[i].UD)), true);
			}
			if (native.hi[0] != cpuRegs.HI.UD[0] || native.hi[1] != cpuRegs.HI.UD[1])
				note(fmt::format("hi native {} interpreter {}", Hex128(native.hi), Hex128(cpuRegs.HI.UD)), true);
			if (native.lo[0] != cpuRegs.LO.UD[0] || native.lo[1] != cpuRegs.LO.UD[1])
				note(fmt::format("lo native {} interpreter {}", Hex128(native.lo), Hex128(cpuRegs.LO.UD)), true);
			if (native.sa != cpuRegs.sa)
				note(fmt::format("sa native {} interpreter {}", native.sa, cpuRegs.sa), true);
			for (int i = 0; i < 32; i++)
			{
				if (native.fpr[i] != fpuRegs.fpr[i].UL)
					note(fmt::format("f{} native {:08x} interpreter {:08x}", i, native.fpr[i], fpuRegs.fpr[i].UL), false);
			}
			if (native.facc != fpuRegs.ACC.UL)
				note(fmt::format("acc native {:08x} interpreter {:08x}", native.facc, fpuRegs.ACC.UL), false);
			if ((native.fcr31 ^ fpuRegs.fprc[31]) & 0x00800000u)
				note(fmt::format("fpu condition native {} interpreter {}", (native.fcr31 >> 23) & 1, (fpuRegs.fprc[31] >> 23) & 1), true);
			size_t bytes = 0;
			u32 first = 0;
			for (const auto& [host, b] : s_shadow)
			{
				if (*reinterpret_cast<const u8*>(host) != b.value)
				{
					if (!bytes || b.address < first)
						first = b.address;
					bytes++;
				}
			}
			if (bytes)
				note(fmt::format("memory: {} byte(s) written differently, the lowest at 0x{:08x}", bytes, first), true);
			if (shown > 4)
				out += fmt::format("; and {} more", shown - 4);
			return out;
		}

		bool RunChecked(u32 pc)
		{
			Crux2NativeState& st = s_state;
			StateIn(st, pc);
			st.budget = kCheckBudget;
			s_shadow.clear();
			UseShadow(true);
			const bool touched_hardware = fastjmp_set(&s_check_jump) != 0;
			if (!touched_hardware)
				s_api.run(&st);
			UseShadow(false);

			if (touched_hardware)
			{
				// (Nothing was written: the module stopped at the access, before it.) PCSX2 runs
				// this address from now on, so that what the check cannot see is not native.
				s_unchecked_io++;
				if (s_hardware_pcs.insert(pc).second)
					s_hardware_refused++;
				s_shadow.clear();
				return false;
			}
			if (st.count == 0 && st.pc == pc)
				return false;

			if (st.exit_reason == CRUX2_NATIVE_EXIT_BUDGET)
			{
				// Too long to replay: the native result is kept, its writes made now.
				for (const auto& [host, b] : s_shadow)
					*reinterpret_cast<u8*>(host) = b.value;
				s_shadow.clear();
				StateOut(st);
				cpuRegs.cycle += Cycles(static_cast<u64>(st.count));
				Count(st);
				s_unchecked_long++;
				return true;
			}

			// The same from the same registers and memory, by the interpreter.
			const Crux2NativeState native = st;
			const u32 stop = native.pc;
			const u64 n = static_cast<u64>(native.count);
			const u32 lo = std::min(s_info->text_start, stop), hi = std::max(s_info->text_end, stop + 4);
			cpuRegs.pc = pc;
			u64 steps = 0;
			const bool reached = intCrux2Replay(stop, n, std::max<u64>(8, n / 8), n * 2 + 64, lo, hi, &steps);
			cpuRegs.cycle += Cycles(steps ? steps : n);
			Count(native);

			if (!reached)
			{
				s_lost++;
				if (s_reported.insert(pc).second)
					ps5px_log("native check: function entered at 0x%08x: the native code ran %llu instructions to 0x%08x; the interpreter "
							  "did not get there (%llu instructions, now at 0x%08x)",
						pc, static_cast<unsigned long long>(n), stop, static_cast<unsigned long long>(steps), cpuRegs.pc);
				s_shadow.clear();
				return true; // (the interpreter's state is a real one: the game goes on from it)
			}
			bool integer = false;
			const std::string diff = Differences(native, &integer);
			s_shadow.clear();
			s_checked++;
			if (!diff.empty())
			{
				s_differed++;
				if (s_reported.insert(pc).second && s_reported.size() <= 400)
					ps5px_log("native check: %s at 0x%08x (%llu instructions to 0x%08x; the interpreter %llu): %s", integer ? "DIFFERS" : "float differs",
						pc, static_cast<unsigned long long>(n), stop, static_cast<unsigned long long>(steps), diff.c_str());
			}
			return true;
		}
	} // namespace

	bool Run(u32 pc)
	{
		// (A block compiled for a module that has gone, or for a function changed since.)
		if (!s_active || (!s_hardware_pcs.empty() && s_hardware_pcs.count(pc)) || !s_api.has(pc))
			return false;
		if (s_check)
		{
			u32& checks = s_check_counts[pc];
			if (checks < kChecksPerEntry)
			{
				checks++;
				return RunChecked(pc);
			}
		}
		return RunNative(pc);
	}

	void Refuse(u32 pc) { s_refused.insert(pc); }

	void Clear(u32 start, u32 words)
	{
		if (s_module)
			s_api.invalidate(start, words * 4);
	}

	void Reset()
	{
		s_refused.clear();
		if (s_module)
			s_api.invalidate(0, 0xFFFFFFFFu);
	}

	std::string Summary()
	{
		if (!s_module)
			return "no module";
		u32 same = 0, different = 0, invalidations = 0;
		s_api.stats(&same, &different, &invalidations);
		return fmt::format("{}: {} runs, {} instructions natively ({} per run), {} gave way to events, {} to the emulator's "
						   "instructions; functions {} the same, {} changed, {} checked again; {} addresses compiled the usual way{}",
			s_module_name, s_runs, s_instructions, s_runs ? s_instructions / s_runs : 0, s_budget_exits, s_host_exits, same, different,
			invalidations, s_refused.size(), s_traps ? fmt::format(", {} trap conditions not raised", s_traps) : std::string()) +
			(s_hardware_refused ? fmt::format("; {} of them left to PCSX2 for touching hardware", s_hardware_refused) : std::string()) +
			(s_check ? fmt::format("; checked against the interpreter: {} runs, {} different ({} functions named), {} not checked "
								   "(hardware {}, too long {}), {} where the interpreter went elsewhere",
						   s_checked, s_differed, s_reported.size(), s_unchecked_io + s_unchecked_long, s_unchecked_io, s_unchecked_long, s_lost)
					 : std::string());
	}
} // namespace PS5Native
