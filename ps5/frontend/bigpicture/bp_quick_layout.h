// Crux2 - the interface: which entries the quick menu shows, and in which order.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From OGBox's quick menu (ps5/src/ogbox_main.cpp, GPL-3.0+, the same author's).
//
// The quick menu's entries have names ("save", "settings", "opt:EmuCore/GS/upscale_multiplier" for a
// game's setting). Triangle in the menu chooses which are shown and moves them; what was chosen is
// kept as one line, the names shown in their order ([PS5UI] QuickLayout), empty for the menu as it
// comes. Resume and Quit are always there. Header-only and without the interface's drawing, so that
// ps5/tests/bigpicture/quick_layout_test.cpp runs it on a PC.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace bp::quicklayout
{
/// The menu as it comes, in its order (the entries that are there only now and then, the auto-save,
/// the undo, the achievements, among them where they come).
inline const std::vector<std::string>& defaults()
{
	static const std::vector<std::string> ids = {"resume", "save", "load", "slot", "autosave", "undo", "achievements",
		"leaderboards", "disc", "gsdump", "savelog", "gameoptions", "patches", "settings", "fastforward", "restart", "quit"};
	return ids;
}

/// Always shown: the way back to the game and the way out of it.
inline bool fixed(const std::string& id)
{
	return id == "resume" || id == "quit";
}

/// A game's setting: "opt:" and the setting's own name.
inline bool isOption(const std::string& id)
{
	return id.rfind("opt:", 0) == 0;
}

/// The names shown, in order, from the kept line: empty is the menu as it comes; spaces and names
/// given twice are left out; Resume first and Quit last when the line has not got them.
inline std::vector<std::string> parse(const std::string& line)
{
	if (line.find_first_not_of(" ,") == std::string::npos)
		return defaults();
	std::vector<std::string> ids;
	size_t at = 0;
	while (at <= line.size())
	{
		const size_t comma = line.find(',', at);
		std::string id = line.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		id.erase(std::remove(id.begin(), id.end(), ' '), id.end());
		if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end())
			ids.push_back(id);
		if (comma == std::string::npos)
			break;
		at = comma + 1;
	}
	if (std::find(ids.begin(), ids.end(), "resume") == ids.end())
		ids.insert(ids.begin(), "resume");
	if (std::find(ids.begin(), ids.end(), "quit") == ids.end())
		ids.push_back("quit");
	return ids;
}

/// The line kept for those names: empty when they are the menu as it comes.
inline std::string serialise(const std::vector<std::string>& ids)
{
	if (ids == defaults())
		return std::string();
	std::string line;
	for (const std::string& id : ids)
		line += (line.empty() ? "" : ",") + id;
	return line;
}

/// The order of the entries there are (`have`, by name) as the layout shows them: indices into
/// `have`, those the layout does not show left out.
inline std::vector<int> order(const std::vector<std::string>& layout, const std::vector<std::string>& have)
{
	std::vector<int> out;
	for (const std::string& id : layout)
	{
		const auto it = std::find(have.begin(), have.end(), id);
		if (it != have.end())
			out.push_back(static_cast<int>(it - have.begin()));
	}
	return out;
}

/// The menu being edited: every entry there can be, the shown ones first in their order, then the
/// others in theirs; and the names the layout shows that are not among them (a setting this game
/// has not got), which are kept as they were.
struct Edit
{
	struct Row
	{
		std::string id;
		bool shown = false;
	};
	std::vector<Row> rows;
	std::vector<std::string> unseen;

	static Edit of(const std::vector<std::string>& layout, const std::vector<std::string>& all)
	{
		Edit e;
		for (const std::string& id : layout)
		{
			if (std::find(all.begin(), all.end(), id) != all.end())
				e.rows.push_back({id, true});
			else
				e.unseen.push_back(id);
		}
		for (const std::string& id : all)
		{
			if (std::find(layout.begin(), layout.end(), id) == layout.end())
				e.rows.push_back({id, fixed(id)});
		}
		return e;
	}

	/// Cross: shown or not (Resume and Quit stay).
	void toggle(int i)
	{
		if (i < 0 || i >= static_cast<int>(rows.size()) || fixed(rows[static_cast<size_t>(i)].id))
			return;
		rows[static_cast<size_t>(i)].shown = !rows[static_cast<size_t>(i)].shown;
	}

	/// Left and right: the entry one place up (-1) or down (+1). Returns where it is now.
	int move(int i, int dir)
	{
		const int j = i + dir;
		if (i < 0 || i >= static_cast<int>(rows.size()) || j < 0 || j >= static_cast<int>(rows.size()))
			return i;
		std::swap(rows[static_cast<size_t>(i)], rows[static_cast<size_t>(j)]);
		return j;
	}

	/// The layout it makes: the shown ones in their order, then those kept unseen.
	std::vector<std::string> layout() const
	{
		std::vector<std::string> ids;
		for (const Row& r : rows)
		{
			if (r.shown)
				ids.push_back(r.id);
		}
		for (const std::string& id : unseen)
			ids.push_back(id);
		return ids;
	}
};

/// Where the cursor is after the entries shown changed (one came, as Apply Changes Now at the top, or
/// went): on the entry it was on, by name, where that is still shown; else at the same place, as far
/// down as there are entries.
inline int keepFocus(const std::vector<std::string>& shown, const std::string& was, int at)
{
	if (shown.empty())
		return 0;
	const auto it = std::find(shown.begin(), shown.end(), was);
	if (!was.empty() && it != shown.end())
		return static_cast<int>(it - shown.begin());
	return std::clamp(at, 0, static_cast<int>(shown.size()) - 1);
}
} // namespace bp::quicklayout
