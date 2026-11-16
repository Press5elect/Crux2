// Crux2 Big Picture - the HD texture pack manager.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// One sheet over the screen that opened it, in two panes, as the card manager (bp_cards.cpp):
//
//   left   Look again; the game it was opened for (THIS GAME); the games with a pack on any place
//          (INSTALLED: the cover and the title from the library, the serial when the library does
//          not have the game; the size of all its copies, where they are); the library's games
//          the collection has a pack for that none is installed of (IN THE COLLECTION); the
//          move or delete and the download that run, and those waiting (UNDER WAY); the places with what is free
//          (PLACES);
//   right  the row in focus: a game's copies (where, size, files, whether the game reads it) and
//          the collection's packs for it; a place's packs; what a download or an action is.
//
// Up / Down move, Right or Cross go into the right pane, Left or Circle come back, Cross on a pack
// opens what can be done with it (move to another place, download again, the game's texture
// settings, delete), Triangle opens the game's texture settings, Square looks again, Circle on
// the left closes the manager. Nothing here touches a file: the shelf (bp.h) does, on threads of
// its own, and says what came of it; the lists are made again whenever its packsGeneration moves.

#include "bp_internal.h"

#include <map>
#include <set>

namespace bp
{
namespace
{
struct PackRow
{
	enum Kind { Heading, Action, Note, Game, Place, Download, Job } kind = Game;
	std::string label, sub;
	std::string serial;        // a game's
	bool inLibrary = false;
	GameInfo game;             // the library's game, for its cover
	int place = -1;            // a place's index
	int queued = -2;           // a download: -1 the one that runs, else its place in the queue
	std::function<void()> act; // an action's
	std::string key() const
	{
		switch (kind)
		{
			case Game: return "g:" + serial;
			case Place: return "p:" + std::to_string(place);
			case Download: return "d:" + std::to_string(queued);
			case Job: return "j";
			default: return "a:" + label;
		}
	}
};

struct PackItem
{
	bool offered = false;
	InstalledPack pack;
	OfferedPack offer;
	std::string label, sub;
	bool warn = false; // drawn in the warm colour (unfinished, not read)
};

struct Manager
{
	bool open = false;
	TexturePackShelf* shelf = nullptr;
	std::string serial;   // the game it was opened for
	unsigned generation = ~0u, games = ~0u;
	bool downloading = false, jobbing = false;
	std::vector<PackRow> rows;
	std::vector<PackPlace> places;
	std::vector<InstalledPack> installed;
	std::vector<OfferedPack> offered;
	std::map<std::string, GameInfo> library; // by serial (PlayStation 2 games)
	int focus = 0, item = 0;
	bool inItems = false;
	float scroll = 0, itemScroll = 0, shown = 0;
	std::string keep;     // the row focused before the lists were made again
} s_m;

constexpr float MGR_W = 1760, MGR_H = 852, MGR_X = (1920 - MGR_W) / 2, MGR_Y = 118;
constexpr float LEFT_W = 720, ROW_H = 96, HEAD_H = 50, ITEM_H = 78;
constexpr const char* DOT = "   \xc2\xb7   ";

std::string count(uint64_t n, const char* one, const char* many)
{
	// 12,345 files
	std::string digits = std::to_string(n), out;
	for (size_t i = 0; i < digits.size(); i++)
	{
		if (i > 0 && (digits.size() - i) % 3 == 0)
			out.push_back(',');
		out.push_back(digits[i]);
	}
	return out + " " + (n == 1 ? one : many);
}

std::string placeName(int place)
{
	return (place >= 0 && place < static_cast<int>(s_m.places.size())) ? s_m.places[static_cast<size_t>(place)].name : std::string("?");
}

std::string freeText(const PackPlace& p)
{
	return p.freeBytes >= 0 ? formatSize(static_cast<uint64_t>(p.freeBytes)) + " free" : std::string("free space not known");
}

const char* placeIcon(const PackPlace& p)
{
	return p.kind == 0 ? ICON_FA_HOUSE : p.kind == 1 ? ICON_FA_HARD_DRIVE : ICON_FA_NETWORK_WIRED;
}

// The packs of the collection for a serial.
std::vector<const OfferedPack*> offersFor(const std::string& serial)
{
	std::vector<const OfferedPack*> out;
	for (const OfferedPack& o : s_m.offered)
	{
		if (std::find(o.serials.begin(), o.serials.end(), serial) != o.serials.end())
			out.push_back(&o);
	}
	return out;
}

// What a game is called here: the library's title, else the collection's pack's, else the serial.
std::string titleOf(const std::string& serial)
{
	if (auto it = s_m.library.find(serial); it != s_m.library.end())
		return it->second.title;
	const std::vector<const OfferedPack*> offers = offersFor(serial);
	if (!offers.empty())
		return offers.front()->title;
	return serial;
}

std::string sizeLine(const InstalledPack& p)
{
	if (!p.measured)
		return "measuring...";
	return formatSize(p.bytes) + DOT + (p.zip ? std::string("one ZIP file") : count(p.files, "file", "files"));
}

// Where the copy the game reads is, for one that is not read.
std::string readLine(const InstalledPack& p)
{
	if (p.state == InstalledPack::Moving)
		return "an unfinished move: move the pack here again to go on, or delete what was copied";
	if (p.state == InstalledPack::Deleting)
		return "what a stopped delete left (not read): Cross finishes it";
	if (p.read)
		return "the game reads this one";
	for (const InstalledPack& other : s_m.installed)
	{
		if (other.serial == p.serial && other.zip == p.zip && other.read)
			return "not read: the copy on " + placeName(other.place) + " comes first";
	}
	return "not read";
}

void addHeading(const char* label)
{
	PackRow h;
	h.kind = PackRow::Heading;
	h.label = label;
	s_m.rows.push_back(std::move(h));
}

PackRow gameRow(const std::string& serial)
{
	PackRow r;
	r.kind = PackRow::Game;
	r.serial = serial;
	r.label = titleOf(serial);
	if (auto it = s_m.library.find(serial); it != s_m.library.end())
	{
		r.inLibrary = true;
		r.game = it->second;
	}
	uint64_t bytes = 0;
	int copies = 0, measuring = 0, unfinished = 0;
	std::vector<std::string> where;
	for (const InstalledPack& p : s_m.installed)
	{
		if (p.serial != serial)
			continue;
		if (p.state != InstalledPack::Whole)
		{
			unfinished++;
			continue;
		}
		copies++;
		bytes += p.bytes;
		measuring += p.measured ? 0 : 1;
		const std::string name = placeName(p.place);
		if (std::find(where.begin(), where.end(), name) == where.end())
			where.push_back(name);
	}
	if (copies == 0 && unfinished == 0)
	{
		const std::vector<const OfferedPack*> offers = offersFor(serial);
		uint64_t size = 0;
		for (const OfferedPack* o : offers)
			size += o->size;
		r.sub = serial + DOT +
				(offers.empty() ? std::string(s_m.shelf->collectionState() == 2 ? "no pack installed, none in the collection" : "no pack installed") :
								  count(offers.size(), "pack", "packs") + " in the collection, " + formatSize(size));
	}
	else
	{
		std::string places;
		for (const std::string& w : where)
			places += (places.empty() ? "" : ", ") + w;
		r.sub = serial + DOT + (measuring > 0 ? std::string("measuring...") : formatSize(bytes)) + DOT +
				(copies <= 1 ? std::string() : std::to_string(copies) + " copies on ") + places;
		if (unfinished > 0)
			r.sub += DOT + std::string("an unfinished move or delete");
	}
	if (!r.inLibrary)
		r.sub += DOT + std::string("not in the library");
	return r;
}

bool byLabel(const PackRow& a, const PackRow& b)
{
	return std::lexicographical_compare(a.label.begin(), a.label.end(), b.label.begin(), b.label.end(),
		[](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
}

void rebuild()
{
	Manager& m = s_m;
	TexturePackShelf* shelf = m.shelf;
	m.generation = shelf->packsGeneration();
	m.games = be->gamesGeneration();
	m.downloading = !be->texturePackDoing().empty();
	m.jobbing = !shelf->packJob().empty();
	if (m.focus >= 0 && m.focus < static_cast<int>(m.rows.size()))
		m.keep = m.rows[static_cast<size_t>(m.focus)].key();
	m.rows.clear();
	m.places = shelf->packPlaces();
	m.installed = shelf->installedPacks();
	m.offered = shelf->collectionPacks();
	m.library.clear();
	for (const GameInfo& g : be->games())
	{
		if (!g.serial.empty() && g.kind == GameInfo::PS2 && m.library.count(g.serial) == 0)
			m.library[g.serial] = g;
	}

	{
		PackRow a;
		a.kind = PackRow::Action;
		a.label = ICON_FA_ROTATE "   Look again";
		a.sub = "Looks into every place and measures every pack anew, and fetches the collection's list again.";
		a.act = [] {
			s_m.shelf->lookAgain(true);
			toast("Looking at every place again...", 3.f);
		};
		m.rows.push_back(std::move(a));
	}

	// The game it was opened for.
	if (!m.serial.empty())
	{
		addHeading("THIS GAME");
		m.rows.push_back(gameRow(m.serial));
	}

	// The games with a pack somewhere.
	std::set<std::string> withPacks;
	for (const InstalledPack& p : m.installed)
		withPacks.insert(p.serial);
	{
		std::vector<PackRow> games;
		for (const std::string& serial : withPacks)
		{
			if (serial != m.serial)
				games.push_back(gameRow(serial));
		}
		std::sort(games.begin(), games.end(), byLabel);
		addHeading("INSTALLED");
		if (games.empty() && (m.serial.empty() || withPacks.count(m.serial) == 0))
		{
			PackRow n;
			n.kind = PackRow::Note;
			n.label = "No texture pack yet";
			n.sub = "None on this console, the USB drives or the shares. The collection's packs for your games are below.";
			m.rows.push_back(std::move(n));
		}
		for (PackRow& r : games)
			m.rows.push_back(std::move(r));
	}

	// The library's games the collection has a pack for, none installed.
	addHeading("IN THE COLLECTION");
	const int state = shelf->collectionState();
	if (state == 1 || state == 0)
	{
		PackRow n;
		n.kind = PackRow::Note;
		n.label = ICON_FA_HOURGLASS_HALF "   Asking the Internet Archive...";
		n.sub = "For the list of the packs its collection has (kept for a day).";
		m.rows.push_back(std::move(n));
	}
	else if (state == 3)
	{
		PackRow a;
		a.kind = PackRow::Action;
		a.label = ICON_FA_TRIANGLE_EXCLAMATION "   The collection's list could not be had";
		a.sub = shelf->collectionError() + ". Cross asks again.";
		a.act = [] { s_m.shelf->lookAgain(true); };
		m.rows.push_back(std::move(a));
	}
	else
	{
		std::vector<PackRow> games;
		for (const auto& [serial, game] : m.library)
		{
			if (serial == m.serial || withPacks.count(serial) != 0 || offersFor(serial).empty())
				continue;
			games.push_back(gameRow(serial));
		}
		std::sort(games.begin(), games.end(), byLabel);
		if (games.empty())
		{
			PackRow n;
			n.kind = PackRow::Note;
			n.label = "Nothing more for your games";
			n.sub = "The collection has no pack for a game of your library that is not installed already.";
			m.rows.push_back(std::move(n));
		}
		for (PackRow& r : games)
			m.rows.push_back(std::move(r));
	}

	// The downloads.
	const std::vector<std::string> queue = shelf->queuedDownloads();
	if (m.downloading || !queue.empty() || m.jobbing)
	{
		addHeading("UNDER WAY");
		if (m.jobbing)
		{
			PackRow j;
			j.kind = PackRow::Job;
			j.label = ICON_FA_TRUCK "   Moving or deleting";
			m.rows.push_back(std::move(j));
		}
		if (m.downloading)
		{
			PackRow d;
			d.kind = PackRow::Download;
			d.queued = -1;
			d.label = ICON_FA_DOWNLOAD "   Downloading now";
			m.rows.push_back(std::move(d));
		}
		for (size_t i = 0; i < queue.size(); i++)
		{
			PackRow d;
			d.kind = PackRow::Download;
			d.queued = static_cast<int>(i);
			d.label = ICON_FA_CLOCK "   " + queue[i];
			d.sub = i == 0 ? std::string("Next, when the one that runs is done") : "Waiting: " + std::to_string(i + 1) + " in the queue";
			m.rows.push_back(std::move(d));
		}
	}

	// The places.
	addHeading("PLACES");
	for (size_t i = 0; i < m.places.size(); i++)
	{
		const PackPlace& p = m.places[i];
		PackRow r;
		r.kind = PackRow::Place;
		r.place = static_cast<int>(i);
		r.label = std::string(placeIcon(p)) + "   " + p.name;
		int packs = 0;
		uint64_t bytes = 0;
		for (const InstalledPack& pack : m.installed)
		{
			if (pack.place == r.place)
			{
				packs++;
				bytes += pack.bytes;
			}
		}
		r.sub = freeText(p) + DOT + (packs == 0 ? std::string("no pack") : count(static_cast<uint64_t>(packs), "pack", "packs") + ", " + formatSize(bytes)) +
				(p.writable ? std::string() : DOT + std::string("read only"));
		m.rows.push_back(std::move(r));
	}

	// The focus stays on the row it was on, when it is still there.
	int found = -1;
	for (size_t i = 0; i < m.rows.size() && found < 0; i++)
	{
		if (m.rows[i].kind != PackRow::Heading && !m.keep.empty() && m.rows[i].key() == m.keep)
			found = static_cast<int>(i);
	}
	if (found < 0)
	{
		found = std::clamp(m.focus, 0, std::max(0, static_cast<int>(m.rows.size()) - 1));
		while (found < static_cast<int>(m.rows.size()) - 1 && m.rows[static_cast<size_t>(found)].kind == PackRow::Heading)
			found++;
	}
	m.focus = found;
	m.keep.clear();
}

PackRow* focused()
{
	return (s_m.focus >= 0 && s_m.focus < static_cast<int>(s_m.rows.size())) ? &s_m.rows[static_cast<size_t>(s_m.focus)] : nullptr;
}

// The right pane's packs for a row.
std::vector<PackItem> itemsFor(const PackRow* row)
{
	std::vector<PackItem> items;
	if (row == nullptr)
		return items;
	if (row->kind == PackRow::Game)
	{
		for (const InstalledPack& p : s_m.installed)
		{
			if (p.serial != row->serial)
				continue;
			PackItem it;
			it.pack = p;
			it.label = std::string(p.zip ? ICON_FA_BOX_ARCHIVE "   " : ICON_FA_FOLDER_OPEN "   ") + "On " + placeName(p.place) + (p.zip ? " (ZIP)" : "");
			it.sub = (p.state == InstalledPack::Whole ? sizeLine(p) + DOT : std::string()) + readLine(p);
			it.warn = p.state != InstalledPack::Whole || !p.read;
			items.push_back(std::move(it));
		}
		for (const OfferedPack* o : offersFor(row->serial))
		{
			PackItem it;
			it.offered = true;
			it.offer = *o;
			it.label = ICON_FA_CLOUD_ARROW_DOWN "   " + o->title;
			it.sub = formatSize(o->size) + " to download from the Internet Archive" +
					 (items.empty() ? std::string() : DOT + std::string("installed already: downloading again brings it anew"));
			items.push_back(std::move(it));
		}
	}
	else if (row->kind == PackRow::Place)
	{
		for (const InstalledPack& p : s_m.installed)
		{
			if (p.place != row->place)
				continue;
			PackItem it;
			it.pack = p;
			it.label = titleOf(p.serial) + (p.zip ? " (ZIP)" : "");
			it.sub = p.serial + DOT + (p.state == InstalledPack::Whole ? sizeLine(p) + DOT : std::string()) + readLine(p);
			it.warn = p.state != InstalledPack::Whole || !p.read;
			items.push_back(std::move(it));
		}
	}
	return items;
}

// Whether `need` bytes go into a place; when it is known they do not, asked first.
void ifRoom(int place, uint64_t need, const char* verb, std::function<void()> then)
{
	const PackPlace& p = s_m.places[static_cast<size_t>(place)];
	const uint64_t margin = need / 100 + (64ull << 20);
	if (p.freeBytes < 0 || need + margin <= static_cast<uint64_t>(p.freeBytes))
	{
		then();
		return;
	}
	confirm("It may not fit",
		"This needs about " + formatSize(need) + " and " + p.name + " has " + formatSize(static_cast<uint64_t>(p.freeBytes)) +
			" free. What does not fit stops it when the place is full (a move leaves the pack where it is).",
		[then](bool yes) {
			if (yes)
				then();
		},
		verb, "No");
}

std::string roomNote(int place, uint64_t need)
{
	const PackPlace& p = s_m.places[static_cast<size_t>(place)];
	if (p.freeBytes < 0)
		return "";
	const uint64_t margin = need / 100 + (64ull << 20);
	return "   (" + freeText(p) + (need + margin > static_cast<uint64_t>(p.freeBytes) ? ": may not fit)" : ")");
}

void openGameTextures(const std::string& serial)
{
	auto it = s_m.library.find(serial);
	if (it == s_m.library.end())
	{
		toast("This game is not in the library: its settings are opened from there.", 4.f);
		return;
	}
	const GameInfo game = it->second;
	const bool there = (screen == Screen::Settings && settingsGame().serial == serial && !settingsGame().path.empty());
	closeTexturePacks();
	if (!there)
		openSettings(&game, be->running().valid ? Screen::Quick : Screen::Library);
	if (screen == Screen::Settings)
		settingsFocus("Graphics", "Load textures");
}

void downloadMenu(const OfferedPack& offer, int fromPlace)
{
	std::vector<int> targets;
	std::vector<std::string> names;
	const uint64_t need = offer.size + offer.size / 20;
	for (size_t i = 0; i < s_m.places.size(); i++)
	{
		const PackPlace& p = s_m.places[i];
		if (!p.downloads || !p.writable)
			continue;
		targets.push_back(static_cast<int>(i));
		names.push_back(std::string(placeIcon(p)) + "   " + p.name + roomNote(static_cast<int>(i), need));
	}
	if (targets.empty())
	{
		message("Nowhere to download to", "No place can take a download now.");
		return;
	}
	int current = 0;
	for (size_t i = 0; i < targets.size(); i++)
	{
		if (targets[i] == fromPlace)
			current = static_cast<int>(i);
	}
	pick("Download " + offer.title + " (" + formatSize(offer.size) + ") to", names, current, [targets, offer, need](int choice) {
		if (choice < 0 || choice >= static_cast<int>(targets.size()) || s_m.shelf == nullptr)
			return;
		const int place = targets[static_cast<size_t>(choice)];
		ifRoom(place, need, "Download anyway", [offer, place] {
			if (s_m.shelf != nullptr)
				s_m.shelf->queueDownload(offer, place);
		});
	});
}

void deleteAsked(const InstalledPack& pack)
{
	const std::string what = titleOf(pack.serial) + " on " + placeName(pack.place) +
							 (pack.measured ? " (" + formatSize(pack.bytes) + ", " + count(pack.files, "file", "files") + ")" : std::string());
	confirm("Delete this texture pack?", what + ". The game shows its own textures again unless another copy is read.",
		[pack, what](bool yes) {
			if (!yes)
				return;
			confirm("Really delete it?", what + " is deleted for good: it cannot be undone.", [pack](bool sure) {
				if (sure && s_m.shelf != nullptr)
					s_m.shelf->deletePack(pack);
			}, "Yes, delete it", "No");
		},
		"Delete", "No");
}

void packMenu(const PackItem& item)
{
	TexturePackShelf* shelf = s_m.shelf;
	if (item.offered)
	{
		downloadMenu(item.offer, 0);
		return;
	}
	const InstalledPack pack = item.pack;
	const std::string busy = shelf->packBusy(pack);
	std::vector<std::string> names;
	std::vector<std::function<void()>> acts;
	if (pack.state == InstalledPack::Moving)
	{
		// The source the move was from: the whole copy of the same pack on another place.
		for (const InstalledPack& other : s_m.installed)
		{
			if (other.serial == pack.serial && other.state == InstalledPack::Whole && !other.zip && other.place != pack.place)
			{
				// (Not when a whole copy is where the move was going: that one is read; delete this.)
				bool whole = false;
				for (const InstalledPack& there : s_m.installed)
					whole |= there.serial == pack.serial && !there.zip && there.place == pack.place && there.state == InstalledPack::Whole;
				if (whole)
					break;
				names.push_back(ICON_FA_TRUCK "   Go on with the move from " + placeName(other.place));
				acts.push_back([other, place = pack.place] {
					if (s_m.shelf != nullptr)
						s_m.shelf->movePack(other, place);
				});
				break;
			}
		}
		names.push_back(ICON_FA_TRASH "   Delete what was copied");
		acts.push_back([pack] {
			confirm("Delete the unfinished copy?", "What an unfinished move copied to " + placeName(pack.place) + " is deleted. The pack it came from stays.",
				[pack](bool yes) {
					if (yes && s_m.shelf != nullptr)
						s_m.shelf->deletePack(pack);
				}, "Delete", "No");
		});
	}
	else if (pack.state == InstalledPack::Deleting)
	{
		names.push_back(ICON_FA_TRASH "   Finish deleting");
		acts.push_back([pack] {
			if (s_m.shelf != nullptr)
				s_m.shelf->deletePack(pack);
		});
	}
	else
	{
		for (size_t i = 0; i < s_m.places.size(); i++)
		{
			const PackPlace& p = s_m.places[i];
			if (static_cast<int>(i) == pack.place || !p.writable)
				continue;
			// (Not where a whole copy of it is already: the move would be refused.)
			bool there = false;
			for (const InstalledPack& other : s_m.installed)
				there |= other.serial == pack.serial && other.zip == pack.zip && other.place == static_cast<int>(i) && other.state == InstalledPack::Whole;
			if (there)
				continue;
			const int place = static_cast<int>(i);
			names.push_back(std::string(ICON_FA_TRUCK "   Move to ") + p.name + roomNote(place, pack.bytes));
			acts.push_back([pack, place] {
				ifRoom(place, pack.bytes, "Move anyway", [pack, place] {
					if (s_m.shelf != nullptr)
						s_m.shelf->movePack(pack, place);
				});
			});
		}
		const std::vector<const OfferedPack*> offers = offersFor(pack.serial);
		if (!offers.empty() && pack.place >= 0 && pack.place < static_cast<int>(s_m.places.size()) && s_m.places[static_cast<size_t>(pack.place)].downloads)
		{
			const OfferedPack offer = *offers.front();
			names.push_back(ICON_FA_CLOUD_ARROW_DOWN "   Download again from the collection");
			acts.push_back([offer, place = pack.place] {
				if (s_m.shelf != nullptr)
					s_m.shelf->queueDownload(offer, place);
			});
		}
		if (s_m.library.count(pack.serial) != 0)
		{
			names.push_back(ICON_FA_SLIDERS "   Texture settings of the game");
			acts.push_back([serial = pack.serial] { openGameTextures(serial); });
		}
		names.push_back(ICON_FA_TRASH "   Delete");
		acts.push_back([pack] { deleteAsked(pack); });
	}
	const std::string title = titleOf(pack.serial) + " on " + placeName(pack.place);
	pick(title, names, -1, [acts, busy](int choice) {
		if (choice < 0 || choice >= static_cast<int>(acts.size()))
			return;
		if (!busy.empty())
		{
			message("Not now", busy);
			return;
		}
		acts[static_cast<size_t>(choice)]();
	});
}

void downloadRowMenu(const PackRow& row)
{
	if (row.queued == -1)
	{
		confirm("Stop the download?", "What was unpacked whole stays; the next in the queue starts.", [](bool yes) {
			if (yes)
				be->stopTexturePack();
		}, "Stop it", "No");
		return;
	}
	const int index = row.queued;
	pick(row.label, {ICON_FA_XMARK "   Take it off the queue"}, -1, [index](int choice) {
		if (choice == 0 && s_m.shelf != nullptr)
			s_m.shelf->unqueueDownload(index);
	});
}

void moveFocus(int step)
{
	Manager& m = s_m;
	const int n = static_cast<int>(m.rows.size());
	int at = m.focus;
	for (int tries = 0; tries < n; tries++)
	{
		at += step;
		if (at < 0 || at >= n)
			return;
		if (m.rows[static_cast<size_t>(at)].kind != PackRow::Heading)
		{
			m.focus = at;
			m.item = 0;
			m.itemScroll = 0;
			be->cue(CueFocus);
			return;
		}
	}
}

bool stale()
{
	return s_m.generation != s_m.shelf->packsGeneration() || s_m.games != be->gamesGeneration() ||
		   s_m.downloading != !be->texturePackDoing().empty() || s_m.jobbing != !s_m.shelf->packJob().empty();
}
} // namespace

bool texturePacksOpen()
{
	return s_m.open;
}

void openTexturePacks(TexturePackShelf* shelf, const std::string& serial)
{
	if (shelf == nullptr)
		return;
	Manager& m = s_m;
	m.open = true;
	m.shelf = shelf;
	m.serial = serial;
	m.focus = 0;
	m.item = 0;
	m.inItems = false;
	m.scroll = m.itemScroll = 0;
	m.shown = 0;
	m.generation = ~0u;
	m.rows.clear();
	m.keep = serial.empty() ? std::string() : "g:" + serial;
	// The places are looked into again (a drive may have come); only packs not measured before
	// are measured, and the collection's list is fetched when it was not yet.
	shelf->lookAgain(false);
	if (be != nullptr)
		be->cue(CueModalOpen);
	holdInput();
}

bool texturePacksFocus(const std::string& serial)
{
	if (!s_m.open || s_m.shelf == nullptr)
		return false;
	if (stale())
		rebuild();
	for (size_t i = 0; i < s_m.rows.size(); i++)
	{
		if (s_m.rows[i].kind == PackRow::Game && s_m.rows[i].serial == serial)
		{
			s_m.focus = static_cast<int>(i);
			s_m.inItems = false;
			s_m.item = 0;
			return true;
		}
	}
	return false;
}

void closeTexturePacks()
{
	if (!s_m.open)
		return;
	s_m.open = false;
	s_m.rows.clear();
	if (be != nullptr)
		be->cue(CueModalClose);
}

void texturePacksInput(const Input& now)
{
	Manager& m = s_m;
	if (!m.open || m.shelf == nullptr)
		return;
	if (stale())
		rebuild();
	PackRow* row = focused();
	const std::vector<PackItem> items = itemsFor(row);
	if (now.square)
	{
		m.shelf->lookAgain(true);
		toast("Looking at every place again...", 3.f);
		return;
	}
	if (now.triangle && row != nullptr && row->kind == PackRow::Game)
	{
		openGameTextures(row->serial);
		return;
	}
	if (m.inItems)
	{
		const int n = static_cast<int>(items.size());
		if (n == 0 || now.left || now.back)
		{
			m.inItems = false;
			be->cue(CueBack);
			return;
		}
		m.item = std::clamp(m.item, 0, n - 1);
		if (now.up && m.item > 0)
		{
			m.item--;
			be->cue(CueFocus);
		}
		if (now.down && m.item < n - 1)
		{
			m.item++;
			be->cue(CueFocus);
		}
		if (now.accept)
		{
			be->cue(CueSelect);
			packMenu(items[static_cast<size_t>(m.item)]);
		}
		return;
	}

	if (now.back)
	{
		closeTexturePacks();
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
		while (m.focus > 0 && m.rows[static_cast<size_t>(m.focus)].kind == PackRow::Heading)
			m.focus--;
	}
	row = focused();
	if (row == nullptr)
		return;
	if (now.accept && row->kind == PackRow::Action)
	{
		be->cue(CueSelect);
		const std::function<void()> act = row->act;
		if (act)
			act();
		return;
	}
	if (now.accept && row->kind == PackRow::Job)
	{
		be->cue(CueSelect);
		confirm("Stop it?",
			"A move stopped leaves the pack where it was (moving it again goes on from what was copied); a delete stopped leaves the "
			"rest unread, and deleting it again finishes it.",
			[](bool yes) {
				if (yes && s_m.shelf != nullptr)
					s_m.shelf->stopPackJob();
			},
			"Stop it", "No");
		return;
	}
	if (now.accept && row->kind == PackRow::Download)
	{
		be->cue(CueSelect);
		downloadRowMenu(*row);
		return;
	}
	if ((now.accept || now.right) && (row->kind == PackRow::Game || row->kind == PackRow::Place))
	{
		const std::vector<PackItem> its = itemsFor(row);
		if (!its.empty())
		{
			m.inItems = true;
			m.item = 0;
			m.itemScroll = 0;
			be->cue(CueOpen);
		}
		else if (now.accept && row->kind == PackRow::Game)
		{
			toast(m.shelf->collectionState() == 2 ? "The collection has no pack for " + row->serial + "." :
													"The collection's list is not here yet: Look again asks for it.", 4.f);
		}
	}
}

void texturePacksDraw(ImDrawList* dl)
{
	Manager& m = s_m;
	if (!m.open || m.shelf == nullptr)
		return;
	if (stale())
		rebuild();
	m.shown = approach(m.shown, 1, 14);
	const float t = m.shown;
	dl->AddRectFilled(V(0, 0), V(W, H), col::rgba(0, 0, 0, static_cast<int>(180 * t)));
	const float x = MGR_X, y = MGR_Y + (1 - t) * 24, w = MGR_W, h = MGR_H;
	glow(dl, x, y, w, h, 26, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
	rect(dl, x, y, w, h, alpha(col::panel, t), 26);

	// The title, and what goes on.
	text(dl, bold(), 40, x + 48, y + 34, alpha(col::text, t), ICON_FA_IMAGES "   HD texture packs");
	{
		std::string doing = m.shelf->packJob();
		const int measuring = m.shelf->measuring();
		if (doing.empty() && measuring > 0)
			doing = "Measuring " + count(static_cast<uint64_t>(measuring), "pack", "packs") + "...";
		if (doing.empty())
		{
			uint64_t bytes = 0;
			for (const InstalledPack& p : m.installed)
				bytes += p.bytes;
			doing = count(m.installed.size(), "pack", "packs") + ", " + formatSize(bytes);
		}
		const float sw = std::min(textSize(regular(), 24, doing.c_str()).x, w - 640);
		textIn(dl, regular(), 24, x + w - 48 - sw, y + 46, alpha(col::dim, t), doing, sw, false);
	}
	rect(dl, x + 32, y + 104, w - 64, 1, alpha(col::line, t));

	// ---- the left pane
	const float lx = x + 24, ly = y + 118, lw = LEFT_W, lh = h - 118 - 24;
	float content = 0, focusTop = 0, focusH = ROW_H;
	for (size_t i = 0; i < m.rows.size(); i++)
	{
		const float rh = (m.rows[i].kind == PackRow::Heading) ? HEAD_H : ROW_H;
		if (static_cast<int>(i) == m.focus)
		{
			focusTop = content;
			focusH = rh;
		}
		content += rh;
	}
	float wanted = m.scroll;
	if (focusTop - wanted < lh * 0.25f)
		wanted = focusTop - lh * 0.25f;
	if (focusTop + focusH - wanted > lh * 0.75f)
		wanted = focusTop + focusH - lh * 0.75f;
	wanted = std::clamp(wanted, 0.f, std::max(0.f, content - lh));
	m.scroll = approach(m.scroll, wanted, 16);

	const std::string downloadNow = be->texturePackDoing();
	dl->PushClipRect(V(lx, ly), V(lx + lw, ly + lh), true);
	float ry = ly - m.scroll;
	for (size_t i = 0; i < m.rows.size(); i++)
	{
		const PackRow& r = m.rows[i];
		const float rh = (r.kind == PackRow::Heading) ? HEAD_H : ROW_H;
		if (ry + rh < ly || ry > ly + lh)
		{
			ry += rh;
			continue;
		}
		if (r.kind == PackRow::Heading)
		{
			text(dl, bold(), 20, lx + 24, ry + 18, alpha(col::accent, t), r.label.c_str());
			const ImVec2 ts = textSize(bold(), 20, r.label.c_str());
			rect(dl, lx + 24 + ts.x + 16, ry + 30, lw - 48 - ts.x - 16, 1, alpha(col::line, t));
			ry += rh;
			continue;
		}
		const bool on = static_cast<int>(i) == m.focus;
		if (on)
			rect(dl, lx + 8, ry + 4, lw - 16, rh - 8, alpha(m.inItems ? col::panelHi : col::accent, t * (m.inItems ? 1.f : 0.92f)), 16);
		const ImU32 ink = alpha(on && !m.inItems ? col::onAccent : col::text, t);
		const ImU32 faint = alpha(on && !m.inItems ? col::onAccent : col::dim, t * (on && !m.inItems ? 0.8f : 1.f));
		float tx = lx + 28;
		if (r.kind == PackRow::Game)
		{
			const float cw = 56, ch = 78, cx = tx, cy = ry + (rh - ch) / 2;
			const Texture cover = r.inLibrary ? be->cover(r.game) : Texture{};
			if (cover.valid())
			{
				imageFit(dl, cover, cx, cy, cw, ch, 6, alpha(col::white, t));
			}
			else
			{
				rect(dl, cx, cy, cw, ch, alpha(col::sheet, t), 8);
				const char* icon = ICON_FA_IMAGES;
				const ImVec2 is = textSize(regular(), 26, icon);
				text(dl, regular(), 26, cx + (cw - is.x) / 2, cy + (ch - is.y) / 2, alpha(col::dim, t), icon);
			}
			tx += cw + 22;
		}
		const std::string sub = (r.kind == PackRow::Download && r.queued == -1) ? downloadNow : (r.kind == PackRow::Job) ? m.shelf->packJob() : r.sub;
		textIn(dl, bold(), 26, tx, ry + 16, ink, r.label, lx + lw - 28 - tx, on);
		textIn(dl, regular(), 20, tx, ry + 54, faint, sub, lx + lw - 28 - tx, false);
		ry += rh;
	}
	dl->PopClipRect();

	// ---- the right pane
	const float px = x + 24 + LEFT_W + 24, py = y + 118, pw = w - (px - x) - 32, ph = h - 118 - 24;
	rect(dl, px, py, pw, ph, alpha(col::sheet, t), 20);
	const PackRow* row = focused();
	const std::vector<PackItem> items = itemsFor(row);
	float sy = py + 30;
	if (row == nullptr)
	{
		// (Nothing: the lists are being made.)
	}
	else if (row->kind == PackRow::Game)
	{
		if (row->inLibrary)
			drawGameCover(dl, row->game, px + 40, sy, 100, 140, 8, 0);
		const float hx = px + 40 + (row->inLibrary ? 124 : 0);
		textIn(dl, bold(), 34, hx, sy, alpha(col::text, t), row->label, px + pw - 40 - hx, !m.inItems);
		textIn(dl, regular(), 22, hx, sy + 50, alpha(col::dim, t),
			row->serial + DOT + (row->inLibrary ? std::string("in your library") : std::string("not in your library")), px + pw - 40 - hx, false);
		std::string hint;
		if (items.empty())
			hint = m.shelf->collectionState() == 2 ? "No pack is installed, and the collection has none for this game." :
				   m.shelf->collectionState() == 3 ? "No pack is installed. The collection's list could not be had: Look again asks for it." :
													  "No pack is installed. The collection's list is being fetched...";
		else
			hint = "Cross on a pack: move it, download it again, delete it." + std::string(row->inLibrary ? " Triangle: the game's texture settings." : "");
		text(dl, regular(), 20, hx, sy + 86, alpha(col::dim, t), hint.c_str(), px + pw - 40 - hx);
		sy += row->inLibrary ? 160 : 130;
	}
	else if (row->kind == PackRow::Place)
	{
		const PackPlace& p = m.places[static_cast<size_t>(row->place)];
		textIn(dl, bold(), 34, px + 40, sy, alpha(col::text, t), row->label, pw - 80, !m.inItems);
		textIn(dl, regular(), 22, px + 40, sy + 50, alpha(col::dim, t), p.where, pw - 80, false);
		std::string line = freeText(p) + DOT +
						   (p.writable ? (p.downloads ? std::string("packs can be downloaded and moved here") : std::string("packs can be moved here (not downloaded)")) :
										 std::string("read only"));
		if (p.freeBytes < 0 && p.kind != 2)
			line += ". (The console tells a title nothing of the room on its storage or its drives: a pack too large stops when the place is full.)";
		text(dl, regular(), 20, px + 40, sy + 86, alpha(col::dim, t), line.c_str(), pw - 80);
		sy += 150;
	}
	else
	{
		const std::string says = (row->kind == PackRow::Job) ? m.shelf->packJob() + "\n\nCross stops it." :
								 (row->kind == PackRow::Download && row->queued == -1) ?
									 downloadNow + "\n\nCross stops it: what was unpacked whole stays, and the next in the queue starts." :
								 row->kind == PackRow::Download ? row->label.substr(std::strlen(ICON_FA_CLOCK "   ")) + "\n\n" + row->sub + ". Cross takes it off the queue." :
																  row->sub;
		text(dl, regular(), 26, px + 40, sy + 10, alpha(col::dim, t), says.c_str(), pw - 80);
	}
	if (!items.empty())
	{
		rect(dl, px + 24, sy, pw - 48, 1, alpha(col::line, t));
		sy += 12;
		const float listH = py + ph - sy - 12;
		const float all = ITEM_H * static_cast<float>(items.size());
		const int sel = std::clamp(m.item, 0, static_cast<int>(items.size()) - 1);
		float wanted2 = m.itemScroll;
		const float top = ITEM_H * static_cast<float>(sel);
		if (top - wanted2 < 0)
			wanted2 = top;
		if (top + ITEM_H - wanted2 > listH)
			wanted2 = top + ITEM_H - listH;
		wanted2 = std::clamp(wanted2, 0.f, std::max(0.f, all - listH));
		m.itemScroll = approach(m.itemScroll, wanted2, 16);
		dl->PushClipRect(V(px, sy), V(px + pw, sy + listH), true);
		float yy = sy - m.itemScroll;
		for (size_t i = 0; i < items.size(); i++, yy += ITEM_H)
		{
			if (yy + ITEM_H < sy || yy > sy + listH)
				continue;
			const PackItem& it = items[i];
			const bool on = m.inItems && static_cast<int>(i) == sel;
			if (on)
				rect(dl, px + 16, yy + 3, pw - 32, ITEM_H - 6, alpha(col::accent, t * 0.92f), 14);
			const ImU32 ink = alpha(on ? col::onAccent : col::text, t);
			const ImU32 dim = alpha(on ? col::onAccent : (it.warn ? col::warm : col::dim), t * (on ? 0.8f : 1.f));
			textIn(dl, bold(), 24, px + 40, yy + 12, ink, it.label, pw - 80, on);
			textIn(dl, regular(), 19, px + 40, yy + 46, dim, it.sub, pw - 80, on);
		}
		dl->PopClipRect();
	}

	// What the buttons do here.
	std::vector<Hint> hints;
	if (m.inItems)
	{
		hints.push_back({Glyph::DPadUD, "Pack"});
		hints.push_back({Glyph::Cross, "What to do with it"});
		hints.push_back({Glyph::Circle, "Back"});
	}
	else
	{
		hints.push_back({Glyph::DPadUD, "Move"});
		if (row != nullptr && (row->kind == PackRow::Game || row->kind == PackRow::Place))
			hints.push_back({Glyph::Cross, "Packs"});
		else if (row != nullptr && (row->kind == PackRow::Action || row->kind == PackRow::Download || row->kind == PackRow::Job))
			hints.push_back({Glyph::Cross, "Choose"});
		if (row != nullptr && row->kind == PackRow::Game && row->inLibrary)
			hints.push_back({Glyph::Triangle, "Texture settings"});
		hints.push_back({Glyph::Square, "Look again"});
		hints.push_back({Glyph::Circle, "Close"});
	}
	hintBar(dl, hints);
}
} // namespace bp
