// Crux2 - the thinking of ps5_extras.cpp, with nothing of the console or the emulator in it.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Three small things the comforts of v0.5.0 decide by, kept apart so that the PC can test them
// (ps5/tests/extras/extras_logic_test.cpp):
//
//   * CardGroups: which games share a memory card when each game has its own. PCSX2's game
//     database names, for some games, the other games whose saves they read (a sequel that
//     imports the first part's save, the discs of one game, the regions' releases of it): games
//     joined that way, in either direction, get one card between them, named by the smallest
//     serial of the group. A game the list does not join to any other has a card of its own.
//
//   * TexturePackCandidates: where a game's texture pack is looked for on a drive or a share.
//
//   * PlayClock: how long a game was played since its state was last saved by itself. Counts only
//     the time the game ran: frames that came more than a second apart (a pause, a menu, a state
//     being loaded) add nothing.

#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace PS5Extras
{

/// "slus-20062" as "SLUS-20062"; and empty when it is nothing a file can be named by (a serial is
/// letters, digits, '-', '.' and '_', and at most 24 of them).
inline std::string CardKey(std::string_view serial)
{
	std::string out;
	if (serial.empty() || serial.size() > 24)
		return out;
	out.reserve(serial.size());
	for (const char c : serial)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		if (!(std::isalnum(u) || c == '-' || c == '.' || c == '_'))
			return std::string();
		out.push_back(static_cast<char>(std::toupper(u)));
	}
	// (No "." and no "..", and nothing hidden.)
	if (out.front() == '.')
		return std::string();
	return out;
}

class CardGroups
{
public:
	/// A game and the games whose saves it reads. Names that are no serial are passed over.
	void Add(std::string_view serial, const std::vector<std::string>& others)
	{
		const std::string a = CardKey(serial);
		if (a.empty())
			return;
		Find(a);
		for (const std::string& other : others)
		{
			const std::string b = CardKey(other);
			if (b.empty() || b == a)
				continue;
			Join(a, b);
		}
	}

	/// The name a game's card goes by: the smallest serial of its group, or its own when it is in
	/// none. Empty when the serial is nothing a file can be named by.
	std::string KeyOf(std::string_view serial)
	{
		const std::string a = CardKey(serial);
		if (a.empty() || m_parent.find(a) == m_parent.end())
			return a;
		return Find(a);
	}

	/// How many games the lists name.
	size_t Size() const { return m_parent.size(); }

private:
	// (The root of a group is its smallest serial: the smaller root stays one when two join.)
	std::string Find(const std::string& a)
	{
		auto it = m_parent.find(a);
		if (it == m_parent.end())
		{
			m_parent.emplace(a, a);
			return a;
		}
		std::string root = it->second;
		while (true)
		{
			const std::string& up = m_parent[root];
			if (up == root)
				break;
			root = up;
		}
		// Every game on the way points at the root from now on.
		std::string at = a;
		while (at != root)
		{
			std::string next = m_parent[at];
			m_parent[at] = root;
			at = std::move(next);
		}
		return root;
	}

	void Join(const std::string& a, const std::string& b)
	{
		const std::string ra = Find(a), rb = Find(b);
		if (ra == rb)
			return;
		if (ra < rb)
			m_parent[rb] = ra;
		else
			m_parent[ra] = rb;
	}

	std::map<std::string, std::string> m_parent;
};

/// Where a game's texture pack may be at each place (a USB drive's or a share's top folder), in
/// the order they are looked in: beside the games in Crux2/ (PSPCSX2/, as the builds before named it) or PS2/, then at the top.
inline std::vector<std::string> TexturePackCandidates(std::string_view serial, const std::vector<std::string>& places)
{
	std::vector<std::string> out;
	if (CardKey(serial).empty())
		return out;
	static constexpr const char* folders[] = {"Crux2/", "PSPCSX2/", "PS2/", ""};
	for (const std::string& place : places)
	{
		if (place.empty())
			continue;
		std::string base = place;
		while (base.size() > 1 && base.back() == '/')
			base.pop_back();
		for (const char* folder : folders)
			out.push_back(base + "/" + folder + "textures/" + std::string(serial) + "/replacements");
	}
	return out;
}

/// The same places for a pack kept as one ZIP file: <place>/Crux2/textures/<serial>.zip and so on.
inline std::vector<std::string> TexturePackZipCandidates(std::string_view serial, const std::vector<std::string>& places)
{
	std::vector<std::string> out;
	if (CardKey(serial).empty())
		return out;
	static constexpr const char* folders[] = {"Crux2/", "PSPCSX2/", "PS2/", ""};
	for (const std::string& place : places)
	{
		if (place.empty())
			continue;
		std::string base = place;
		while (base.size() > 1 && base.back() == '/')
			base.pop_back();
		for (const char* folder : folders)
			out.push_back(base + "/" + folder + "textures/" + std::string(serial) + ".zip");
	}
	return out;
}

class PlayClock
{
public:
	void Reset()
	{
		m_last = -1.0;
		m_played = 0.0;
	}

	/// A frame of the running game at `now` (seconds, from any start). True when `every` seconds
	/// were played since Saved() or Reset(); `every` of 0 or less is never.
	bool Tick(double now, double every)
	{
		if (m_last >= 0.0)
		{
			const double passed = now - m_last;
			if (passed > 0.0 && passed < 1.0)
				m_played += passed;
		}
		m_last = now;
		return every > 0.0 && m_played >= every;
	}

	void Saved() { m_played = 0.0; }

	/// It could not be saved now (a memory card is being written to): asked again once `seconds`
	/// more were played.
	void Later(double seconds, double every) { m_played = std::max(0.0, every - seconds); }

	double Played() const { return m_played; }

private:
	double m_last = -1.0;
	double m_played = 0.0;
};

} // namespace PS5Extras
