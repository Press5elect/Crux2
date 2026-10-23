// Crux2 - the web panel: the title from a phone or a computer on the same network.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Adapted from PSSwanStation's web panel (ps5/src/web.h and web.cpp), by the same author.
//
// A small HTTP server on port 3311, on one thread of its own, serving a single page (inline CSS
// and JavaScript, nothing fetched from elsewhere) and a few JSON answers under /api/. From it a
// phone can start a game from the library, pause, save and load states, turn
// fast-forward on and off, open the quick menu, leave the game, type text into whatever text
// field the title has open, and download the two logs.
//
// Every request must carry the key: 128 random bits as 32 hexadecimal digits, in the address
// (?key=...) the QR code on the TV carries, or in the cookie the server sets after the first
// request that had it. Without it the answer is 403 and nothing is done. A new key (newKey())
// makes every phone that had the old one scan the code again.
//
// The title is reached only through the Hooks given to init(), and the hooks are only ever
// called from tick(), on the title's main thread: the server thread puts what a request needs
// into a queue and waits (about two seconds at most) for the next tick() to do it. The status
// shown on the page is taken by tick() four times a second and kept for the server thread.
//
// The console's address is never written to the log; status() and url() carry it for the screen.
//
// No PCSX2 in this file or in ps5_web.cpp: ps5/tests/web builds both for the PC.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PS5Web
{
	constexpr int kPort = 3311;

	/// A game of the library, as the page lists it. `path` is what startGame() gets back.
	struct Game
	{
		std::string title;
		std::string serial;
		std::string region;
		std::string path;
		bool has_cover = false; ///< cover(serial, ...) has a picture for it
	};

	/// What the "Now playing" part of the page shows.
	struct Status
	{
		bool running = false; ///< a game is running (paused or not)
		bool paused = false;
		bool fast_forward = false;
		bool text_field_open = false; ///< an on-screen text field would take typed text now
		std::string title;
		std::string serial;
		float speed_percent = 0.0f; ///< emulation speed, 100 = full speed
		float fps = 0.0f;
	};

	enum class Key
	{
		Enter,
		Backspace,
	};

	enum class Log
	{
		Boot, ///< crux2-boot.log
		Emulator, ///< logs/emulog.txt
	};

	/// What the title does for the panel. Every one is called on the main thread, from tick().
	/// Any may be left empty: the page then hears that it is not available (501).
	struct Hooks
	{
		std::function<std::vector<Game>()> listGames;
		/// Starts the game at `path` (one listGames() gave), leaving the running one if there is one.
		std::function<bool(const std::string& path)> startGame;
		std::function<Status()> status;
		/// One of the names below; false when it could not be done now.
		///   "pause", "resume", "quick_menu", "leave"   (arg 0)
		///   "save_state", "load_state"                              (arg: the slot, 1 to 10)
		///   "fast_forward"                                          (arg: 1 on, 0 off)
		std::function<bool(const std::string& name, int arg)> action;
		/// Text typed on the phone (valid UTF-8, no control characters), for whatever text field
		/// the title has open.
		std::function<bool(const std::string& utf8)> typeText;
		std::function<bool(Key key)> pressKey;
		/// The whole text of a log; false when there is none.
		std::function<bool(Log which, std::string* text)> readLog;
		/// A game's cover picture and its type ("image/png", "image/jpeg"); false when it has none.
		std::function<bool(const std::string& serial, std::vector<std::uint8_t>* bytes, std::string* mime)> cover;

		/// The console's own IPv4 address on the network ("" when not known), for url() and
		/// status(). Optional: when empty the module asks the system itself (see ps5_web.cpp).
		/// PS5Net::CurrentStatus().console_ip is the natural thing to give.
		std::function<std::string()> localAddress;
		/// A key was made (at init when none was given, or by newKey()), so the title may keep it
		/// for the next start (a settings file). Optional. Never write it to a log.
		std::function<void(const std::string& key)> keyChanged;
	};

	/// Where the server listens. The defaults are the console's; the PC tests bind 127.0.0.1 on a
	/// port the system picks (port 0).
	struct Options
	{
		std::string bind_address = "0.0.0.0";
		int port = kPort;
		/// A key kept from an earlier run (32 hexadecimal digits); anything else and a new one is made.
		std::string key;
		/// How long a request waits for tick() before it is answered 503.
		int main_thread_timeout_ms = 2000;
	};

	/// One line for the title's boot log (no newline). Never given addresses or the key.
	using LogFunction = std::function<void(const char* line)>;

	/// Once, before anything else. Does not start the server.
	void init(const Hooks& hooks, LogFunction log, const Options& options = Options());
	/// Starts (true) or stops (false) the server, as the setting says. Cheap when nothing changes.
	/// Returns whether it is listening afterwards.
	bool apply(bool enabled);
	/// Once a frame on the main thread: what requests asked for is done here.
	void tick();
	/// The title closes: stops the server, answers nothing more. init() may be called again after.
	void shutdown();

	bool listening();
	/// The port listened on (the one the system picked when Options::port was 0); 0 when off.
	int port();
	/// For the screen: "Listening on 192.0.2.10:3311", or "Off", or why it could not start.
	std::string status();
	/// http://<address>:<port>/?key=<key>, or "" while off or while the console's address is not known.
	/// For the screen (and its QR code) only: never write it to a log.
	std::string url();
	std::string key();
	/// A new key: whatever had the old one (an address, a cookie) is refused from now on.
	void newKey();

	/// A QR code of `text` (error correction M or better), for the screen: size x size modules,
	/// row by row, 1 dark and 0 light, without the quiet zone (four light modules around it,
	/// which the drawing must add). size is 0 when the text does not fit.
	struct QrCode
	{
		int size = 0;
		std::vector<std::uint8_t> modules;
		bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<size_t>(y) * size + x] != 0; }
	};
	QrCode qr(const std::string& text);
} // namespace PS5Web
