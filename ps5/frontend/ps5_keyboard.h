// Crux2 - a USB keyboard, through the console's keyboard module (ps5_keyboard.cpp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Two uses. The interface's text fields take what is typed (Poll). And the keys reach PCSX2's
// InputManager as host keyboard keys, which is what its USB keyboard device ("HID Keyboard" in a
// USB port) and any "Keyboard/..." binding listen to.
//
// The module is optional: it is looked up while the title runs (it cannot be linked, see the
// .cpp), and when it is not there, or a function of it is not, one line goes to the boot log and
// everything here quietly does nothing.

#pragma once

#include "ps5_keyboard_logic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace PS5Keyboard
{
	/// [PS5] USBKeyboard (on by default). Off lets go of any key held and stops reading.
	void SetEnabled(bool enabled);

	/// Reads the keyboard and passes its key events on. Called by the input source each time it
	/// polls (ps5_input_source.cpp), on the thread that polls input; cheap when there is none.
	void Update();

	/// What was typed since the last call: text, Enter, Backspace, Escape, the arrows and the
	/// rest of PS5KeyboardLogic::Typed. From any thread (the interface's).
	PS5KeyboardLogic::Typed Poll();

	/// While a text field has the keyboard, keys go to it alone and not to InputManager (a game's
	/// keyboard and the hotkeys do not see the typing). The interface sets it while a field is
	/// being edited and clears it after.
	void SetTextCapture(bool capture);

	/// Whether a keyboard is there now (for a hint on the screen).
	bool IsConnected();

	/// The host key codes (a key's HID usage) and their names, for InputManager's
	/// ConvertHostKeyboardStringToCode / ConvertHostKeyboardCodeToString (ps5_host.cpp forwards
	/// to these).
	std::optional<uint32_t> NameToCode(std::string_view name);
	std::optional<std::string> CodeToName(uint32_t code);
} // namespace PS5Keyboard

namespace PS5SystemModules
{
	/// A function of a module this process holds, found while the title runs: module_name is the
	/// start of the module's file name ("libScePad" matches "libScePad.sprx", not
	/// "libScePadTracker"); the function is looked up by its name, then by its NID (the console's
	/// loader names exports by NID; ps5/tools/check-imports.py derives them), then with dlsym.
	/// The process's module list shows a title only its own modules (eboot.bin, libc.prx), not
	/// the system's; the kernel's library ("libkernel") is reached by the handles the console
	/// gives it in every process (1, or 0x2001 in another kind of process). A function found in
	/// no module is said once in the boot log, with the modules the list has.
	void* FindFunction(const char* module_name, const char* function, const char* nid);

	/// Loads a system module by its internal ID (sceSysmoduleLoadModuleInternal, found while the
	/// title runs; libSceAudioIn is 0x80000002). Its answer, or INT32_MIN when the loader was not found.
	int LoadInternal(uint32_t internal_id);

	/// A function of the module of this handle (sceKernelLoadStartModule's answer, say).
	void* FindInHandle(int32_t handle, const char* function, const char* nid);

	/// The same in a system module given by its ID, which is loaded first
	/// (sceSysmoduleLoadModule) and then found by that ID, or else by module_name among the
	/// process's modules. *load_result (when given) gets what loading said.
	void* FindSystemFunction(uint16_t sysmodule_id, const char* module_name, const char* function, const char* nid,
		int* load_result = nullptr);
} // namespace PS5SystemModules
