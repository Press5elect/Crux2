// Crux2 - the console's microphones for the PlayStation 2's USB microphones (SingStar).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// PCSX2 emulates SingStar's USB microphones (USB/usb-mic) and hears them through cubeb, which the
// console does not have for input. Here each microphone is the console's: the one of a player's
// controller (the DualSense has one) or of the headset plugged into it, read with libSceAudioIn
// (found while the title runs, as the keyboard module is: ps5_keyboard.h) for that player's user.
//
//   "ps5-mic-1" ... "ps5-mic-4"   player 1 to 4's microphone (the order of the controllers)
//
// Settings > Controllers > SingStar sets the USB port up: one microphone (player 1's) or two
// (players 1 and 2: SingStar's two microphones, red and blue). What the console's microphone
// gives (16 kHz for voice, else 48 kHz) is brought to the rate the game asks for. Needs proper
// testing on a console: which type and rate libSceAudioIn takes from a title is tried in turn and
// said in the boot log ("mic:").

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class AudioDevice;

namespace PS5Mic
{
/// The emulated device's source, for a name above; nullptr for any other name.
std::unique_ptr<AudioDevice> Create(int dir, uint32_t channels, const std::string& name, int32_t latency);
/// The names and what to call them, for PCSX2's list of input devices.
std::vector<std::pair<std::string, std::string>> Devices();

/// What the interface's SingStar choice (0 off, 1 one microphone, 2 two) means for the settings of
/// USB port 2: its device type and the two microphones' names.
struct UsbSetup
{
	const char* type;    // "None" or "singstar"
	const char* player1; // "" or "ps5-mic-1"
	const char* player2; // "" or "ps5-mic-2"
};
UsbSetup SetupFor(int choice);

/// The microphone test: the rate it records and plays at.
constexpr unsigned TEST_RATE = 48000;

struct TestResult
{
	bool opened = false;          // the console's microphone opened
	double seconds = 0.0;         // how long it listened
	std::vector<int16_t> samples; // what came, at TEST_RATE, mono
	int peak = 0;                 // the loudest sample (of 32767)
	double rms = 0.0;             // how loud on average
	std::string details;          // the reads, as the boot log's "mic:" line says them
};

/// Listens to player `player`'s microphone (0 = player 1) for `seconds`, as the game's would be
/// heard; written to the boot log ("mic test:"). Blocks: run it on a thread of its own.
TestResult Record(int player, double seconds, const std::atomic_bool* cancel = nullptr);

/// Plays the recording through the console's speakers or headset. Blocks until it is played.
bool PlayBack(const std::vector<int16_t>& samples);
} // namespace PS5Mic
