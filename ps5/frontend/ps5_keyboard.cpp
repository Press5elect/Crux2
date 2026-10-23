// Crux2 - a USB keyboard, through the console's keyboard module.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// How a title reaches a USB keyboard on the PS5, as PSWin (and before it prospero-win's runtime,
// native/pw_hid_ps5.c) found it: libSceKeyboard is system module 0x0106. The SDK has a stub
// library for it, but the title does not link against it: a title that imports a function the
// console does not give it does not start at all, and whether a fake-signed title is given this
// module was an open question. So the module is loaded with sceSysmoduleLoadModule(0x0106) and its
// three functions are looked up while the title runs: sceKeyboardInit, sceKeyboardOpen(user, 0,
// 0, param) and sceKeyboardReadState(handle, state) - by name, then by NID (PSWin build 22 needed
// the NIDs), through the handle sceSysmoduleGetModuleHandleInternal gives for the ID, or, when
// that handle is refused (PSWin build 23 saw sceKernelDlsym answer 0x80020003, ESRCH, for it), the
// handle the process's own module list has for the keyboard module (PSWin build 24).
//
// The state read is the 96 bytes ps5_keyboard_logic.h describes. A keyboard plugged in later is
// opened when it comes (the open is tried again every two seconds); when the module cannot be
// used at all, three tries two seconds apart are made and then one line in the boot log says why.
//
// Imports this adds (all in the SDK's stubs): libSceSysmodule's sceSysmoduleLoadModule and
// sceSysmoduleGetModuleHandleInternal, libkernel's sceKernelDlsym, sceKernelGetModuleList and
// sceKernelGetModuleInfo - the calls PSWin's launcher makes on the console.

#include "ps5_keyboard.h"

#include "Input/InputManager.h"

#include <algorithm>
#include <array>
#include <climits>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
int sceSysmoduleLoadModule(uint16_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t* handle);
int sceKernelDlsym(int32_t handle, const char* symbol, void** address);
int sceKernelGetModuleList(int32_t* handles, size_t capacity, size_t* count);
int sceKernelGetModuleInfo(int32_t handle, void* info);
int sceUserServiceGetInitialUser(int32_t* user_id);
void ps5px_log(const char* format, ...);
}

// ------------------------------------------------------------------------------ system modules

namespace PS5SystemModules
{
	namespace
	{
		// The module list's entry: the size of the structure first, the module's name at 8.
		constexpr size_t MODULE_INFO_SIZE = 0x160;
		constexpr size_t MODULE_NAME_OFFSET = 8;
		constexpr size_t MODULE_NAME_MAX = 256;

		bool NameMatches(const char* name, const char* wanted)
		{
			const size_t n = std::strlen(wanted);
			if (std::strncmp(name, wanted, n) != 0)
				return false;
			return name[n] == '\0' || name[n] == '.';
		}

		/// The process's modules (handles), as the console lists them.
		std::vector<int32_t> ModuleList()
		{
			std::array<int32_t, 256> handles;
			size_t count = 0;
			if (sceKernelGetModuleList(handles.data(), handles.size(), &count) != 0)
				return {};
			return std::vector<int32_t>(handles.begin(), handles.begin() + static_cast<std::ptrdiff_t>(std::min(count, handles.size())));
		}

		/// A module's name ("libkernel.sprx"), or empty.
		std::string ModuleName(int32_t handle)
		{
			alignas(8) uint8_t info[MODULE_INFO_SIZE] = {};
			const uint64_t size = sizeof(info);
			std::memcpy(info, &size, sizeof(size));
			if (sceKernelGetModuleInfo(handle, info) != 0)
				return {};
			char name[MODULE_NAME_MAX + 1] = {};
			std::memcpy(name, info + MODULE_NAME_OFFSET, MODULE_NAME_MAX);
			return name;
		}

		/// The names a module goes by in a process: the kernel's library is libkernel in one
		/// process, libkernel_sys or libkernel_web in another.
		std::vector<const char*> NamesOf(const char* module_name)
		{
			if (std::strcmp(module_name, "libkernel") == 0)
				return {"libkernel", "libkernel_sys", "libkernel_web"};
			return {module_name};
		}

		int32_t FindModule(const char* module_name)
		{
			const std::vector<int32_t> modules = ModuleList();
			for (const char* wanted : NamesOf(module_name))
			{
				for (const int32_t handle : modules)
				{
					if (NameMatches(ModuleName(handle).c_str(), wanted))
						return handle;
				}
			}
			return -1;
		}

		void* FindIn(int32_t module, const char* function, const char* nid)
		{
			if (module < 0)
				return nullptr;
			void* address = nullptr;
			if (sceKernelDlsym(module, function, &address) == 0 && address)
				return address;
			address = nullptr;
			if (nid && sceKernelDlsym(module, nid, &address) == 0 && address)
				return address;
			return dlsym(reinterpret_cast<void*>(static_cast<intptr_t>(module)), function);
		}

		std::mutex s_said_mutex;
		bool s_said_modules = false;
	} // namespace

	void* FindFunction(const char* module_name, const char* function, const char* nid)
	{
		// The module by its name(s) first; then every module of the process, since a function can be
		// given by another module than the one its documentation names.
		const int32_t named = FindModule(module_name);
		if (void* address = FindIn(named, function, nid))
			return address;
		// The kernel's library is not in the list a title sees: its fixed handles.
		if (std::strcmp(module_name, "libkernel") == 0)
		{
			for (const int32_t handle : {int32_t{1}, int32_t{0x2001}})
			{
				if (void* address = FindIn(handle, function, nid))
					return address;
			}
		}
		const std::vector<int32_t> modules = ModuleList();
		for (const int32_t handle : modules)
		{
			if (handle == named)
				continue;
			if (void* address = FindIn(handle, function, nid))
			{
				ps5px_log("system modules: %s found in %s (handle %d), not in %s", function, ModuleName(handle).c_str(), handle, module_name);
				return address;
			}
		}
		// The system's modules are not in the list a title sees, but they have handles: the small
		// numbers the loader gives in load order, and the 0x2000 range. Each is asked in turn (an
		// unused handle answers at once).
		for (int32_t handle = 0; handle < 0x2040; handle = (handle == 511) ? 0x2000 : handle + 1)
		{
			if (handle == named || handle == 1 || handle == 0x2001 ||
				std::find(modules.begin(), modules.end(), handle) != modules.end())
				continue;
			if (void* address = FindIn(handle, function, nid))
			{
				ps5px_log("system modules: %s found in the module of handle %#x", function, static_cast<unsigned>(handle));
				return address;
			}
		}
		// Not found anywhere: once, what the process has, so the boot log says where to look.
		const std::lock_guard lock(s_said_mutex);
		if (!s_said_modules)
		{
			s_said_modules = true;
			std::string names;
			for (const int32_t handle : modules)
			{
				names += names.empty() ? "" : ", ";
				names += ModuleName(handle);
			}
			ps5px_log("system modules: %s not found (%s was %s); the process has %zu module(s): %s", function, module_name,
				named >= 0 ? "found" : "not found", modules.size(), names.c_str());
		}
		else
		{
			ps5px_log("system modules: %s not found", function);
		}
		return nullptr;
	}

	int LoadInternal(uint32_t internal_id)
	{
		using LoadFn = int (*)(uint32_t);
		const auto load = reinterpret_cast<LoadFn>(FindFunction("libSceSysmodule", "sceSysmoduleLoadModuleInternal", "39iV5E1HoCk"));
		if (!load)
			return INT32_MIN;
		return load(internal_id);
	}

	void* FindInHandle(int32_t handle, const char* function, const char* nid)
	{
		return FindIn(handle, function, nid);
	}

	void* FindSystemFunction(uint16_t sysmodule_id, const char* module_name, const char* function, const char* nid,
		int* load_result)
	{
		const int loaded = sceSysmoduleLoadModule(sysmodule_id);
		if (load_result)
			*load_result = loaded;

		int32_t handle = -1;
		if (sceSysmoduleGetModuleHandleInternal(sysmodule_id, &handle) != 0 || handle < 0)
		{
			handle = -1;
			if (sceSysmoduleGetModuleHandleInternal(0x80000000u | sysmodule_id, &handle) != 0)
				handle = -1;
		}
		if (void* address = FindIn(handle, function, nid))
			return address;
		const int32_t listed = FindModule(module_name);
		return (listed >= 0 && listed != handle) ? FindIn(listed, function, nid) : nullptr;
	}
} // namespace PS5SystemModules

// ------------------------------------------------------------------------------ the keyboard

namespace PS5Keyboard
{
	namespace
	{
		constexpr uint16_t KEYBOARD_MODULE = 0x0106;
		constexpr int RESOLVE_TRIES = 3;
		constexpr double RETRY_SECONDS = 2.0;

		using InitFn = int (*)();
		using OpenFn = int (*)(int32_t user, int32_t type, int32_t index, void* param);
		using ReadStateFn = int (*)(int32_t handle, void* state);

		struct State
		{
			std::atomic<bool> enabled{true}; // set from the settings, read by the polling thread

			// The polling thread's.
			bool reading = false; // keys may be held: a switch-off lets go of them first
			bool resolved = false;
			bool given_up = false;
			int resolve_tries = 0;
			bool open_logged = false;
			bool read_logged = false;
			InitFn init = nullptr;
			OpenFn open = nullptr;
			ReadStateFn read_state = nullptr;
			int32_t handle = -1;
			double next_try = 0.0;
			PS5KeyboardLogic::Tracker tracker;
			std::vector<PS5KeyboardLogic::KeyEvent> events;

			// Shared with the interface.
			std::mutex mutex;
			PS5KeyboardLogic::Typed typed;
			bool capture = false;
			bool connected = false;
		};

		State& S()
		{
			static State s;
			return s;
		}

		double Now()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		bool Resolve(State& s)
		{
			int loaded = 0;
			s.init = reinterpret_cast<InitFn>(
				PS5SystemModules::FindSystemFunction(KEYBOARD_MODULE, "libSceKeyboard", "sceKeyboardInit", "wadT3QBCGY0", &loaded));
			s.open = reinterpret_cast<OpenFn>(
				PS5SystemModules::FindSystemFunction(KEYBOARD_MODULE, "libSceKeyboard", "sceKeyboardOpen", "HJ+KnEHcaxI"));
			s.read_state = reinterpret_cast<ReadStateFn>(
				PS5SystemModules::FindSystemFunction(KEYBOARD_MODULE, "libSceKeyboard", "sceKeyboardReadState", "6HpE68bzX6M"));
			if (s.init && s.open && s.read_state)
			{
				const int init = s.init();
				ps5px_log("keyboard: module 0x%04x ready (load 0x%x), init 0x%x", KEYBOARD_MODULE, static_cast<unsigned>(loaded),
					static_cast<unsigned>(init));
				return true;
			}
			if (++s.resolve_tries >= RESOLVE_TRIES)
			{
				s.given_up = true;
				ps5px_log("keyboard: the console's keyboard module cannot be used (load 0x%x; init %s, open %s, read %s): no USB keyboard",
					static_cast<unsigned>(loaded), s.init ? "found" : "missing", s.open ? "found" : "missing",
					s.read_state ? "found" : "missing");
			}
			return false;
		}

		void TryOpen(State& s)
		{
			int32_t user = -1;
			if (sceUserServiceGetInitialUser(&user) < 0 || user < 0)
				return;
			uint8_t param[8] = {};
			const int handle = s.open(user, 0, 0, param);
			if (handle >= 0)
			{
				s.handle = handle;
				ps5px_log("keyboard: opened for user 0x%x, handle %d", static_cast<unsigned>(user), handle);
			}
			else if (!s.open_logged)
			{
				s.open_logged = true;
				ps5px_log("keyboard: none to open yet (0x%x); tried again every %.0f s", static_cast<unsigned>(handle), RETRY_SECONDS);
			}
		}

		// Passes the key events on, and the text to the queue; a disconnected report lets go.
		void Deliver(State& s, const PS5KeyboardLogic::Report& report)
		{
			PS5KeyboardLogic::Typed typed;
			s.events.clear();
			s.tracker.Take(report, Now(), &s.events, &typed);

			bool capture;
			{
				std::lock_guard lock(s.mutex);
				capture = s.capture;
				s.connected = report.connected;
				if (typed.Any())
					s.typed.Append(typed);
			}
			for (const PS5KeyboardLogic::KeyEvent& e : s.events)
			{
				// A key let go is always passed on, so nothing stays held when capture begins.
				if (capture && e.down)
					continue;
				InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(e.code), e.down ? 1.0f : 0.0f);
			}
		}
	} // namespace

	void SetEnabled(bool enabled)
	{
		// Only noted here (the settings' thread, maybe under their lock): the polling thread lets
		// go of the keys held, as an event of its own.
		S().enabled.store(enabled, std::memory_order_release);
	}

	void Update()
	{
		State& s = S();
		if (!s.enabled.load(std::memory_order_acquire))
		{
			if (s.reading)
			{
				s.reading = false;
				Deliver(s, PS5KeyboardLogic::Report());
			}
			return;
		}
		if (s.given_up)
			return;
		s.reading = true;

		if (!s.resolved || s.handle < 0)
		{
			const double now = Now();
			if (now < s.next_try)
				return;
			s.next_try = now + RETRY_SECONDS;
			if (!s.resolved)
				s.resolved = Resolve(s);
			if (s.resolved && s.handle < 0)
				TryOpen(s);
			if (s.handle < 0)
				return;
		}

		std::array<uint8_t, PS5KeyboardLogic::STATE_SIZE> state = {};
		const int rc = s.read_state(s.handle, state.data());
		if (rc != 0)
		{
			if (!s.read_logged)
			{
				s.read_logged = true;
				ps5px_log("keyboard: reading failed (0x%x)", static_cast<unsigned>(rc));
			}
			Deliver(s, PS5KeyboardLogic::Report());
			return;
		}
		Deliver(s, PS5KeyboardLogic::ParseState(state.data(), state.size()));
	}

	PS5KeyboardLogic::Typed Poll()
	{
		State& s = S();
		std::lock_guard lock(s.mutex);
		PS5KeyboardLogic::Typed typed = std::move(s.typed);
		s.typed = {};
		return typed;
	}

	void SetTextCapture(bool capture)
	{
		State& s = S();
		std::lock_guard lock(s.mutex);
		s.capture = capture;
	}

	bool IsConnected()
	{
		State& s = S();
		std::lock_guard lock(s.mutex);
		return s.connected;
	}

	std::optional<uint32_t> NameToCode(std::string_view name)
	{
		return PS5KeyboardLogic::CodeOf(name);
	}

	std::optional<std::string> CodeToName(uint32_t code)
	{
		if (code < 4 || code > 0xe7)
			return std::nullopt;
		return PS5KeyboardLogic::NameOrUsage(code);
	}
} // namespace PS5Keyboard
