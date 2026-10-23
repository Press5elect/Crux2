// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - host test of the game list's letter jumps (frontend/ps5_letter_jump.h): the
// order titles are sorted in, the group a title is in, and where L2 and R2 land, on lists sorted
// the way the interface sorts them.
#include "ps5_letter_jump.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int checks, failures;
#define CHECK(condition) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static std::vector<std::string> Sorted(std::vector<std::string> titles)
{
	std::sort(titles.begin(), titles.end(), [](const std::string& a, const std::string& b) { return PS5CompareTitles(a, b) < 0; });
	return titles;
}

// The first entry of each run of one group, in list order.
static std::vector<size_t> GroupStarts(const std::vector<std::string>& titles)
{
	std::vector<size_t> starts;
	for (size_t i = 0; i < titles.size(); i++)
	{
		if (i == 0 || PS5LetterGroup(titles[i]) != PS5LetterGroup(titles[i - 1]))
			starts.push_back(i);
	}
	return starts;
}

static void CheckList(const std::vector<std::string>& titles)
{
	const size_t count = titles.size();
	const auto group_of = [&titles](size_t index) { return PS5LetterGroup(titles[index]); };
	const std::vector<size_t> starts = GroupStarts(titles);

	// with the cursor on no game, either trigger goes to the first
	CHECK(PS5LetterJumpTarget(count, count, +1, group_of) == 0);
	CHECK(PS5LetterJumpTarget(count, count, -1, group_of) == 0);
	if (count == 0)
		return;

	for (size_t current = 0; current < count; current++)
	{
		// which run the cursor is in
		size_t run = 0;
		while (run + 1 < starts.size() && starts[run + 1] <= current)
			run++;

		// forward: the first game of the next run, and from the last run the list's first game
		const size_t next = PS5LetterJumpTarget(count, current, +1, group_of);
		CHECK(next == starts[(run + 1) % starts.size()]);

		// back: the first game of the run before, and from the first run that of the last
		const size_t previous = PS5LetterJumpTarget(count, current, -1, group_of);
		CHECK(previous == starts[(run + starts.size() - 1) % starts.size()]);
		CHECK(next < count && previous < count);
	}

	// R2 from the first game visits every group's first game in order and comes round to the
	// start; L2 visits them in the opposite order: A B C ... and back C B A
	std::vector<size_t> forward, backward;
	size_t cursor = 0;
	for (size_t step = 0; step < starts.size(); step++)
	{
		forward.push_back(cursor);
		cursor = PS5LetterJumpTarget(count, cursor, +1, group_of);
	}
	CHECK(cursor == 0);
	CHECK(forward == starts);
	for (size_t step = 0; step < starts.size(); step++)
	{
		cursor = PS5LetterJumpTarget(count, cursor, -1, group_of);
		backward.push_back(cursor);
	}
	std::reverse(backward.begin(), backward.end());
	CHECK(backward == starts);
}

int main()
{
	// groups
	CHECK(PS5LetterGroup("Alpha") == 'A');
	CHECK(PS5LetterGroup("alpha") == 'A');
	CHECK(PS5LetterGroup("zebra") == 'Z');
	// nothing is skipped: a title sorts by its first character as it is
	CHECK(PS5LetterGroup("  Spaced") == '#');
	CHECK(PS5LetterGroup("\tTabbed") == '#');
	CHECK(PS5LetterGroup("007") == '#');
	CHECK(PS5LetterGroup(".dot") == '#');
	CHECK(PS5LetterGroup("") == '#');
	CHECK(PS5LetterGroup("   ") == '#');
	CHECK(PS5LetterGroup("\xc3\x89" "clair") == '#'); // a letter outside A-Z
	CHECK(PS5LetterGroup("[bracket") == '#');

	// the order: case does not part a letter's titles; equal but for case is still an order
	CHECK(PS5CompareTitles("apple", "Banana") < 0);
	CHECK(PS5CompareTitles("Banana", "apple") > 0);
	CHECK(PS5CompareTitles("ico", "Ico") > 0);
	CHECK(PS5CompareTitles("Ico", "ico") < 0);
	CHECK(PS5CompareTitles("Ico", "Ico") == 0);
	CHECK(PS5CompareTitles("Ico", "Ico 2") < 0);
	CHECK(PS5CompareTitles("", "") == 0);
	CHECK(PS5CompareTitles("", "a") < 0);
	CHECK(PS5CompareTitles("1 first", "Alpha") < 0);
	CHECK(PS5CompareTitles("Zed", "\xc3\x89" "clair") < 0);

	// lists as the interface holds them: sorted by title
	CheckList({});
	CheckList({"Only"});
	CheckList({"Same", "Same again", "still S"});
	CheckList(Sorted({"alpha", "Beta", "beta 2", "Gamma", "007", "1 first", "delta", "Delta Force", "zeta", "Zulu", "apex"}));
	{
		const std::vector<std::string> list = Sorted({"ico", "Ico", "island", "Jak", "ICO 2", "java"});
		// one run of I, one of J: case did not part them
		CHECK(GroupStarts(list).size() == 2);
		CheckList(list);
	}

	// a list in an order that is not by title (sorted by size, say): each run of a letter is a
	// stop, and both directions still go round
	CheckList({"Beta", "alpha", "Bravo", "Bingo", "007", "Able", "Zulu", "Baker"});

	// many random lists
	std::mt19937 rng(249);
	for (int round = 0; round < 300; round++)
	{
		std::vector<std::string> titles(rng() % 60);
		for (std::string& title : titles)
		{
			static const char alphabet[] = "abcdeABCDE019 .zZ";
			const size_t length = rng() % 6;
			for (size_t i = 0; i < length; i++)
				title.push_back(alphabet[rng() % (sizeof(alphabet) - 1)]);
		}
		const std::vector<std::string> sorted = Sorted(titles);
		// sorted by title, a letter is one run: no letter is visited twice
		const std::vector<size_t> starts = GroupStarts(sorted);
		std::string seen;
		for (const size_t start : starts)
		{
			const char group = PS5LetterGroup(sorted[start]);
			if (group != '#')
				CHECK(seen.find(group) == std::string::npos);
			seen.push_back(group);
		}
		CheckList(sorted);
		CheckList(titles);
	}

	std::printf("letter jumps: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
