// Crux2 Big Picture - the memory card manager: a card's menu, bringing saves and whole cards in,
// making cards.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Lists to choose from, one after the other (bp.h says what a CardShelf is). Nothing here reads or
// writes a card: the shelf does, and says what came of it.
//
// v1.5.0 made it one manager with a few plain questions:
//   - a card's menu: bring saves in, its saves (each with its own menu), copy the card out,
//     format it, put it back as it was, rename, copy, delete;
//   - bringing saves in asks "from where?" first: a PlayStation 2 on the network (its address,
//     remembered), Crux2's saves folder, each USB drive, each share; then lists what is there:
//     save files of every kind, saves copied off a card as folders, the saves on a real
//     PlayStation 2's cards, and card images (whose saves are listed in turn);
//   - bringing a whole card in: the card images of a place, made a card of Crux2's;
//   - a new card: its size, then its name.

#include "bp_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>

namespace bp
{
namespace
{
std::string sizeText(uint64_t bytes)
{
	char out[32];
	if (bytes >= (1ull << 20))
		std::snprintf(out, sizeof(out), "%.1f MB", static_cast<double>(bytes) / (1ull << 20));
	else
		std::snprintf(out, sizeof(out), "%.0f KB", static_cast<double>(bytes) / 1024.0);
	return out;
}

std::string saveTitle(const CardSave& save)
{
	return save.title.empty() ? save.name : save.title;
}

/// A menu as rows with what each does.
struct Menu
{
	std::vector<std::string> names;
	std::vector<std::function<void()>> acts;

	void add(std::string name, std::function<void()> act)
	{
		names.push_back(std::move(name));
		acts.push_back(std::move(act));
	}
	void open(const std::string& title)
	{
		pick(title, names, -1, [acts = acts](int choice) {
			if (choice >= 0 && choice < static_cast<int>(acts.size()))
				acts[static_cast<size_t>(choice)]();
		});
	}
};

/// Whether cards may be changed now; says why not when they may not.
bool mayChange(CardShelf* shelf)
{
	const std::string why = shelf->cardsBusy();
	if (why.empty())
		return true;
	toast(why, 5.f);
	return false;
}

/// The cards a save can go to, by title (not `except`, and not those that cannot be looked into).
void pickCard(CardShelf* shelf, const std::string& title, const std::string& except, std::function<void(const std::string&)> then)
{
	std::vector<std::string> cards, titles;
	for (const std::string& name : shelf->cardNames())
	{
		if (name == except)
			continue;
		const CardView view = shelf->lookAtCard(name);
		if (view.state == CardView::Closed)
			continue;
		const int slot = shelf->slotOf(name);
		cards.push_back(name);
		titles.push_back(cardTitle(name) + (slot ? "   \xc2\xb7   slot " + std::to_string(slot) : std::string()) + "   \xc2\xb7   " + cardSummary(view));
	}
	if (cards.empty())
	{
		toast("There is no card to put it on: make one first (New card).", 5.f);
		return;
	}
	pick(title, std::move(titles), -1, [cards, then](int choice) {
		if (choice >= 0 && choice < static_cast<int>(cards.size()))
			then(cards[static_cast<size_t>(choice)]);
	});
}

/// "From where?": a source chosen (the PlayStation 2's address asked), then `then` with what was
/// found there. `images`: only card images are wanted.
void chooseSource(CardShelf* shelf, bool images, std::function<void(std::vector<CardFound>)> then)
{
	// (A whole card does not come from a PlayStation 2 over the network: its saves do, one by one.)
	const std::vector<std::string> sources = shelf->cardSources();
	std::vector<std::string> names;
	std::vector<int> index;
	for (size_t i = images ? 1 : 0; i < sources.size(); i++)
	{
		names.push_back(sources[i]);
		index.push_back(static_cast<int>(i));
	}
	pick(images ? "Bring a card in from where?" : "Bring saves in from where?", std::move(names), -1, [shelf, images, then, index](int picked) {
		if (picked < 0 || picked >= static_cast<int>(index.size()))
			return;
		const int choice = index[static_cast<size_t>(picked)];
		const auto look = [shelf, images, then](int source, const std::string& address) {
			toast(source == 0 ? "Asking the PlayStation 2 for its memory cards..." : "Looking...", 3.f);
			shelf->findIn(source, address, [shelf, images, then, source](std::vector<CardFound> found, std::string why) {
				std::vector<CardFound> wanted;
				for (CardFound& f : found)
				{
					if (!images || f.kind == CardFound::CardImage)
						wanted.push_back(std::move(f));
				}
				if (wanted.empty())
				{
					if (source == 0)
					{
						message("No saves from the PlayStation 2",
							(why.empty() ? std::string("Its memory cards hold no save.") : "It did not answer as wanted: " + why + ".") +
								"\n\nOn the PlayStation 2, start wLaunchELF, choose MISC, then FTPD (its FTP server), with the network set up "
								"in its settings; its address is what wLaunchELF shows. Both consoles must be on the same network.");
					}
					else
					{
						message(images ? "No card was found" : "Nothing to bring in was found",
							(why.empty() ? std::string() : why + ".\n\n") +
								(images ? "Card images (.ps2, .mcd, .mc2, .bin, .vmc, .vm2) of 8 to 64 MB are looked for at the top, in memcards/, "
								          "Crux2/memory cards/, VMC/ and PS2/VMC/ (Open PS2 Loader's cards)." :
								          "Save files (.psu, .max, .cbs, .sps, .xps, .psv), saves copied as folders and card images are looked for "
								          "at the top, in saves/, Crux2/saves/, PS2/SAVES/, memcards/, VMC/ and PS2/VMC/."));
					}
					(void)shelf;
					return;
				}
				then(std::move(wanted));
			});
		};
		if (choice == 0)
		{
			editAddress("The PlayStation 2's address", shelf->ps2Address(), "192.168.1.20", [look](const std::string& address) {
				if (!address.empty())
					look(0, address);
			});
			return;
		}
		look(choice, std::string());
	});
}

void imageSaves(CardShelf* shelf, const CardFound& image, const std::string& card)
{
	const CardView view = shelf->lookAtImage(image);
	if (view.state != CardView::Listed || view.saves.empty())
	{
		message(image.name, view.state == CardView::Listed ? std::string("This card holds no save.") :
		                    view.state == CardView::New ? std::string("Nobody formatted this card yet: it holds nothing.") :
		                                                  view.why);
		return;
	}
	std::vector<std::string> names;
	for (const CardSave& save : view.saves)
		names.push_back(saveTitle(save) + "   \xc2\xb7   " + sizeText(save.bytes) + (save.when.empty() ? "" : "   \xc2\xb7   " + save.when));
	pick("Which save of " + image.name + "?", std::move(names), -1, [shelf, image, card, saves = view.saves](int choice) {
		if (choice >= 0 && choice < static_cast<int>(saves.size()) && mayChange(shelf))
			shelf->copyFromImage(image, saves[static_cast<size_t>(choice)].name, card);
	});
}

void saveMenu(CardShelf* shelf, const std::string& card, const CardSave& save)
{
	Menu menu;
	menu.add("Copy it to another card", [shelf, card, save] {
		if (!mayChange(shelf))
			return;
		pickCard(shelf, "Copy \"" + saveTitle(save) + "\" to", card, [shelf, card, save](const std::string& to) {
			if (mayChange(shelf))
				shelf->copySave(card, save.name, to);
		});
	});
	menu.add("Copy it out as a file (.psu)", [shelf, card, save] {
		if (!mayChange(shelf))
			return;
		const std::vector<std::string> places = shelf->exportPlaces();
		if (places.size() <= 1)
		{
			shelf->exportSave(card, save.name, 0);
			return;
		}
		pick("Copy \"" + saveTitle(save) + "\" out to", places, -1, [shelf, card, save](int place) {
			if (place >= 0 && mayChange(shelf))
				shelf->exportSave(card, save.name, place);
		});
	});
	menu.add("Delete it", [shelf, card, save] {
		if (!mayChange(shelf))
			return;
		confirm("Delete this save?",
			"\"" + saveTitle(save) + "\" goes from " + cardTitle(card) +
				". Until the card is next changed, \"Undo the last change\" in the card's menu brings it back.",
			[shelf, card, save](bool yes) {
				if (yes && mayChange(shelf))
					shelf->deleteSave(card, save.name);
			},
			"Delete", "Keep");
	});
	menu.open(saveTitle(save) + "   \xc2\xb7   " + sizeText(save.bytes) + (save.when.empty() ? "" : "   \xc2\xb7   " + save.when));
}
} // namespace

std::string cardTitle(const std::string& card)
{
	const size_t dot = card.rfind('.');
	return (dot == std::string::npos || dot == 0) ? card : card.substr(0, dot);
}

std::string cardSummary(const CardView& view)
{
	switch (view.state)
	{
		case CardView::Listed:
			return (view.saves.empty() ? std::string("empty") : std::to_string(view.saves.size()) + (view.saves.size() == 1 ? " save" : " saves")) + ", " +
				   sizeText(view.freeBytes) + " free of " + sizeText(view.totalBytes);
		case CardView::New:
			return "new: nothing on it yet";
		default:
			return view.why;
	}
}

void bringSavesInto(CardShelf* shelf, const std::string& card)
{
	if (!mayChange(shelf))
		return;
	chooseSource(shelf, false, [shelf, card](std::vector<CardFound> found) {
		std::vector<std::string> names;
		for (const CardFound& f : found)
		{
			const std::string kind = (f.kind == CardFound::CardImage) ? " (a card: its saves)" : (f.kind == CardFound::SaveFolder) ? " (a save folder)" : "";
			names.push_back(f.name + kind + "   \xc2\xb7   " + f.where + "   \xc2\xb7   " + sizeText(f.bytes));
		}
		pick("Bring which in to " + cardTitle(card) + "?", std::move(names), -1, [shelf, card, found](int choice) {
			if (choice < 0 || choice >= static_cast<int>(found.size()) || !mayChange(shelf))
				return;
			const CardFound& f = found[static_cast<size_t>(choice)];
			if (f.kind == CardFound::CardImage)
				imageSaves(shelf, f, card);
			else
				shelf->bringIn(card, f);
		});
	});
}

void bringCardsIn(CardShelf* shelf)
{
	if (!mayChange(shelf))
		return;
	chooseSource(shelf, true, [shelf](std::vector<CardFound> found) {
		std::vector<std::string> names;
		for (const CardFound& f : found)
			names.push_back(f.name + "   \xc2\xb7   " + f.where + "   \xc2\xb7   " + sizeText(f.bytes));
		pick("Bring which card in?", std::move(names), -1, [shelf, found](int choice) {
			if (choice < 0 || choice >= static_cast<int>(found.size()) || !mayChange(shelf))
				return;
			const CardFound f = found[static_cast<size_t>(choice)];
			editText("The new card's name", cardTitle(f.name), false, [shelf, f](const std::string& typed) {
				if (mayChange(shelf))
					shelf->bringInCard(f, typed.empty() ? cardTitle(f.name) : typed);
			}, 40);
		});
	});
}

void newCard(CardShelf* shelf)
{
	if (!mayChange(shelf))
		return;
	static const int sizes[] = {8, 16, 32, 64, 0};
	pick("A new card of which size?",
		{"8 MB: what every PlayStation 2 game expects", "16 MB", "32 MB", "64 MB: room for many saves (some games take no card over 8 MB)",
			"A PlayStation 1 card"},
		0, [shelf](int choice) {
			if (choice < 0 || choice >= static_cast<int>(std::size(sizes)))
				return;
			const int mb = sizes[choice];
			int number = 1;
			const std::vector<std::string> names = shelf->cardNames();
			for (;; number++)
			{
				const std::string title = "Card " + std::to_string(number);
				if (std::find_if(names.begin(), names.end(), [&](const std::string& n) { return cardTitle(n) == title; }) == names.end())
					break;
			}
			editText("The new card's name", "Card " + std::to_string(number), false, [shelf, mb](const std::string& typed) {
				if (!typed.empty() && mayChange(shelf))
					shelf->createCard(typed, mb);
			}, 40);
		});
}

// The menu of what can be done with a card itself (the card manager's Triangle): its saves are in
// the manager's right pane, so they are not listed here.
void cardOptions(CardShelf* shelf, const std::string& card, const std::string& shownAs);

void openCard(CardShelf* shelf, const std::string& card)
{
	const CardView view = shelf->lookAtCard(card);
	Menu menu;
	if (view.state != CardView::Closed)
	{
		menu.add("Bring saves in...", [shelf, card] { bringSavesInto(shelf, card); });
		for (const CardSave& save : view.saves)
			menu.add(saveTitle(save) + "   \xc2\xb7   " + sizeText(save.bytes) + (save.when.empty() ? "" : "   \xc2\xb7   " + save.when),
				[shelf, card, save] { saveMenu(shelf, card, save); });
	}
	if (view.undo)
	{
		menu.add("Undo the last change", [shelf, card] {
			if (!mayChange(shelf))
				return;
			confirm("Undo the last change?",
				cardTitle(card) + " goes back to what it was before a save was last brought in, copied to it or deleted here, or it was "
				"formatted. What a game saved to it since then is lost.",
				[shelf, card](bool yes) {
					if (yes && mayChange(shelf))
						shelf->undoCard(card);
				},
				"Undo", "Leave it");
		});
	}
	const std::vector<std::string> places = shelf->exportPlaces();
	if (places.size() > 1)
	{
		menu.add("Copy the whole card out to a USB drive or a share", [shelf, card, places] {
			std::vector<std::string> others(places.begin() + 1, places.end());
			pick("Copy " + cardTitle(card) + " out to (Crux2/memory cards there)", std::move(others), -1, [shelf, card](int choice) {
				if (choice >= 0)
					shelf->exportCard(card, choice + 1);
			});
		});
	}
	if (view.state != CardView::Closed)
	{
		menu.add("Format the card (erase every save)...", [shelf, card, view] {
			if (!mayChange(shelf))
				return;
			const std::string holds = (view.state == CardView::Listed && !view.saves.empty()) ?
				"The " + std::to_string(view.saves.size()) + (view.saves.size() == 1 ? " save" : " saves") + " on it " + (view.saves.size() == 1 ? "is" : "are") + " erased" :
				"It is made empty";
			confirm("Format " + cardTitle(card) + "?", holds + ". Until the card is next changed, \"Undo the last change\" brings them back.",
				[shelf, card](bool yes) {
					if (yes && mayChange(shelf))
						shelf->formatCard(card);
				},
				"Format", "Keep it");
		});
	}
	menu.add("Rename the card...", [shelf, card] {
		if (!mayChange(shelf))
			return;
		editText("The card's new name", cardTitle(card), false, [shelf, card](const std::string& typed) {
			if (mayChange(shelf))
				shelf->renameCard(card, typed);
		}, 40);
	});
	menu.add("Make a copy of the card", [shelf, card] {
		if (mayChange(shelf))
			shelf->duplicateCard(card);
	});
	menu.add("Delete the card...", [shelf, card, view] {
		if (!mayChange(shelf))
			return;
		const std::string holds = (view.state == CardView::Listed && !view.saves.empty()) ?
			"The " + std::to_string(view.saves.size()) + (view.saves.size() == 1 ? " save" : " saves") + " on it " + (view.saves.size() == 1 ? "goes" : "go") + " with it" :
			(view.state == CardView::Closed) ? "Whatever is on it goes with it" : "There is nothing on it";
		confirm("Delete " + cardTitle(card) + "?", holds + ", and this cannot be undone. (A backup holds the cards as they were when it was made.)",
			[shelf, card](bool yes) {
				if (yes && mayChange(shelf))
					shelf->deleteCard(card);
			},
			"Delete the card", "Keep it");
	});
	const int slot = shelf->slotOf(card);
	menu.open(cardTitle(card) + (slot ? "  (slot " + std::to_string(slot) + ")" : std::string()) + ": " + cardSummary(view));
}

// ============================================================================ the card manager
//
// v1.6.0, after PSFlyCast's: one sheet over the settings, in two panes.
//
//   left   what can be made or brought in, then the cards: GAMES (a game's own card, with its
//          cover and its title, the serial under it), MY CARDS (made or brought in here) and
//          SYSTEM (the emulator's own Mcd001 and Mcd002); each says its saves, its free room and
//          the slot it is in;
//   right  the card in focus: its name, a bar of how full it is, and its saves, newest first.
//
// Up / Down move, Right or Cross go into a card's saves, Left or Circle come back, Cross on a save
// opens what can be done with it, Triangle what can be done with the card, Square brings saves in
// to it. Circle on the left closes the manager. The lists are made again whenever the shelf says
// a card changed (the Backend's pagesGeneration).

void cardOptions(CardShelf* shelf, const std::string& card, const std::string& shownAs)
{
	const CardView view = shelf->lookAtCard(card);
	Menu menu;
	if (view.state != CardView::Closed)
		menu.add("Bring saves in...", [shelf, card] { bringSavesInto(shelf, card); });
	if (view.undo)
	{
		menu.add("Undo the last change", [shelf, card, shownAs] {
			if (!mayChange(shelf))
				return;
			confirm("Undo the last change?",
				shownAs + " goes back to what it was before a save was last brought in, copied to it or deleted here, or it was "
				"formatted. What a game saved to it since then is lost.",
				[shelf, card](bool yes) {
					if (yes && mayChange(shelf))
						shelf->undoCard(card);
				},
				"Undo", "Leave it");
		});
	}
	const std::vector<std::string> places = shelf->exportPlaces();
	if (places.size() > 1)
	{
		menu.add("Copy the card out to a USB drive or a share", [shelf, card, places, shownAs] {
			std::vector<std::string> others(places.begin() + 1, places.end());
			pick("Copy " + shownAs + " out to (Crux2/memory cards there)", std::move(others), -1, [shelf, card](int choice) {
				if (choice >= 0)
					shelf->exportCard(card, choice + 1);
			});
		});
	}
	if (view.state != CardView::Closed)
	{
		menu.add("Format the card...", [shelf, card, view, shownAs] {
			if (!mayChange(shelf))
				return;
			const std::string holds = (view.state == CardView::Listed && !view.saves.empty()) ?
				"The " + std::to_string(view.saves.size()) + (view.saves.size() == 1 ? " save" : " saves") + " on it " +
					(view.saves.size() == 1 ? "is" : "are") + " erased" :
				"It is made empty";
			confirm("Format " + shownAs + "?", holds + ". Until the card is next changed, \"Undo the last change\" brings them back.",
				[shelf, card](bool yes) {
					if (yes && mayChange(shelf))
						shelf->formatCard(card);
				},
				"Format", "Keep it");
		});
	}
	menu.add("Rename the card...", [shelf, card] {
		if (!mayChange(shelf))
			return;
		editText("The card's new name", cardTitle(card), false, [shelf, card](const std::string& typed) {
			if (mayChange(shelf))
				shelf->renameCard(card, typed);
		}, 40);
	});
	menu.add("Make a copy of the card", [shelf, card] {
		if (mayChange(shelf))
			shelf->duplicateCard(card);
	});
	menu.add("Delete the card...", [shelf, card, view, shownAs] {
		if (!mayChange(shelf))
			return;
		const std::string holds = (view.state == CardView::Listed && !view.saves.empty()) ?
			"The " + std::to_string(view.saves.size()) + (view.saves.size() == 1 ? " save" : " saves") + " on it " +
				(view.saves.size() == 1 ? "goes" : "go") + " with it" :
			(view.state == CardView::Closed) ? "Whatever is on it goes with it" : "There is nothing on it";
		confirm("Delete " + shownAs + "?", holds + ", and this cannot be undone. (A backup holds the cards as they were when it was made.)",
			[shelf, card](bool yes) {
				if (yes && mayChange(shelf))
					shelf->deleteCard(card);
			},
			"Delete the card", "Keep it");
	});
	menu.open(shownAs);
}

namespace
{
struct ManagerRow
{
	enum Kind { Heading, Action, Card } kind = Card;
	std::string label;   // a heading's or an action's words; a card's title
	std::string sub;     // a card's second line: its serial or file, what it holds
	std::string card;    // the card's file
	int slot = 0;
	int megabytes = 8;
	bool hasGame = false;
	GameInfo game;       // a game's card: the game, for its cover
	CardView view;
	std::function<void()> act;
};

struct Manager
{
	bool open = false;
	CardShelf* shelf = nullptr;
	unsigned generation = ~0u;
	std::vector<ManagerRow> rows;
	int focus = 0;        // the left pane's row
	int save = 0;         // the right pane's save
	bool inSaves = false; // the right pane has the controller
	float scroll = 0, saveScroll = 0, shown = 0;
	std::string keep;     // the card focused before the lists were made again
} s_manager;

constexpr float MGR_W = 1760, MGR_H = 852, MGR_X = (1920 - MGR_W) / 2, MGR_Y = 118;
constexpr float LEFT_W = 720, ROW_H = 96, HEAD_H = 50, SAVE_H = 74;

bool isSystemName(const std::string& title)
{
	// Mcd001 .. Mcd999: PCSX2's own cards, the ones every installation begins with.
	if (title.size() != 6 || title.compare(0, 3, "Mcd") != 0)
		return false;
	return std::isdigit(static_cast<unsigned char>(title[3])) && std::isdigit(static_cast<unsigned char>(title[4])) &&
		   std::isdigit(static_cast<unsigned char>(title[5]));
}

std::string plural(size_t n, const char* one, const char* many)
{
	return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string holdsLine(const CardView& view, int megabytes)
{
	std::string size = megabytes > 0 ? std::to_string(megabytes) + " MB" : megabytes == 0 ? std::string("PlayStation 1") : std::string("Folder");
	switch (view.state)
	{
		case CardView::Listed:
			return size + "   \xc2\xb7   " + (view.saves.empty() ? std::string("empty") : plural(view.saves.size(), "save", "saves")) + "   \xc2\xb7   " +
				   sizeText(view.freeBytes) + " free";
		case CardView::New:
			return size + "   \xc2\xb7   not formatted yet";
		default:
			return size + "   \xc2\xb7   " + view.why;
	}
}

void rebuild()
{
	Manager& m = s_manager;
	CardShelf* shelf = m.shelf;
	m.generation = be->pagesGeneration();
	const std::string keep = (m.focus >= 0 && m.focus < static_cast<int>(m.rows.size())) ? m.rows[static_cast<size_t>(m.focus)].card : m.keep;
	m.rows.clear();

	// The library's games, by serial: a card's game is shown with its cover and the title the
	// library gives it.
	std::vector<GameInfo> games = be->games();
	const auto gameOf = [&games](const std::string& serial) -> const GameInfo* {
		for (const GameInfo& g : games)
		{
			if (!serial.empty() && g.serial == serial && g.kind == GameInfo::PS2)
				return &g;
		}
		return nullptr;
	};

	std::vector<ManagerRow> gamesRows, mine, system;
	for (const std::string& card : shelf->cardNames())
	{
		ManagerRow r;
		r.kind = ManagerRow::Card;
		r.card = card;
		r.slot = shelf->slotOf(card);
		r.megabytes = shelf->cardMegabytes(card);
		r.view = shelf->lookAtCard(card);
		const std::string title = cardTitle(card);
		const std::string serial = shelf->cardSerial(card);
		const GameInfo* game = gameOf(serial);
		const std::string named = shelf->cardGame(card);
		if (!serial.empty() && (game != nullptr || !named.empty()))
		{
			r.label = game != nullptr ? game->title : named;
			if (game != nullptr)
			{
				r.hasGame = true;
				r.game = *game;
			}
			r.sub = serial + "   \xc2\xb7   " + holdsLine(r.view, r.megabytes);
			gamesRows.push_back(std::move(r));
		}
		else if (shelf->systemCard(card) || isSystemName(title))
		{
			r.label = title;
			r.sub = holdsLine(r.view, r.megabytes);
			system.push_back(std::move(r));
		}
		else
		{
			r.label = title;
			r.sub = holdsLine(r.view, r.megabytes);
			mine.push_back(std::move(r));
		}
	}
	const auto byTitle = [](const ManagerRow& a, const ManagerRow& b) {
		return std::lexicographical_compare(a.label.begin(), a.label.end(), b.label.begin(), b.label.end(),
			[](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
	};
	std::sort(gamesRows.begin(), gamesRows.end(), byTitle);
	std::sort(mine.begin(), mine.end(), byTitle);
	std::sort(system.begin(), system.end(), byTitle);

	const auto heading = [](const char* label) {
		ManagerRow h;
		h.kind = ManagerRow::Heading;
		h.label = label;
		m.rows.push_back(std::move(h));
	};
	const auto action = [](const char* label, const char* sub, std::function<void()> act) {
		ManagerRow a;
		a.kind = ManagerRow::Action;
		a.label = label;
		a.sub = sub;
		a.act = std::move(act);
		m.rows.push_back(std::move(a));
	};
	action(ICON_FA_PLUS "   New card", "8 to 64 MB, or a PlayStation 1 card", [shelf] { newCard(shelf); });
	action(ICON_FA_DOWNLOAD "   Bring a card in", "A card image from a USB drive or a share", [shelf] { bringCardsIn(shelf); });
	if (!gamesRows.empty())
	{
		heading("GAMES");
		for (ManagerRow& r : gamesRows)
			m.rows.push_back(std::move(r));
	}
	if (!mine.empty())
	{
		heading("MY CARDS");
		for (ManagerRow& r : mine)
			m.rows.push_back(std::move(r));
	}
	if (!system.empty())
	{
		heading("SYSTEM");
		for (ManagerRow& r : system)
			m.rows.push_back(std::move(r));
	}

	// The focus stays on the card it was on, when it is still there.
	int found = -1;
	for (size_t i = 0; i < m.rows.size(); i++)
	{
		if (m.rows[i].kind == ManagerRow::Card && !keep.empty() && m.rows[i].card == keep)
			found = static_cast<int>(i);
	}
	if (found < 0)
	{
		found = std::clamp(m.focus, 0, std::max(0, static_cast<int>(m.rows.size()) - 1));
		while (found < static_cast<int>(m.rows.size()) - 1 && m.rows[static_cast<size_t>(found)].kind == ManagerRow::Heading)
			found++;
	}
	m.focus = found;
	m.keep.clear();
	const ManagerRow* r = (m.focus >= 0 && m.focus < static_cast<int>(m.rows.size())) ? &m.rows[static_cast<size_t>(m.focus)] : nullptr;
	const int saves = (r != nullptr && r->kind == ManagerRow::Card) ? static_cast<int>(r->view.saves.size()) : 0;
	m.save = std::clamp(m.save, 0, std::max(0, saves - 1));
	if (saves == 0)
		m.inSaves = false;
}

ManagerRow* focused()
{
	Manager& m = s_manager;
	return (m.focus >= 0 && m.focus < static_cast<int>(m.rows.size())) ? &m.rows[static_cast<size_t>(m.focus)] : nullptr;
}

/// The saves of a card, newest first.
std::vector<CardSave> sortedSaves(const CardView& view)
{
	std::vector<CardSave> saves = view.saves;
	std::stable_sort(saves.begin(), saves.end(), [](const CardSave& a, const CardSave& b) { return a.when > b.when; });
	return saves;
}

void moveFocus(int step)
{
	Manager& m = s_manager;
	const int n = static_cast<int>(m.rows.size());
	if (n == 0)
		return;
	int at = m.focus;
	for (int tries = 0; tries < n; tries++)
	{
		at += step;
		if (at < 0 || at >= n)
			return; // (the lists stop at their ends)
		if (m.rows[static_cast<size_t>(at)].kind != ManagerRow::Heading)
		{
			m.focus = at;
			m.save = 0;
			m.saveScroll = 0;
			if (be != nullptr)
				be->cue(CueFocus);
			return;
		}
	}
}
} // namespace

bool cardManagerOpen()
{
	return s_manager.open;
}

void openCardManager(CardShelf* shelf)
{
	Manager& m = s_manager;
	m.open = true;
	m.shelf = shelf;
	m.focus = 0;
	m.save = 0;
	m.inSaves = false;
	m.scroll = m.saveScroll = 0;
	m.shown = 0;
	m.generation = ~0u;
	if (be != nullptr)
		be->cue(CueModalOpen);
	holdInput();
}

void closeCardManager()
{
	if (!s_manager.open)
		return;
	s_manager.open = false;
	s_manager.rows.clear();
	if (be != nullptr)
		be->cue(CueModalClose);
}

void cardManagerInput(const Input& now)
{
	Manager& m = s_manager;
	if (!m.open || m.shelf == nullptr)
		return;
	if (m.generation != be->pagesGeneration())
		rebuild();
	ManagerRow* row = focused();
	const bool isCard = row != nullptr && row->kind == ManagerRow::Card;
	const std::vector<CardSave> saves = isCard ? sortedSaves(row->view) : std::vector<CardSave>();
	const std::string shownAs = isCard ? row->label : std::string();

	if (now.triangle && isCard)
	{
		cardOptions(m.shelf, row->card, shownAs);
		return;
	}
	if (now.square && isCard)
	{
		bringSavesInto(m.shelf, row->card);
		return;
	}
	if (m.inSaves)
	{
		const int n = static_cast<int>(saves.size());
		if (n == 0 || now.left || now.back)
		{
			m.inSaves = false;
			if (be != nullptr)
				be->cue(CueBack);
			return;
		}
		if (now.up && m.save > 0)
		{
			m.save--;
			if (be != nullptr)
				be->cue(CueFocus);
		}
		if (now.down && m.save < n - 1)
		{
			m.save++;
			if (be != nullptr)
				be->cue(CueFocus);
		}
		if (now.l2)
			m.save = 0;
		if (now.r2)
			m.save = n - 1;
		if (now.accept)
		{
			if (be != nullptr)
				be->cue(CueSelect);
			saveMenu(m.shelf, row->card, saves[static_cast<size_t>(std::clamp(m.save, 0, n - 1))]);
		}
		return;
	}

	if (now.back)
	{
		closeCardManager();
		holdInput();
		return;
	}
	if (now.up)
		moveFocus(-1);
	if (now.down)
		moveFocus(1);
	if (now.l2)
	{
		m.focus = 0;
		moveFocus(0);
	}
	if (now.r2)
	{
		m.focus = static_cast<int>(m.rows.size()) - 1;
		while (m.focus > 0 && m.rows[static_cast<size_t>(m.focus)].kind == ManagerRow::Heading)
			m.focus--;
	}
	row = focused();
	if (row == nullptr)
		return;
	if (now.accept && row->kind == ManagerRow::Action)
	{
		if (be != nullptr)
			be->cue(CueSelect);
		const std::function<void()> act = row->act;
		act();
		return;
	}
	if ((now.accept || now.right) && row->kind == ManagerRow::Card)
	{
		if (row->view.state == CardView::Listed && !row->view.saves.empty())
		{
			m.inSaves = true;
			m.save = std::clamp(m.save, 0, static_cast<int>(row->view.saves.size()) - 1);
			if (be != nullptr)
				be->cue(CueOpen);
		}
		else if (now.accept)
		{
			// Nothing to go into: what can be done with the card instead.
			cardOptions(m.shelf, row->card, row->label);
		}
	}
}

void cardManagerDraw(ImDrawList* dl)
{
	Manager& m = s_manager;
	if (!m.open || m.shelf == nullptr)
		return;
	if (m.generation != be->pagesGeneration())
		rebuild();
	m.shown = approach(m.shown, 1, 14);
	const float t = m.shown;
	dl->AddRectFilled(V(0, 0), V(W, H), col::rgba(0, 0, 0, static_cast<int>(180 * t)));
	const float x = MGR_X, y = MGR_Y + (1 - t) * 24, w = MGR_W, h = MGR_H;
	glow(dl, x, y, w, h, 26 * col::round, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
	rect(dl, x, y, w, h, alpha(col::panel, t), 26 * col::round);

	// The title, and what the slots hold.
	text(dl, bold(), 40, x + 48, y + 34, alpha(col::text, t), ICON_FA_SD_CARD "   Memory cards");
	{
		std::string slots;
		for (int slot = 1; slot <= 2; slot++)
		{
			std::string in = "empty";
			for (const ManagerRow& r : m.rows)
			{
				if (r.kind == ManagerRow::Card && r.slot == slot)
					in = r.label;
			}
			slots += (slot == 1 ? "" : "      ") + std::string("Slot ") + std::to_string(slot) + ":  " + in;
		}
		const float sw = std::min(textSize(regular(), 24, slots.c_str()).x, w - 560);
		textIn(dl, regular(), 24, x + w - 48 - sw, y + 46, alpha(col::dim, t), slots, sw, false);
	}
	rect(dl, x + 32, y + 104, w - 64, 1, alpha(col::line, t));

	// ---- the left pane: the cards
	const float lx = x + 24, ly = y + 118, lw = LEFT_W, lh = h - 118 - 24;
	float content = 0, focusTop = 0, focusH = ROW_H;
	for (size_t i = 0; i < m.rows.size(); i++)
	{
		const float rh = (m.rows[i].kind == ManagerRow::Heading) ? HEAD_H : ROW_H;
		if (static_cast<int>(i) == m.focus)
		{
			focusTop = content;
			focusH = rh;
		}
		content += rh;
	}
	// (The row in focus stays in the middle third of the pane.)
	float wanted = m.scroll;
	if (focusTop - wanted < lh * 0.25f)
		wanted = focusTop - lh * 0.25f;
	if (focusTop + focusH - wanted > lh * 0.75f)
		wanted = focusTop + focusH - lh * 0.75f;
	wanted = std::clamp(wanted, 0.f, std::max(0.f, content - lh));
	m.scroll = approach(m.scroll, wanted, 16);

	dl->PushClipRect(V(lx, ly), V(lx + lw, ly + lh), true);
	float ry = ly - m.scroll;
	for (size_t i = 0; i < m.rows.size(); i++)
	{
		const ManagerRow& r = m.rows[i];
		const float rh = (r.kind == ManagerRow::Heading) ? HEAD_H : ROW_H;
		if (ry + rh < ly || ry > ly + lh)
		{
			ry += rh;
			continue;
		}
		if (r.kind == ManagerRow::Heading)
		{
			text(dl, bold(), 20, lx + 24, ry + 18, alpha(col::accent, t), r.label.c_str());
			const ImVec2 ts = textSize(bold(), 20, r.label.c_str());
			rect(dl, lx + 24 + ts.x + 16, ry + 30, lw - 48 - ts.x - 16, 1, alpha(col::line, t));
			ry += rh;
			continue;
		}
		const bool on = static_cast<int>(i) == m.focus;
		if (on)
		{
			rect(dl, lx + 8, ry + 4, lw - 16, rh - 8, alpha(m.inSaves ? col::panelHi : col::accent, t * (m.inSaves ? 1.f : 0.92f)), 16 * col::round);
		}
		const ImU32 ink = alpha(on && !m.inSaves ? col::onAccent : col::text, t);
		const ImU32 faint = alpha(on && !m.inSaves ? col::onAccent : col::dim, t * (on && !m.inSaves ? 0.8f : 1.f));
		float tx = lx + 28;
		if (r.kind == ManagerRow::Card)
		{
			// The game's cover, or a card.
			const float cw = 56, ch = 78, cx = tx, cy = ry + (rh - ch) / 2;
			Texture cover = r.hasGame ? be->cover(r.game) : Texture{};
			if (cover.valid())
				imageFit(dl, cover, cx, cy, cw, ch, 6, alpha(col::white, t));
			else
			{
				rect(dl, cx, cy, cw, ch, alpha(col::sheet, t), 8);
				const char* icon = ICON_FA_SD_CARD;
				const ImVec2 is = textSize(regular(), 30, icon);
				text(dl, regular(), 30, cx + (cw - is.x) / 2, cy + (ch - is.y) / 2, alpha(col::dim, t), icon);
			}
			tx += cw + 22;
			float right = lx + lw - 28;
			if (r.slot > 0)
			{
				const std::string badge = "Slot " + std::to_string(r.slot);
				const ImVec2 bs = textSize(bold(), 20, badge.c_str());
				const float bx = right - bs.x - 24, by = ry + (rh - 34) / 2;
				rect(dl, bx, by, bs.x + 24, 34, alpha(on && !m.inSaves ? col::onAccent : col::good, t * 0.2f), 17);
				text(dl, bold(), 20, bx + 12, by + 17 - bs.y / 2, alpha(on && !m.inSaves ? col::onAccent : col::good, t), badge.c_str());
				right = bx - 16;
			}
			textIn(dl, bold(), 26, tx, ry + 16, ink, r.label, right - tx, on);
			textIn(dl, regular(), 20, tx, ry + 54, faint, r.sub, right - tx, false);
		}
		else
		{
			textIn(dl, bold(), 26, tx, ry + 16, ink, r.label, lw - 56, on);
			textIn(dl, regular(), 20, tx, ry + 54, faint, r.sub, lw - 56, false);
		}
		ry += rh;
	}
	dl->PopClipRect();

	// ---- the right pane: the card in focus
	const float px = x + 24 + LEFT_W + 24, py = y + 118, pw = w - (px - x) - 32, ph = h - 118 - 24;
	rect(dl, px, py, pw, ph, alpha(col::sheet, t), 20 * col::round);
	const ManagerRow* row = focused();
	if (row == nullptr || row->kind != ManagerRow::Card)
	{
		const char* says = (row != nullptr && row->kind == ManagerRow::Action) ? row->sub.c_str() : "No card yet: make one, or bring one in.";
		text(dl, regular(), 26, px + 40, py + 40, alpha(col::dim, t), says, pw - 80);
	}
	else
	{
		const CardView& v = row->view;
		textIn(dl, bold(), 34, px + 40, py + 30, alpha(col::text, t), row->label, pw - 80, !m.inSaves);
		// Under the name: the card's file when the name is a game's, and where the card is.
		const std::string where = row->slot > 0 ? "In slot " + std::to_string(row->slot) : std::string("Not in a slot");
		const std::string file = (row->label != cardTitle(row->card) ? cardTitle(row->card) + "   \xc2\xb7   " : std::string()) + where;
		textIn(dl, regular(), 22, px + 40, py + 78, alpha(col::dim, t), file, pw - 80, false);
		float sy = py + 122;
		if (v.state == CardView::Listed && v.totalBytes > 0)
		{
			// How full it is.
			const float used = 1.f - static_cast<float>(v.freeBytes) / static_cast<float>(v.totalBytes);
			const float bw = pw - 80;
			rect(dl, px + 40, sy, bw, 12, alpha(col::line, t), 6);
			rect(dl, px + 40, sy, std::max(12.f, bw * std::clamp(used, 0.f, 1.f)), 12, alpha(used > 0.9f ? col::bad : col::accent, t), 6);
			const std::string room = sizeText(v.totalBytes - v.freeBytes) + " used   \xc2\xb7   " + sizeText(v.freeBytes) + " free of " +
									 sizeText(v.totalBytes) + "   \xc2\xb7   " + plural(v.saves.size(), "save", "saves");
			text(dl, regular(), 20, px + 40, sy + 22, alpha(col::dim, t), room.c_str());
			sy += 64;
		}
		rect(dl, px + 24, sy, pw - 48, 1, alpha(col::line, t));
		sy += 12;
		const float listH = py + ph - sy - 12;
		if (v.state != CardView::Listed || v.saves.empty())
		{
			const std::string says = v.state == CardView::New ? std::string("Nobody formatted this card yet. A game formats it when it first saves, or Triangle formats it now.") :
									 v.state == CardView::Closed ? v.why :
																   std::string("No saves on this card yet. Square brings saves in.");
			text(dl, regular(), 24, px + 40, sy + 24, alpha(col::dim, t), says.c_str(), pw - 80);
		}
		else
		{
			const std::vector<CardSave> saves = sortedSaves(v);
			const float content2 = SAVE_H * static_cast<float>(saves.size());
			float wanted2 = m.saveScroll;
			const float top = SAVE_H * static_cast<float>(m.save);
			if (top - wanted2 < 0)
				wanted2 = top;
			if (top + SAVE_H - wanted2 > listH)
				wanted2 = top + SAVE_H - listH;
			wanted2 = std::clamp(wanted2, 0.f, std::max(0.f, content2 - listH));
			m.saveScroll = approach(m.saveScroll, wanted2, 16);
			dl->PushClipRect(V(px, sy), V(px + pw, sy + listH), true);
			float yy = sy - m.saveScroll;
			for (size_t i = 0; i < saves.size(); i++, yy += SAVE_H)
			{
				if (yy + SAVE_H < sy || yy > sy + listH)
					continue;
				const CardSave& save = saves[i];
				const bool on = m.inSaves && static_cast<int>(i) == m.save;
				if (on)
					rect(dl, px + 16, yy + 3, pw - 32, SAVE_H - 6, alpha(col::accent, t * 0.92f), 14 * col::round);
				const ImU32 ink = alpha(on ? col::onAccent : col::text, t);
				const ImU32 dim = alpha(on ? col::onAccent : col::dim, t * (on ? 0.8f : 1.f));
				const std::string when = save.when.empty() ? std::string() : save.when;
				const std::string size = sizeText(save.bytes);
				const float rightW = std::max(textSize(regular(), 20, when.c_str()).x, textSize(regular(), 20, size.c_str()).x);
				textIn(dl, bold(), 24, px + 40, yy + 12, ink, saveTitle(save), pw - 120 - rightW, on);
				textIn(dl, regular(), 18, px + 40, yy + 44, dim, save.name, pw - 120 - rightW, false);
				const float rx = px + pw - 40;
				text(dl, regular(), 20, rx - textSize(regular(), 20, size.c_str()).x, yy + 14, ink, size.c_str());
				text(dl, regular(), 20, rx - textSize(regular(), 20, when.c_str()).x, yy + 42, dim, when.c_str());
			}
			dl->PopClipRect();
		}
	}

	// What the buttons do here.
	std::vector<Hint> hints;
	if (m.inSaves)
	{
		hints.push_back({Glyph::DPadUD, "Save"});
		hints.push_back({Glyph::Cross, "What to do with it"});
		hints.push_back({Glyph::Triangle, "Card"});
		hints.push_back({Glyph::Circle, "Cards"});
	}
	else
	{
		hints.push_back({Glyph::DPadUD, "Card"});
		if (row != nullptr && row->kind == ManagerRow::Card)
		{
			hints.push_back({Glyph::Cross, "Saves"});
			hints.push_back({Glyph::Square, "Bring saves in"});
			hints.push_back({Glyph::Triangle, "Card"});
		}
		else
		{
			hints.push_back({Glyph::Cross, "Choose"});
		}
		hints.push_back({Glyph::Circle, "Close"});
	}
	hintBar(dl, hints);
}
} // namespace bp
