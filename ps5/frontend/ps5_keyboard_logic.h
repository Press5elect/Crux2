// Crux2 - a USB keyboard's reports turned into key events and typed text, without the console.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The console's keyboard module reports what a USB boot keyboard reports: the modifier keys as
// bits and the other keys held as usages of the HID keyboard page (0x07). Crux2 uses those usages
// as its host key codes, so a key's code is its usage; the modifiers are the usages 0xE0 to 0xE7
// that the keyboard page gives them. Each code has a name, which is what a binding string says
// ("Keyboard/Return") and, more to the point, what PCSX2's USB keyboard device (USB/usb-hid,
// "HID Keyboard") asks InputManager::ConvertHostKeyboardStringToCode for: the names are the ones
// of its table (QEMU's key names), so every key it knows finds its code here.
//
// The tracker follows reports over time: which keys went down and up (for InputManager), and
// what was typed (for the interface's text fields), with Backspace, Delete and the arrows
// repeating while held as on a PC. US layout; Shift and Caps Lock apply.
//
// The 96-byte state the console writes (the layout PSWin and prospero-win read it with): u64
// timestamp, 8 bytes, u8 connected (+3), s32 number of keys, u32 locks, u32 modifiers,
// u16 usages[16], 32 reserved.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PS5KeyboardLogic
{
	struct Report
	{
		bool connected = false;
		uint8_t modifiers = 0; // bit 0 left Ctrl, 1 left Shift, 2 left Alt, 3 left GUI, 4 to 7 the right ones
		std::array<uint8_t, 16> keys = {}; // usages held, 0 for none
	};

	constexpr size_t STATE_SIZE = 96;

	inline uint32_t ReadLE32(const uint8_t* p)
	{
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
			   (static_cast<uint32_t>(p[3]) << 24);
	}

	/// The console's keyboard state as a report. Usages above 0xFF (none on a keyboard page) and
	/// the modifier usages, should the console list them too, are left out of the keys: the
	/// modifiers come from their bits.
	inline Report ParseState(const uint8_t* state, size_t size)
	{
		Report report;
		if (size < STATE_SIZE)
			return report;
		report.connected = state[16] != 0;
		if (!report.connected)
			return report;
		int32_t count = static_cast<int32_t>(ReadLE32(state + 20));
		count = std::clamp<int32_t>(count, 0, 16);
		report.modifiers = static_cast<uint8_t>(ReadLE32(state + 28));
		size_t n = 0;
		for (int32_t i = 0; i < count; i++)
		{
			const unsigned usage = state[32 + 2 * i] | (static_cast<unsigned>(state[33 + 2 * i]) << 8);
			if (usage == 0 || usage > 0xff)
				continue;
			if (usage >= 0xe0 && usage <= 0xe7)
			{
				report.modifiers |= static_cast<uint8_t>(1u << (usage - 0xe0));
				continue;
			}
			report.keys[n++] = static_cast<uint8_t>(usage);
		}
		return report;
	}

	// ---------------------------------------------------------------- names

	struct KeyName
	{
		uint8_t usage;
		const char* name;
	};

	// The names of PCSX2's USB keyboard device (usb-hid.cpp, s_qkeycode_names) for the usages a
	// keyboard sends. Keys it does not know keep a name of their own, so they can still be bound.
	// Five keys are deliberately not given the device's name for them, because QEMU's tables
	// behind the device would send the game another key (its keypad "=" arrives as F13, its keypad
	// "," as the right GUI key, Yen as the left GUI key, Open as F24, SysRq as Print Screen):
	// unknown to the device, they are simply not sent (tests/input/keymap_check.py follows every
	// name through those tables). The device's Help to Find and the Japanese conversion keys are
	// named as it names them, but QEMU's tables send nothing for them.
	// clang-format off
	inline constexpr KeyName KEY_NAMES[] = {
		{0x04, "A"}, {0x05, "B"}, {0x06, "C"}, {0x07, "D"}, {0x08, "E"}, {0x09, "F"}, {0x0a, "G"}, {0x0b, "H"},
		{0x0c, "I"}, {0x0d, "J"}, {0x0e, "K"}, {0x0f, "L"}, {0x10, "M"}, {0x11, "N"}, {0x12, "O"}, {0x13, "P"},
		{0x14, "Q"}, {0x15, "R"}, {0x16, "S"}, {0x17, "T"}, {0x18, "U"}, {0x19, "V"}, {0x1a, "W"}, {0x1b, "X"},
		{0x1c, "Y"}, {0x1d, "Z"},
		{0x1e, "1"}, {0x1f, "2"}, {0x20, "3"}, {0x21, "4"}, {0x22, "5"}, {0x23, "6"}, {0x24, "7"}, {0x25, "8"},
		{0x26, "9"}, {0x27, "0"},
		{0x28, "Return"}, {0x29, "Escape"}, {0x2a, "Backspace"}, {0x2b, "Tab"}, {0x2c, "Space"},
		{0x2d, "Minus"}, {0x2e, "Equal"}, {0x2f, "BracketLeft"}, {0x30, "BracketRight"}, {0x31, "Backslash"},
		{0x32, "NonUSHash"}, {0x33, "Semicolon"}, {0x34, "Apostrophe"}, {0x35, "Agrave"}, {0x36, "Comma"},
		{0x37, "Period"}, {0x38, "Slash"}, {0x39, "Caps_lock"},
		{0x3a, "F1"}, {0x3b, "F2"}, {0x3c, "F3"}, {0x3d, "F4"}, {0x3e, "F5"}, {0x3f, "F6"}, {0x40, "F7"},
		{0x41, "F8"}, {0x42, "F9"}, {0x43, "F10"}, {0x44, "F11"}, {0x45, "F12"},
		{0x46, "Print"}, {0x47, "Scroll_lock"}, {0x48, "Pause"}, {0x49, "Insert"}, {0x4a, "Home"},
		{0x4b, "PageUp"}, {0x4c, "Delete"}, {0x4d, "End"}, {0x4e, "PageDown"},
		{0x4f, "Right"}, {0x50, "Left"}, {0x51, "Down"}, {0x52, "Up"},
		{0x53, "Num_lock"}, {0x54, "NumpadSlash"}, {0x55, "NumpadAsterisk"}, {0x56, "NumpadMinus"},
		{0x57, "NumpadPlus"}, {0x58, "NumpadReturn"},
		{0x59, "Numpad1"}, {0x5a, "Numpad2"}, {0x5b, "Numpad3"}, {0x5c, "Numpad4"}, {0x5d, "Numpad5"},
		{0x5e, "Numpad6"}, {0x5f, "Numpad7"}, {0x60, "Numpad8"}, {0x61, "Numpad9"}, {0x62, "Numpad0"},
		{0x63, "NumpadPeriod"}, {0x64, "Less"}, {0x65, "Compose"}, {0x66, "Power"}, {0x67, "KeypadEqual"},
		{0x68, "F13"}, {0x69, "F14"}, {0x6a, "F15"}, {0x6b, "F16"}, {0x6c, "F17"}, {0x6d, "F18"},
		{0x6e, "F19"}, {0x6f, "F20"}, {0x70, "F21"}, {0x71, "F22"}, {0x72, "F23"}, {0x73, "F24"},
		{0x74, "Execute"}, {0x75, "Help"}, {0x76, "Menu"}, {0x77, "Front"}, {0x78, "Stop"}, {0x79, "Again"},
		{0x7a, "Undo"}, {0x7b, "Cut"}, {0x7c, "Copy"}, {0x7d, "Paste"}, {0x7e, "Find"},
		{0x7f, "AudioMute"}, {0x80, "VolumeUp"}, {0x81, "VolumeDown"},
		{0x85, "KeypadComma"}, {0x87, "Ro"}, {0x88, "KatakanaHiragana"}, {0x89, "International3"}, {0x8a, "Henkan"},
		{0x8b, "Muhenkan"}, {0x9a, "SysReq"},
		{0xe0, "Control"}, {0xe1, "Shift"}, {0xe2, "Alt"}, {0xe3, "Meta"},
		{0xe4, "Control_r"}, {0xe5, "Shift_r"}, {0xe6, "Alt_r"}, {0xe7, "Meta_r"},
	};
	// clang-format on

	inline const char* NameOf(uint32_t code)
	{
		for (const KeyName& k : KEY_NAMES)
		{
			if (k.usage == code)
				return k.name;
		}
		return nullptr;
	}

	/// A key's code from its name; also "Usage<n>" (decimal) for a key with no name.
	inline std::optional<uint32_t> CodeOf(std::string_view name)
	{
		for (const KeyName& k : KEY_NAMES)
		{
			if (name == k.name)
				return k.usage;
		}
		// The name PCSX2's own table gives Enter elsewhere, and the key a Shift+Tab binding names.
		if (name == "Enter")
			return 0x28;
		if (name == "Backtab")
			return 0x2b;
		if (name.starts_with("Usage") && name.size() > 5 && name.size() <= 8)
		{
			uint32_t value = 0;
			for (const char ch : name.substr(5))
			{
				if (ch < '0' || ch > '9')
					return std::nullopt;
				value = value * 10 + static_cast<uint32_t>(ch - '0');
			}
			if (value >= 4 && value <= 0xe7)
				return value;
		}
		return std::nullopt;
	}

	/// The name of a code: its own, or "Usage<n>".
	inline std::string NameOrUsage(uint32_t code)
	{
		if (const char* name = NameOf(code))
			return name;
		return "Usage" + std::to_string(code);
	}

	// ---------------------------------------------------------------- characters

	/// The character a key types on a US keyboard, with or without Shift; 0 for none.
	inline char Character(uint8_t usage, bool shift)
	{
		if (usage >= 0x04 && usage <= 0x1d)
			return static_cast<char>((shift ? 'A' : 'a') + (usage - 0x04));
		if (usage >= 0x1e && usage <= 0x27)
		{
			static constexpr char plain[] = "1234567890", shifted[] = "!@#$%^&*()";
			return (shift ? shifted : plain)[usage - 0x1e];
		}
		if (usage >= 0x2c && usage <= 0x38 && usage != 0x32)
		{
			static constexpr char plain[] = " -=[]\\#;'`,./", shifted[] = " _+{}|~:\"~<>?";
			return (shift ? shifted : plain)[usage - 0x2c];
		}
		switch (usage)
		{
			case 0x54: return '/';
			case 0x55: return '*';
			case 0x56: return '-';
			case 0x57: return '+';
			case 0x62: return '0';
			case 0x63: return '.';
			case 0x64: return shift ? '|' : '\\';
			default: break;
		}
		if (usage >= 0x59 && usage <= 0x61)
			return static_cast<char>('1' + (usage - 0x59));
		return 0;
	}

	// ---------------------------------------------------------------- following the reports

	/// A key going down or up.
	struct KeyEvent
	{
		uint32_t code;
		bool down;
	};

	/// What a text field takes: the text typed (UTF-8; ASCII from a US layout) and the special keys,
	/// each counted (a held Backspace or arrow counts again with each repeat).
	struct Typed
	{
		std::string text;
		int backspace = 0;
		int del = 0;
		int enter = 0;
		int escape = 0;
		int tab = 0;
		int up = 0, down = 0, left = 0, right = 0;
		int home = 0, end = 0;

		bool Any() const
		{
			return !text.empty() || backspace || del || enter || escape || tab || up || down || left || right || home || end;
		}

		/// Adds another's keys after these (the queue the interface drains). Text past max_text
		/// bytes and counts past 64 are dropped: nobody draining it must not grow it without end.
		void Append(const Typed& other, size_t max_text = 256)
		{
			if (text.size() < max_text)
				text.append(other.text, 0, std::min(other.text.size(), max_text - text.size()));
			const auto add = [](int& a, int b) { a = std::min(a + b, 64); };
			add(backspace, other.backspace);
			add(del, other.del);
			add(enter, other.enter);
			add(escape, other.escape);
			add(tab, other.tab);
			add(up, other.up);
			add(down, other.down);
			add(left, other.left);
			add(right, other.right);
			add(home, other.home);
			add(end, other.end);
		}
	};

	class Tracker
	{
	public:
		static constexpr double REPEAT_DELAY = 0.45;
		static constexpr double REPEAT_INTERVAL = 0.06;

		/// One report at a time on a clock that only goes forward (seconds). Appends the keys that
		/// went down or up (modifiers as 0xE0 to 0xE7) to events, and what they typed to typed.
		/// A report of no keyboard lets go of everything.
		void Take(const Report& report, double now, std::vector<KeyEvent>* events, Typed* typed)
		{
			const Report current = report.connected ? report : Report();

			// Up first, so a key's events stay in order when it is let go and pressed again.
			for (uint32_t bit = 0; bit < 8; bit++)
			{
				if ((m_modifiers & (1u << bit)) && !(current.modifiers & (1u << bit)))
					events->push_back({0xe0 + bit, false});
			}
			for (const uint8_t usage : m_held)
			{
				if (IsKey(usage) && !Holds(current.keys, usage))
					events->push_back({usage, false});
			}
			for (uint32_t bit = 0; bit < 8; bit++)
			{
				if (!(m_modifiers & (1u << bit)) && (current.modifiers & (1u << bit)))
					events->push_back({0xe0 + bit, true});
			}
			for (const uint8_t usage : current.keys)
			{
				if (!IsKey(usage) || Holds(m_held, usage))
					continue;
				events->push_back({usage, true});
				Press(usage, current.modifiers, typed);
				if (Repeats(usage))
				{
					m_repeating = usage;
					m_next_repeat = now + REPEAT_DELAY;
				}
			}

			if (m_repeating != 0 && !Holds(current.keys, m_repeating))
				m_repeating = 0;
			if (m_repeating != 0 && now >= m_next_repeat)
			{
				Press(m_repeating, current.modifiers, typed);
				m_next_repeat = std::max(m_next_repeat + REPEAT_INTERVAL, now);
			}

			m_held = current.keys;
			m_modifiers = current.modifiers;
		}

		bool CapsLock() const { return m_caps; }

	private:
		// 1 to 3 are a report's error codes (too many keys at once), not keys.
		static bool IsKey(uint8_t usage) { return usage > 0x03; }

		static bool Holds(const std::array<uint8_t, 16>& keys, uint8_t usage)
		{
			return std::find(keys.begin(), keys.end(), usage) != keys.end();
		}

		static bool Repeats(uint8_t usage)
		{
			return usage == 0x2a || usage == 0x4c || (usage >= 0x4f && usage <= 0x52);
		}

		void Press(uint8_t usage, uint8_t modifiers, Typed* typed)
		{
			switch (usage)
			{
				case 0x28:
				case 0x58: typed->enter++; return;
				case 0x29: typed->escape++; return;
				case 0x2a: typed->backspace++; return;
				case 0x2b: typed->tab++; return;
				case 0x4c: typed->del++; return;
				case 0x4a: typed->home++; return;
				case 0x4d: typed->end++; return;
				case 0x4f: typed->right++; return;
				case 0x50: typed->left++; return;
				case 0x51: typed->down++; return;
				case 0x52: typed->up++; return;
				case 0x39: m_caps = !m_caps; return;
				default: break;
			}
			// Ctrl, Alt and GUI chords are not text.
			constexpr uint8_t CTRL_ALT_GUI = 0x01 | 0x04 | 0x08 | 0x10 | 0x40 | 0x80;
			if (modifiers & CTRL_ALT_GUI)
				return;
			bool shift = (modifiers & (0x02 | 0x20)) != 0;
			if (m_caps && usage >= 0x04 && usage <= 0x1d)
				shift = !shift;
			if (const char ch = Character(usage, shift))
				typed->text.push_back(ch);
		}

		std::array<uint8_t, 16> m_held = {};
		uint8_t m_modifiers = 0;
		bool m_caps = false;
		uint8_t m_repeating = 0;
		double m_next_repeat = 0.0;
	};
} // namespace PS5KeyboardLogic
