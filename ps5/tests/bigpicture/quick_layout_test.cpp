// Crux2 - host test of the quick menu's layout (frontend/bigpicture/bp_quick_layout.h): the line kept,
// the order of the entries there are, the editing, and the cursor when entries come or go.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "bp_quick_layout.h"

#include <cstdio>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond)                                                                    \
	do                                                                                 \
	{                                                                                  \
		s_checks++;                                                                    \
		if (!(cond))                                                                   \
		{                                                                              \
			s_failed++;                                                                \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                              \
	} while (0)

using namespace bp::quicklayout;
using Ids = std::vector<std::string>;

int main()
{
	// Nothing kept: the menu as it comes, and that is kept as nothing.
	CHECK(parse("") == defaults());
	CHECK(parse(" , ") == defaults());
	CHECK(serialise(defaults()).empty());
	CHECK(parse(serialise(defaults())) == defaults());

	// A line: spaces and repeats left out, Resume and Quit added where missing.
	CHECK(parse("save, load,save") == (Ids{"resume", "save", "load", "quit"}));
	CHECK(parse("quit,resume,opt:EmuCore/GS/upscale_multiplier") == (Ids{"quit", "resume", "opt:EmuCore/GS/upscale_multiplier"}));
	{
		const Ids mine = {"resume", "opt:EmuCore/GS/upscale_multiplier", "save", "quit"};
		CHECK(serialise(mine) == "resume,opt:EmuCore/GS/upscale_multiplier,save,quit");
		CHECK(parse(serialise(mine)) == mine);
	}

	// The order of the entries there are: with the menu as it comes, today's order; one the layout
	// has that is not there now (no auto-save) is left out, one there that it has not got as well.
	{
		const Ids have = {"resume", "save", "load", "slot", "disc", "gsdump", "gameoptions", "patches", "settings", "fastforward",
			"restart", "quit", "opt:x"};
		const std::vector<int> o = order(defaults(), have);
		CHECK(o.size() == have.size() - 1);
		for (size_t i = 0; i + 1 < have.size(); i++)
			CHECK(o[i] == static_cast<int>(i));
		const std::vector<int> mine = order(Ids{"quit", "opt:x", "resume"}, have);
		CHECK(mine == (std::vector<int>{11, 12, 0}));
	}

	// Editing: the shown ones first, then the others; Resume and Quit cannot be hidden.
	{
		const Ids all = {"resume", "save", "load", "settings", "quit", "opt:a", "opt:b"};
		Edit e = Edit::of(Ids{"resume", "settings", "quit", "opt:gone"}, all);
		CHECK(e.rows.size() == all.size());
		CHECK(e.rows[0].id == "resume" && e.rows[0].shown);
		CHECK(e.rows[1].id == "settings" && e.rows[1].shown);
		CHECK(e.rows[2].id == "quit" && e.rows[2].shown);
		CHECK(e.rows[3].id == "save" && !e.rows[3].shown);
		CHECK(e.unseen == (Ids{"opt:gone"}));
		e.toggle(0);
		e.toggle(2);
		CHECK(e.rows[0].shown && e.rows[2].shown);
		// "opt:b" shown, and moved up to the top; out of range does nothing.
		e.toggle(6);
		int at = 6;
		for (int i = 0; i < 10; i++)
			at = e.move(at, -1);
		CHECK(at == 0 && e.rows[0].id == "opt:b");
		CHECK(e.move(0, -1) == 0);
		CHECK(e.move(static_cast<int>(e.rows.size()) - 1, 1) == static_cast<int>(e.rows.size()) - 1);
		e.toggle(-1);
		e.toggle(99);
		// Settings hidden: the layout has the shown ones in their order, the unseen kept at the end.
		e.toggle(2);
		CHECK(e.layout() == (Ids{"opt:b", "resume", "quit", "opt:gone"}));
		CHECK(parse(serialise(e.layout())) == e.layout());
	}

	// The cursor stays on its entry when Apply Changes Now comes in above it, and at its place when
	// its own entry goes.
	{
		const Ids before = {"resume", "save", "load", "quit"};
		const Ids after = {"apply", "resume", "save", "load", "quit"};
		CHECK(keepFocus(after, before[2], 2) == 3);
		CHECK(keepFocus(before, "apply", 0) == 0);
		CHECK(keepFocus(Ids{"resume", "quit"}, "undo", 5) == 1);
		CHECK(keepFocus(Ids{}, "x", 3) == 0);
		CHECK(keepFocus(before, "", 1) == 1);
	}

	std::printf("quick menu layout: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
