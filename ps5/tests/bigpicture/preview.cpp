// Crux2 - the interface on a PC: every screen drawn, pictured and walked through.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The interface's screens (ps5/frontend/bigpicture/bp_*.cpp) need a Backend and ImGui, and
// nothing of PCSX2 or of the console. This program is another Backend: invented games with
// invented covers, a settings store in memory, a game that "runs", saved states with pictures.
// It draws the screens with the same ImGui and the same fonts as the title, rasterises what
// ImGui hands over in software, and writes pictures (PPM, which run-bigpicture-tests.sh turns
// into PNG).
//
//   preview shots <fonts folder> <out folder>   the screens, one picture each, at 1920x1080
//   preview crawl <fonts folder> [seed]         every row of every settings page changed in every
//                                               way, then random buttons for a long time, with
//                                               what the Backend was asked checked on the way;
//                                               built with the sanitizers, so a read past an
//                                               array or a use of something freed ends it
//
// Every game title, serial and cover here is made up.

#include "bp.h"
#include "bp_internal.h" // the screen that is on, for the checks
#include "ps5_mc.h"      // the memory cards the card manager is shown: true ones, held in memory
#include "ps5_osd_draw.h" // the on-screen display's panel, pictured with made-up figures

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

// Drawn over a frame when set (the on-screen display's panel, which the emulator draws over a game).
std::function<void()> g_overlay;

// -------------------------------------------------------------------- pictures

namespace
{

struct Pixels
{
	int width = 0, height = 0, bpp = 4;
	const unsigned char* data = nullptr;       // not owned (ImGui's own textures)
	std::vector<unsigned char> own;             // or owned
	const unsigned char* at() const { return data != nullptr ? data : own.data(); }
};

std::vector<std::unique_ptr<Pixels>> g_textures;
std::set<const Pixels*> g_live;

ImTextureID idOf(const Pixels* p)
{
	return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(p));
}

Pixels* makeTexture(int w, int h)
{
	auto p = std::make_unique<Pixels>();
	p->width = w;
	p->height = h;
	p->own.resize(static_cast<size_t>(w) * h * 4);
	Pixels* raw = p.get();
	g_textures.push_back(std::move(p));
	g_live.insert(raw);
	return raw;
}

struct Frame
{
	int width, height;
	std::vector<float> rgb; // linear in 0..255 space, three a pixel

	Frame(int w, int h) : width(w), height(h), rgb(static_cast<size_t>(w) * h * 3, 0.f) {}
};

// ImGui's textures: created, updated and destroyed as it asks.
void serveTextures(ImDrawData* data)
{
	if (data->Textures == nullptr)
		return;
	for (ImTextureData* tex : *data->Textures)
	{
		if (tex->Status == ImTextureStatus_WantCreate)
		{
			auto p = std::make_unique<Pixels>();
			p->width = tex->Width;
			p->height = tex->Height;
			p->bpp = tex->BytesPerPixel;
			p->data = static_cast<const unsigned char*>(tex->GetPixels());
			tex->SetTexID(idOf(p.get()));
			tex->BackendUserData = p.get();
			g_live.insert(p.get());
			g_textures.push_back(std::move(p));
			tex->SetStatus(ImTextureStatus_OK);
		}
		else if (tex->Status == ImTextureStatus_WantUpdates)
		{
			tex->SetStatus(ImTextureStatus_OK);
		}
		else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
		{
			g_live.erase(static_cast<Pixels*>(tex->BackendUserData));
			tex->SetTexID(ImTextureID_Invalid);
			tex->BackendUserData = nullptr;
			tex->SetStatus(ImTextureStatus_Destroyed);
		}
	}
}

unsigned g_badTextures = 0;
unsigned g_badIndices = 0;
unsigned long long g_triangles = 0;

inline void sample(const Pixels& t, float u, float v, float out[4])
{
	const float x = std::clamp(u, 0.f, 1.f) * t.width - 0.5f, y = std::clamp(v, 0.f, 1.f) * t.height - 0.5f;
	const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, t.width - 1), y0 = std::clamp(static_cast<int>(std::floor(y)), 0, t.height - 1);
	const int x1 = std::min(x0 + 1, t.width - 1), y1 = std::min(y0 + 1, t.height - 1);
	const float fx = std::clamp(x - x0, 0.f, 1.f), fy = std::clamp(y - y0, 0.f, 1.f);
	const unsigned char* base = t.at();
	const auto texel = [&](int px, int py, float* c) {
		const unsigned char* p = base + (static_cast<size_t>(py) * t.width + px) * t.bpp;
		if (t.bpp == 4)
		{
			c[0] = p[0];
			c[1] = p[1];
			c[2] = p[2];
			c[3] = p[3];
		}
		else
		{
			c[0] = c[1] = c[2] = 255.f;
			c[3] = p[0];
		}
	};
	float a[4], b[4], c[4], d[4];
	texel(x0, y0, a);
	texel(x1, y0, b);
	texel(x0, y1, c);
	texel(x1, y1, d);
	for (int i = 0; i < 4; i++)
		out[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
}

// What ImGui hands over, drawn: triangles with a colour at each corner, a texture, a clip
// rectangle, blended over what is there. `validate` only checks it (indices in range, textures
// that exist) without drawing.
void rasterise(ImDrawData* data, Frame* frame, bool validate)
{
	serveTextures(data);
	for (ImDrawList* list : data->CmdLists)
	{
		const ImDrawVert* vtx = list->VtxBuffer.Data;
		const ImDrawIdx* idx = list->IdxBuffer.Data;
		for (const ImDrawCmd& cmd : list->CmdBuffer)
		{
			if (cmd.UserCallback != nullptr)
				continue;
			const Pixels* tex = reinterpret_cast<const Pixels*>(static_cast<uintptr_t>(cmd.GetTexID()));
			if (tex == nullptr || g_live.count(tex) == 0)
			{
				g_badTextures++;
				continue;
			}
			if (cmd.IdxOffset + cmd.ElemCount > static_cast<unsigned>(list->IdxBuffer.Size))
			{
				g_badIndices++;
				continue;
			}
			const int cx0 = std::max(0, static_cast<int>(cmd.ClipRect.x)), cy0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
			const int cx1 = std::min(frame ? frame->width : 1 << 20, static_cast<int>(std::ceil(cmd.ClipRect.z)));
			const int cy1 = std::min(frame ? frame->height : 1 << 20, static_cast<int>(std::ceil(cmd.ClipRect.w)));
			for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3)
			{
				const unsigned i0 = idx[cmd.IdxOffset + e] + cmd.VtxOffset, i1 = idx[cmd.IdxOffset + e + 1] + cmd.VtxOffset,
							   i2 = idx[cmd.IdxOffset + e + 2] + cmd.VtxOffset;
				if (i0 >= static_cast<unsigned>(list->VtxBuffer.Size) || i1 >= static_cast<unsigned>(list->VtxBuffer.Size) ||
					i2 >= static_cast<unsigned>(list->VtxBuffer.Size))
				{
					g_badIndices++;
					continue;
				}
				g_triangles++;
				if (validate || frame == nullptr)
					continue;
				const ImDrawVert& a = vtx[i0];
				const ImDrawVert& b = vtx[i1];
				const ImDrawVert& c = vtx[i2];
				if (!std::isfinite(a.pos.x + a.pos.y + b.pos.x + b.pos.y + c.pos.x + c.pos.y))
				{
					g_badIndices++;
					continue;
				}
				const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
				if (area == 0)
					continue;
				const int x0 = std::max(cx0, static_cast<int>(std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))));
				const int x1 = std::min(cx1, static_cast<int>(std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))));
				const int y0 = std::max(cy0, static_cast<int>(std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))));
				const int y1 = std::min(cy1, static_cast<int>(std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))));
				if (x0 >= x1 || y0 >= y1)
					continue;
				const bool flat_uv = (a.uv.x == b.uv.x && a.uv.x == c.uv.x && a.uv.y == b.uv.y && a.uv.y == c.uv.y);
				float flat[4] = {255, 255, 255, 255};
				if (flat_uv)
					sample(*tex, a.uv.x, a.uv.y, flat);
				const ImU32 cols[3] = {a.col, b.col, c.col};
				float col[3][4];
				for (int k = 0; k < 3; k++)
				{
					col[k][0] = static_cast<float>((cols[k] >> IM_COL32_R_SHIFT) & 0xff);
					col[k][1] = static_cast<float>((cols[k] >> IM_COL32_G_SHIFT) & 0xff);
					col[k][2] = static_cast<float>((cols[k] >> IM_COL32_B_SHIFT) & 0xff);
					col[k][3] = static_cast<float>((cols[k] >> IM_COL32_A_SHIFT) & 0xff);
				}
				const float inv = 1.f / area;
				for (int y = y0; y < y1; y++)
				{
					const float py = y + 0.5f;
					float* out = frame->rgb.data() + (static_cast<size_t>(y) * frame->width + x0) * 3;
					for (int x = x0; x < x1; x++, out += 3)
					{
						const float px = x + 0.5f;
						float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) * inv;
						float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) * inv;
						float w2 = 1.f - w0 - w1;
						if (w0 < 0 || w1 < 0 || w2 < 0)
							continue;
						float t[4];
						if (flat_uv)
						{
							t[0] = flat[0];
							t[1] = flat[1];
							t[2] = flat[2];
							t[3] = flat[3];
						}
						else
						{
							sample(*tex, a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2, a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2, t);
						}
						const float alpha = (col[0][3] * w0 + col[1][3] * w1 + col[2][3] * w2) * t[3] / (255.f * 255.f);
						if (alpha <= 0.001f)
							continue;
						for (int k = 0; k < 3; k++)
						{
							const float src = (col[0][k] * w0 + col[1][k] * w1 + col[2][k] * w2) * t[k] / 255.f;
							out[k] = src * alpha + out[k] * (1 - alpha);
						}
					}
				}
			}
		}
	}
}

bool writePPM(const std::string& path, const Frame& frame)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	std::fprintf(f, "P6\n%d %d\n255\n", frame.width, frame.height);
	std::vector<unsigned char> row(static_cast<size_t>(frame.width) * 3);
	for (int y = 0; y < frame.height; y++)
	{
		for (int x = 0; x < frame.width * 3; x++)
			row[static_cast<size_t>(x)] = static_cast<unsigned char>(std::clamp(frame.rgb[static_cast<size_t>(y) * frame.width * 3 + x], 0.f, 255.f));
		std::fwrite(row.data(), 1, row.size(), f);
	}
	std::fclose(f);
	return true;
}

// An invented cover: coloured discs over a ground, in the title's own colours.
Pixels* inventCover(unsigned seed, int w, int h)
{
	Pixels* p = makeTexture(w, h);
	std::mt19937 rng(seed);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	const float hue = unit(rng);
	const auto colour = [&](float h2, float s, float v, float* rgb) { ImGui::ColorConvertHSVtoRGB(std::fmod(h2 + 1.f, 1.f), s, v, rgb[0], rgb[1], rgb[2]); };
	float ground[3];
	colour(hue, 0.6f, 0.55f, ground);
	struct Disc
	{
		float x, y, r, rgb[3];
	};
	std::vector<Disc> discs;
	for (int i = 0; i < 9; i++)
	{
		Disc d;
		d.x = unit(rng) * w;
		d.y = unit(rng) * h;
		d.r = (0.12f + 0.3f * unit(rng)) * w;
		colour(hue + (unit(rng) - 0.5f) * 0.25f, 0.5f + 0.5f * unit(rng), 0.5f + 0.5f * unit(rng), d.rgb);
		discs.push_back(d);
	}
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			float rgb[3] = {ground[0], ground[1], ground[2]};
			for (const Disc& d : discs)
			{
				const float dist = std::hypot(x - d.x, y - d.y);
				const float cover = std::clamp((d.r - dist) / 2.f, 0.f, 1.f) * 0.85f;
				for (int k = 0; k < 3; k++)
					rgb[k] = rgb[k] * (1 - cover) + d.rgb[k] * cover;
			}
			// A band across the lower part, where a real cover has its title.
			if (y > h * 0.74f && y < h * 0.9f)
			{
				for (float& k : rgb)
					k *= 0.25f;
			}
			unsigned char* out = p->own.data() + (static_cast<size_t>(y) * w + x) * 4;
			out[0] = static_cast<unsigned char>(rgb[0] * 255);
			out[1] = static_cast<unsigned char>(rgb[1] * 255);
			out[2] = static_cast<unsigned char>(rgb[2] * 255);
			out[3] = 255;
		}
	}
	return p;
}

// An invented picture of a game: a skyline at dusk.
Pixels* inventScene(unsigned seed)
{
	const int w = 320, h = 180;
	Pixels* p = makeTexture(w, h);
	std::mt19937 rng(seed);
	std::vector<int> towers(static_cast<size_t>(w));
	int height = 60;
	for (int x = 0; x < w; x++)
	{
		if (x % 14 == 0)
			height = 30 + static_cast<int>(rng() % 90);
		towers[static_cast<size_t>(x)] = height;
	}
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			unsigned char* out = p->own.data() + (static_cast<size_t>(y) * w + x) * 4;
			const float t = static_cast<float>(y) / h;
			const bool tower = (y > h - 20 - towers[static_cast<size_t>(x)] && y < h - 20);
			out[0] = static_cast<unsigned char>(tower ? 10 : 110 - 60 * t);
			out[1] = static_cast<unsigned char>(tower ? 14 : 80 - 40 * t);
			out[2] = static_cast<unsigned char>(tower ? 24 : 120 - 30 * t);
			if (y >= h - 20)
			{
				out[0] = out[1] = out[2] = 18;
			}
			out[3] = 255;
		}
	}
	return p;
}

// ---------------------------------------------------------------------- backend

} // namespace
// (With the sanitizers: where a failed check was asked from.)
extern "C" void __sanitizer_print_stack_trace(void) __attribute__((weak));
namespace
{

struct Check
{
	unsigned failed = 0;
	void expect(bool ok, const char* what)
	{
		if (!ok)
		{
			if (failed++ < 40)
			{
				std::fprintf(stderr, "FAILED: %s\n", what);
				if (__sanitizer_print_stack_trace)
					__sanitizer_print_stack_trace();
			}
		}
	}
} g_check;

class PreviewBackend final : public bp::Backend, public bp::CardShelf
{
public:
	std::vector<bp::GameInfo> list;
	std::map<std::string, Pixels*> covers;
	std::map<std::string, std::string> values[2]; // "section\nkey" -> value
	bool gameOpen = false;
	unsigned generation = 1, pages = 1;
	bp::Running run;
	bool isStarting = false;
	int startFrames = 0;
	bool startWillFail = false;
	bp::Pad padState;
	bool wantUsb = true, haveUsb = false;
	std::vector<std::string> shares = {"192.168.1.10/Games/PS2"};
	std::map<std::string, std::vector<bp::StateInfo>> savedStates;
	std::map<std::string, Pixels*> statePictures;
	float listenLeft = 0;
	std::string listenKey;
	std::map<std::string, std::string> bindings;
	bool busyCard = false;
	bool scanBusy = false;
	std::string note;
	unsigned quits = 0;
	std::vector<std::string> patchNames = {"Widescreen 16:9", "No interlacing", "60 frames a second"};
	std::vector<std::string> cheatNames = {"Infinite health", "All levels open", "Slow motion"};
	// The titles the games came with (one that was renamed gets its own back), the pictures some
	// of them have beside their covers, and the pictures for behind the screens.
	std::map<std::string, std::string> ownTitles;
	std::map<std::string, Pixels*> pictures; // path + "\n" + which
	std::vector<Pixels*> walls;
	int picturesFetched = 0;
	int corner = -1;

	static std::string k(const char* section, const char* key) { return std::string(section) + "\n" + key; }

	void add(const char* file, const char* title, const char* serial, int source, bp::GameInfo::Kind kind = bp::GameInfo::PS2, int compat = 5,
		time_t last = 0, time_t played = 0, bool cover = true)
	{
		bp::GameInfo g;
		const char* roots[3] = {"/data/homebrew/PPSA99249/games/", "/mnt/usb0/ps2/", "/smb/PS2/"};
		g.path = std::string(roots[source]) + file;
		g.fileName = file;
		g.title = title;
		g.serial = serial;
		g.source = source;
		g.kind = kind;
		g.compatibility = compat;
		g.lastPlayed = last;
		g.playedSeconds = played;
		g.crc = serial[0] ? static_cast<uint32_t>(std::hash<std::string>()(serial)) | 1u : 0;
		g.region = (std::strncmp(serial, "SLES", 4) == 0) ? "PAL-E" : (std::strncmp(serial, "SLPS", 4) == 0) ? "NTSC-J" : (serial[0] ? "NTSC-U" : "");
		g.size = 700ull * 1024 * 1024 + (std::hash<std::string>()(file) % 3500) * 1024ull * 1024;
		g.added = 1780000000 - static_cast<time_t>(std::hash<std::string>()(title) % 9000000);
		// (What a third of them have: cheats; a fifth: a texture pack.)
		g.hasCheats = serial[0] && (std::hash<std::string>()(serial) % 3 == 0);
		g.hasTextures = serial[0] && (std::hash<std::string>()(serial) % 5 == 0);
		if (cover)
			covers[g.path] = inventCover(static_cast<unsigned>(std::hash<std::string>()(title)), 256, 364);
		// Every other game with a cover has a title screen and a picture of play too.
		if (cover && list.size() % 2 == 0)
		{
			pictures[g.path + "\n0"] = inventScene(static_cast<unsigned>(std::hash<std::string>()(title) % 1000 + 11));
			pictures[g.path + "\n1"] = inventScene(static_cast<unsigned>(std::hash<std::string>()(title) % 1000 + 512));
		}
		ownTitles[g.path] = g.title;
		list.push_back(std::move(g));
	}

	void populate()
	{
		const time_t now = 1790000000;
		add("Brass Comet (USA).chd", "Brass Comet", "SLUS-29001", 0, bp::GameInfo::PS2, 6, now - 3600, 5 * 3600 + 720);
		add("The Ninth Lantern (USA).chd", "The Ninth Lantern", "SLUS-29002", 0, bp::GameInfo::PS2, 5, now - 86400, 41 * 3600);
		add("Moonfall Tactics (Europe) (En,Fr,De).iso", "Moonfall Tactics", "SLES-59003", 0, bp::GameInfo::PS2, 5, now - 3 * 86400, 1900);
		add("Starbound Rally (USA).chd", "Starbound Rally", "SLUS-29004", 0, bp::GameInfo::PS2, 4);
		add("Drift Lotus (Japan).iso", "Drift Lotus", "SLPS-29005", 0, bp::GameInfo::PS2, 3, 0, 0, false);
		add("Skyline Drift 2 (USA).chd", "Skyline Drift 2", "SLUS-29006", 0);
		add("Neon Harbor Racing (USA).chd", "Neon Harbor Racing", "SLUS-29007", 0, bp::GameInfo::PS2, 5, now - 9 * 86400, 600);
		add("Ashen Crown (USA) (Disc 1).chd", "Ashen Crown", "SLUS-29008", 0, bp::GameInfo::PS2, 5, now - 7 * 86400, 9 * 3600);
		add("Ashen Crown (USA) (Disc 2).chd", "Ashen Crown", "SLUS-29009", 0);
		add("Ashen Crown (USA) (Disc 3).chd", "Ashen Crown", "SLUS-29010", 0);
		add("Velvet Engine (USA).iso", "Velvet Engine", "SLUS-29011", 0, bp::GameInfo::PS2, 2);
		add("Quiet Orbit (Europe).iso", "Quiet Orbit", "SLES-59012", 0);
		add("Paper Kingdoms (USA).chd", "Paper Kingdoms", "SLUS-29013", 0, bp::GameInfo::PS2, 6);
		add("Harbor of Glass (USA).chd", "Harbor of Glass", "SLUS-29014", 0, bp::GameInfo::PS2, 5, 0, 0, false);
		add("Iron Meridian (USA).chd", "Iron Meridian", "SLUS-29015", 0);
		add("Juniper Run (USA).chd", "Juniper Run", "SLUS-29016", 0);
		add("Kite Season (Europe).iso", "Kite Season", "SLES-59017", 0);
		add("Lantern Street Detectives (USA).chd", "Lantern Street Detectives: The Case of the Very Long Title That Goes On", "SLUS-29018", 0);
		add("Obsidian Tide (USA).chd", "Obsidian Tide", "SLUS-29019", 0);
		add("Rust and Rain (USA).chd", "Rust and Rain", "SLUS-29020", 0);
		add("Salt Marsh Derby (USA).chd", "Salt Marsh Derby", "SLUS-29021", 0);
		add("Tin Soldier Parade (USA).chd", "Tin Soldier Parade", "SLUS-29022", 0);
		add("Umbra Circuit (USA).chd", "Umbra Circuit", "SLUS-29023", 0);
		add("Winter Signal (USA).chd", "Winter Signal", "SLUS-29024", 0);
		add("3 Minutes to Dawn (USA).chd", "3 Minutes to Dawn", "SLUS-29025", 0);
		add("unknown_dump_[!] (Disc 1).iso", "unknown_dump_[!] (Disc 1)", "", 0, bp::GameInfo::PS2, 0, 0, 0, false);
		add("Cobalt Alley (USA).bin", "Cobalt Alley", "SLUS-09026", 0, bp::GameInfo::PS1, 0);
		add("hello_world.elf", "hello_world", "", 0, bp::GameInfo::Program, 0, 0, 0, false);
		add("Ember Fields (USA).chd", "Ember Fields", "SLUS-29030", 2, bp::GameInfo::PS2, 5, now - 2 * 86400, 7200);
		add("Glass Harbour Nights (Europe).iso", "Glass Harbour Nights", "SLES-59031", 2);
		add("Hollow Compass (USA).chd", "Hollow Compass", "SLUS-29032", 2);
		add("Night Ferry (USA).chd", "Night Ferry", "SLUS-29033", 2, bp::GameInfo::PS2, 4, 0, 0, false);
		add("Zephyr Garden (USA).chd", "Zephyr Garden", "SLUS-29034", 2);

		// Saved states of two games.
		const auto states = [&](const char* serial, std::vector<std::pair<int, bool>> slots) {
			for (const bp::GameInfo& g : list)
			{
				if (g.serial != serial)
					continue;
				std::vector<bp::StateInfo>& out = savedStates[g.serial];
				for (const auto& [slot, backup] : slots)
				{
					bp::StateInfo s;
					s.slot = slot;
					s.backup = backup;
					s.when = now - 7200 * (slot + 3);
					s.path = "/states/" + g.serial + "." + std::to_string(slot) + (backup ? ".backup" : "");
					statePictures[s.path] = inventScene(static_cast<unsigned>(slot * 7 + 3));
					out.push_back(std::move(s));
				}
			}
		};
		states("SLUS-29001", {{-2, false}, {-1, false}, {1, false}, {1, true}, {2, false}, {5, false}});
		states("SLUS-29002", {{1, false}, {3, false}});
		for (bp::GameInfo& g : list)
			g.hasStates = savedStates.count(g.serial) != 0;
		for (unsigned i = 0; i < 3; i++)
			walls.push_back(inventScene(900 + i * 37));

		// The settings as the title writes them on its first start (a few of them).
		values[0][k("EmuCore/GS", "upscale_multiplier")] = "4";
		values[0][k("EmuCore/GS", "Renderer")] = "14";
		values[0][k("EmuCore/GS", "accurate_blending_unit")] = "1";
		values[0][k("PS5", "USBDrives")] = "true";
		values[0][k("DEV9/Eth", "ModeDNS1")] = "Manual";
		values[0][k("DEV9/Eth", "DNS1")] = "45.7.228.197";
		bindings["Pad1/Cross"] = "Cross";
		bindings["Pad1/Circle"] = "Circle";
		bindings["Hotkeys/OpenPauseMenu"] = "Options";
	}

	// ---- the library
	unsigned gamesGeneration() override { return generation; }
	std::vector<bp::GameInfo> games() override { return list; }
	bp::Texture cover(const bp::GameInfo& game) override
	{
		const auto it = covers.find(game.path);
		if (it == covers.end())
			return {};
		return {idOf(it->second), static_cast<float>(it->second->width), static_cast<float>(it->second->height)};
	}
	void scan(bool) override { scanBusy = true; }
	void setFavourite(const bp::GameInfo& game, bool favourite) override
	{
		for (bp::GameInfo& g : list)
		{
			if (g.path == game.path)
				g.favourite = favourite;
		}
		generation++;
	}
	void setHidden(const bp::GameInfo& game, bool hidden) override
	{
		for (bp::GameInfo& g : list)
		{
			if (g.path == game.path)
				g.hidden = hidden;
		}
		generation++;
	}
	void setTitle(const bp::GameInfo& game, const std::string& title) override
	{
		for (bp::GameInfo& g : list)
		{
			if (g.path != game.path)
				continue;
			g.title = title.empty() ? ownTitles[g.path] : title;
			g.renamed = !title.empty();
		}
		generation++;
	}
	bp::Texture picture(const bp::GameInfo& game, int which) override
	{
		const auto it = pictures.find(game.path + "\n" + std::to_string(which));
		if (it == pictures.end())
			return {};
		return {idOf(it->second), static_cast<float>(it->second->width), static_cast<float>(it->second->height)};
	}
	void fetchPictures(const bp::GameInfo* game, bool) override
	{
		picturesFetched++;
		if (game != nullptr && pictures.count(game->path + "\n0") == 0)
		{
			pictures[game->path + "\n0"] = inventScene(static_cast<unsigned>(picturesFetched * 5 + 3));
			pictures[game->path + "\n1"] = inventScene(static_cast<unsigned>(picturesFetched * 5 + 4));
		}
		bp::toast(game != nullptr ? "2 pictures fetched." : "Pictures fetched for 6 games.", 3.f);
	}
	int wallpapers(bool) override { return static_cast<int>(walls.size()); }
	bp::Texture wallpaper(int index) override
	{
		if (index < 0 || index >= static_cast<int>(walls.size()))
			return {};
		Pixels* const p = walls[static_cast<size_t>(index)];
		return {idOf(p), static_cast<float>(p->width), static_cast<float>(p->height)};
	}
	bool scanning() override { return scanBusy; }
	std::string backgroundNote() override { return note; }
	std::string shownFolder(const char* which) override
	{
		const std::string root = "/data/homebrew/PPSA99249/";
		if (std::strcmp(which, "root") == 0)
			return root;
		return root + which + "/";
	}
	bool usbWanted() override { return wantUsb; }
	bool usbAvailable() override { return haveUsb; }
	std::vector<std::string> networkFolders() override { return shares; }

	// ---- starting and the running game
	void begin(const bp::GameInfo& game)
	{
		g_check.expect(!run.valid || true, "start");
		isStarting = true;
		startFrames = 0;
		pending = game;
	}
	void start(const bp::GameInfo& game, int state, int boot) override
	{
		g_check.expect(!game.path.empty(), "a game is started by its path");
		g_check.expect(state == INT_MIN || state == -1 || (state >= 1 && state <= 10), "a start names a state slot that exists");
		g_check.expect(boot >= 0 && boot <= 2, "a start names a way to boot that exists");
		begin(game);
	}
	void startStateFile(const bp::GameInfo& game, const std::string& statePath) override
	{
		g_check.expect(!statePath.empty(), "a state to start from has a path");
		begin(game);
	}
	void startBios() override { begin(bp::GameInfo()); }
	bp::Running running() override { return run; }
	bool starting() override { return isStarting; }
	bp::LoadState loadState() override
	{
		bp::LoadState s;
		if (pending.source == 2)
		{
			s.text = "Reading the disc image into memory";
			s.progress = std::min(1.f, startFrames / 200.f);
		}
		return s;
	}
	// What the real machine does on another thread, some frames after it was asked: in the order
	// asked, each after its own wait.
	std::deque<std::pair<int, std::function<void()>>> later;
	int lag = 2;
	void after(int frames, std::function<void()> action) { later.emplace_back(frames, std::move(action)); }

	void setPaused(bool paused) override
	{
		g_check.expect(run.valid, "a game is paused or resumed only while one runs");
		after(lag, [this, paused] {
			if (!run.valid)
				return;
			const bool was = run.paused;
			run.paused = paused;
			if (was && !paused)
				bp::gameResumed();
		});
	}
	void saveState(int slot) override
	{
		g_check.expect(run.valid && slot >= 1 && slot <= 10, "a state is saved to a slot of the running game");
		// (Its file is there a while after: another thread packs it.)
		after(lag + 20, [this, slot, serial = run.serial] { putState(serial, slot); });
	}
	void putState(const std::string& serial, int slot)
	{
		std::vector<bp::StateInfo>& out = savedStates[serial];
		out.erase(std::remove_if(out.begin(), out.end(), [slot](const bp::StateInfo& s) { return s.slot == slot && !s.backup; }), out.end());
		bp::StateInfo s;
		s.slot = slot;
		s.when = 1790000000;
		s.path = "/states/" + serial + "." + std::to_string(slot);
		statePictures[s.path] = inventScene(static_cast<unsigned>(slot + 2));
		out.insert(slot < 0 ? out.begin() : out.end(), std::move(s));
	}
	void loadStateFile(const bp::StateInfo& state) override
	{
		g_check.expect(run.valid && !state.path.empty(), "a state is loaded into a running game");
		loadedSlot = state.slot;
	}
	void reset() override { g_check.expect(run.valid, "only a running game is restarted"); }
	void closeGame(bool save) override
	{
		g_check.expect(run.valid, "only a running game is closed");
		after(lag, [this, save] {
			if (!run.valid)
				return;
			// On its way down: for as long as its state takes to save.
			stoppingSerial = run.serial;
			stoppingSaves = save && run.canSaveState;
			run = bp::Running();
			run.stopping = true;
			stopLeft = save ? 70 : 12;
		});
	}
	void changeDisc(const std::string& path) override
	{
		g_check.expect(run.valid && !path.empty(), "a disc is changed in a running game");
		after(lag, [this, path] {
			if (run.valid)
				run.path = path;
		});
	}
	void setFrameLimit(bool on) override { run.frameLimit = on; }
	void toggleSoftwareRenderer() override { run.hardware = !run.hardware; }
	bool memoryCardBusy() override { return busyCard; }
	bool canUndoLoad() override { return undo && run.valid; }
	void undoLoad() override { undone++; }
	int achievementsOf() override { return run.valid ? cheevos : 0; }
	void openAchievements(int which) override { overlay = which; }
	bool overlayOpen() override { return overlay != 0 && run.valid; }
	bool gameRect(float rect[4]) override
	{
		// A picture of four to three in the middle of the screen.
		const ImVec2 size = ImGui::GetIO().DisplaySize;
		const float w = size.y * 4.f / 3.f;
		rect[0] = (size.x - w) / 2;
		rect[1] = 0;
		rect[2] = rect[0] + w;
		rect[3] = size.y;
		return run.valid;
	}
	void overlayCorner(int to) override { corner = to; }

	// ---- saved states
	std::vector<bp::StateInfo> states(const std::string& serial, uint32_t, bool withBackups) override
	{
		std::vector<bp::StateInfo> out;
		const auto it = savedStates.find(serial);
		if (it != savedStates.end())
		{
			for (const bp::StateInfo& s : it->second)
			{
				if (withBackups || !s.backup)
					out.push_back(s);
			}
		}
		std::sort(out.begin(), out.end(), [](const bp::StateInfo& a, const bp::StateInfo& b) {
			return a.slot != b.slot ? a.slot < b.slot : a.backup < b.backup;
		});
		return out;
	}
	bp::Texture statePicture(const bp::StateInfo& state) override
	{
		const auto it = statePictures.find(state.path);
		if (it == statePictures.end())
			return {};
		return {idOf(it->second), static_cast<float>(it->second->width), static_cast<float>(it->second->height)};
	}
	bool deleteState(const bp::StateInfo& state) override
	{
		for (auto& [serial, out] : savedStates)
			out.erase(std::remove_if(out.begin(), out.end(), [&](const bp::StateInfo& s) { return s.path == state.path; }), out.end());
		return true;
	}
	void resetPlayTime(const bp::GameInfo& game) override
	{
		for (bp::GameInfo& g : list)
		{
			if (g.serial == game.serial)
				g.playedSeconds = g.lastPlayed = 0;
		}
		generation++;
	}

	// ---- settings
	bool openGame(const bp::GameInfo& game) override
	{
		if (game.serial.empty() && game.crc == 0)
			return false;
		gameOpen = true;
		pages++;
		return true;
	}
	void closeGame() override
	{
		gameOpen = false;
		values[1].clear();
		pages++;
	}
	const std::string* find(int layer, const char* section, const char* key)
	{
		g_check.expect(layer == 0 || (layer == 1 && gameOpen), "a game's own value is asked for only while that game is open");
		g_check.expect(section != nullptr && key != nullptr && section[0] != 0 && key[0] != 0, "a value has a section and a key");
		if (layer < 0 || layer > 1)
			return nullptr;
		const auto it = values[layer].find(k(section, key));
		return it == values[layer].end() ? nullptr : &it->second;
	}
	std::optional<bool> getBool(int layer, const char* section, const char* key) override
	{
		const std::string* v = find(layer, section, key);
		return v ? std::optional<bool>(*v == "true") : std::nullopt;
	}
	std::optional<int> getInt(int layer, const char* section, const char* key) override
	{
		const std::string* v = find(layer, section, key);
		return v ? std::optional<int>(std::atoi(v->c_str())) : std::nullopt;
	}
	std::optional<float> getFloat(int layer, const char* section, const char* key) override
	{
		const std::string* v = find(layer, section, key);
		return v ? std::optional<float>(static_cast<float>(std::atof(v->c_str()))) : std::nullopt;
	}
	std::optional<std::string> getString(int layer, const char* section, const char* key) override
	{
		const std::string* v = find(layer, section, key);
		return v ? std::optional<std::string>(*v) : std::nullopt;
	}
	void put(int layer, const char* section, const char* key, std::string value)
	{
		find(layer, section, key);
		if (layer >= 0 && layer <= 1)
			values[layer][k(section, key)] = std::move(value);
		writes++;
	}
	void setBool(int layer, const char* section, const char* key, bool value) override { put(layer, section, key, value ? "true" : "false"); }
	void setInt(int layer, const char* section, const char* key, int value) override { put(layer, section, key, std::to_string(value)); }
	void setFloat(int layer, const char* section, const char* key, float value) override
	{
		g_check.expect(std::isfinite(value), "a value written is a number");
		put(layer, section, key, std::to_string(value));
	}
	void setString(int layer, const char* section, const char* key, const std::string& value) override { put(layer, section, key, value); }
	void erase(int layer, const char* section, const char* key) override
	{
		find(layer, section, key);
		g_check.expect(layer == 1, "only a game's own value is taken away");
		if (layer >= 0 && layer <= 1)
			values[layer].erase(k(section, key));
	}
	void clearGame() override
	{
		g_check.expect(gameOpen, "a game's values are cleared while it is open");
		values[1].clear();
	}
	int ownCount() override { return static_cast<int>(values[1].size()); }

	// ---- pages the Backend makes
	unsigned pagesGeneration() override { return pages; }
	bp::Option bind(const char* section, const char* key, const char* label)
	{
		bp::Option o;
		o.kind = bp::Option::Bind;
		o.section = section;
		o.key = key;
		o.label = label;
		return o;
	}
	static bp::Option head(const char* label)
	{
		bp::Option o;
		o.kind = bp::Option::Heading;
		o.label = label;
		return o;
	}
	bp::Page controllersPage(int) override
	{
		bp::Page page{"\xef\x84\x9b", "Controllers", {}};
		page.rows.push_back(head("CONTROLLER PORT 1"));
		bp::Option type;
		type.kind = bp::Option::StringList;
		type.label = "Controller";
		type.section = "Pad1";
		type.key = "Type";
		type.defString = "DualShock2";
		type.names = {"Not connected", "DualShock 2", "Guitar", "Jogcon", "Negcon", "Pop'n Music"};
		type.strings = {"None", "DualShock2", "Guitar", "Jogcon", "Negcon", "Popn"};
		page.rows.push_back(type);
		for (const char* button : {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Start", "Select", "Up", "Down", "Left", "Right",
				 "L3", "R3", "Left stick up", "Left stick down", "Large motor", "Small motor"})
			page.rows.push_back(bind("Pad1", button, button));
		bp::Option dead;
		dead.kind = bp::Option::FloatRange;
		dead.label = "Stick dead zone";
		dead.desc = "Stick movement ignored around the centre";
		dead.section = "Pad1";
		dead.key = "Deadzone";
		dead.lo = 0;
		dead.hi = 1;
		dead.step = 0.01;
		dead.scale = 100;
		dead.unit = "%";
		page.rows.push_back(dead);
		return page;
	}
	bp::Page hotkeysPage() override
	{
		bp::Page page{"\xef\x84\x9c", "Hotkeys", {}};
		page.rows.push_back(head("GENERAL"));
		page.rows.push_back(bind("Hotkeys", "OpenPauseMenu", "Open the quick menu"));
		page.rows.push_back(bind("Hotkeys", "ToggleTurbo", "Fast forward (hold)"));
		return page;
	}
	bp::Page memoryCardsPage(int) override
	{
		bp::Page page{"\xef\x9f\x82", "Memory cards", {}};
		for (int slot = 1; slot <= 2; slot++)
		{
			page.rows.push_back(head(slot == 1 ? "SLOT 1" : "SLOT 2"));
			bp::Option on;
			on.kind = bp::Option::Bool;
			on.label = "A card in the slot";
			on.section = "MemoryCards";
			on.key = slot == 1 ? "Slot1_Enable" : "Slot2_Enable";
			on.defBool = true;
			page.rows.push_back(on);
			bp::Option card;
			card.kind = bp::Option::StringList;
			card.label = "Card";
			card.section = "MemoryCards";
			card.key = slot == 1 ? "Slot1_Filename" : "Slot2_Filename";
			card.defString = slot == 1 ? "Mcd001.ps2" : "Mcd002.ps2";
			card.names = {"Mcd001 (8 MB)", "Mcd002 (8 MB)", "Big (64 MB)", "Saves (a folder)"};
			card.strings = {"Mcd001.ps2", "Mcd002.ps2", "Big.ps2", "Saves.ps2"};
			page.rows.push_back(card);
		}
		// v1.5.0: the cards, each opening its menu, and what can be made or brought in.
		page.rows.insert(page.rows.begin(), head("YOUR CARDS"));
		std::vector<bp::Option> cards;
		for (const std::string& name : cardNames())
		{
			bp::Option o;
			o.kind = bp::Option::Action;
			o.label = bp::cardTitle(name);
			o.desc = "Its saves, and what can be done with it";
			o.action = [this, name] { bp::openCard(this, name); };
			const int slot = slotOf(name);
			o.info = [slot] { return slot ? "8 MB, in slot " + std::to_string(slot) : std::string("8 MB"); };
			cards.push_back(std::move(o));
		}
		const auto action = [](const char* label, std::function<void()> run) {
			bp::Option o;
			o.kind = bp::Option::Action;
			o.label = label;
			o.desc = "";
			o.action = std::move(run);
			return o;
		};
		cards.push_back(action("New card...", [this] { bp::newCard(this); }));
		cards.push_back(action("Bring a whole card in...", [this] { bp::bringCardsIn(this); }));
		page.rows.insert(page.rows.begin() + 1, cards.begin(), cards.end());
		// v1.6.0: the card manager (the console's page has this row in place of the cards above;
		// the walk tries both: the cards' menus are the same shelf's).
		page.rows.push_back(head("CARD MANAGER"));
		page.rows.push_back(action("Open the card manager", [this] { bp::openCardManager(this); }));
		return page;
	}

	// ---- the saves on the cards. The cards are true ones (ps5_mc.h), held in memory: what the
	// menu does to them is done, and the checks look at what they hold afterwards.
	std::map<std::string, PS5Mc::Bytes> cardFiles, cardsBefore;
	std::map<std::string, PS5Mc::Bytes> saveFiles; // those "found" on the drives, by name
	std::map<std::string, PS5Mc::Bytes> writtenOut; // "place/name" -> the file
	bool saveFilesThere = true;

	static PS5Mc::Stamp cardNow() { return PS5Mc::Stamp::Of(2026, 10, 7, 21, 15, 0); }
	static PS5Mc::Save inventSave(const std::string& name, const std::string& title, size_t size)
	{
		PS5Mc::Save save;
		save.name = name;
		save.mode = PS5Mc::MODE_RWX | PS5Mc::MODE_DIR | PS5Mc::MODE_0400 | PS5Mc::MODE_EXISTS;
		save.created = save.modified = PS5Mc::Stamp::Of(2026, 9, 30, 19, 42, 0);
		PS5Mc::File icon;
		icon.name = "icon.sys";
		icon.mode = PS5Mc::MODE_RWX | PS5Mc::MODE_FILE | PS5Mc::MODE_0400 | PS5Mc::MODE_EXISTS;
		icon.created = icon.modified = save.created;
		icon.data.assign(964, 0);
		std::memcpy(icon.data.data(), "PS2D", 4);
		size_t at = 0xc0;
		for (const char c : title)
		{
			const unsigned code = (c >= 'A' && c <= 'Z') ? 0x8260u + static_cast<unsigned>(c - 'A') :
								  (c >= 'a' && c <= 'z') ? 0x8281u + static_cast<unsigned>(c - 'a') :
								  (c >= '0' && c <= '9') ? 0x824fu + static_cast<unsigned>(c - '0') : 0x8140u;
			icon.data[at++] = static_cast<uint8_t>(code >> 8);
			icon.data[at++] = static_cast<uint8_t>(code);
		}
		PS5Mc::detail::Put16(icon.data.data() + 6, static_cast<uint16_t>(title.size() * 2));
		save.files.push_back(icon);
		PS5Mc::File data = icon;
		data.name = "SAVEDATA.BIN";
		data.data.assign(size, 0);
		for (size_t i = 0; i < size; i++)
			data.data[i] = static_cast<uint8_t>(i * 31 + size);
		save.files.push_back(std::move(data));
		return save;
	}
	void inventCards()
	{
		PS5Mc::Card card;
		PS5Mc::Bytes image(PS5Mc::Card::SIZE_8MB_ECC, 0xff);
		cardFiles["Mcd002.ps2"] = image; // never formatted
		PS5Mc::Card::Format(&image, cardNow());
		std::string why;
		card.Open(image, &why);
		card.Write(inventSave("BESLES-50321CITY", "Harbour City", 180000), cardNow(), &why);
		card.Write(inventSave("BESLES-51188RACE", "Night Circuit 2", 420000), cardNow(), &why);
		card.Write(inventSave("BASLUS-20744OPT", "Options", 3000), cardNow(), &why);
		cardFiles["Mcd001.ps2"] = card.Image();
		cardFiles["Saves.ps2"] = PS5Mc::Bytes(); // a folder card
		saveFiles["Moon Garden.psu"] = PS5Mc::WritePSU(inventSave("BESLES-52002MOON", "Moon Garden", 96000));
		saveFiles["downloaded.max"] = PS5Mc::Bytes(3000, 0x55); // (no save: it is said so)
	}
	/// What a card holds, for the checks: its saves' names with commas between; "?" when it does not open.
	std::string cardHolds(const std::string& name)
	{
		PS5Mc::Card card;
		std::string why, out;
		if (!cardFiles.count(name) || !card.Open(cardFiles[name], &why))
			return "?";
		for (const PS5Mc::Card::Listed& one : card.List())
			out += (out.empty() ? "" : ",") + one.name;
		return out;
	}
	/// A change to a card: made on a copy that is opened (and made ready when it is new); kept
	/// when `change` says it was made, with the card as it was kept for "put back".
	void changeCard(const std::string& name, const char* failed, const std::function<bool(PS5Mc::Card&, std::string*)>& change)
	{
		std::string said;
		PS5Mc::Bytes image = cardFiles[name];
		if (PS5Mc::Card::Unformatted(image))
			PS5Mc::Card::Format(&image, cardNow());
		PS5Mc::Card card;
		if (!card.Open(std::move(image), &said) || !change(card, &said))
		{
			bp::message(failed, said);
			return;
		}
		cardsBefore[name] = cardFiles[name];
		cardFiles[name] = card.Image();
		pages++;
		bp::toast(said);
	}

	std::vector<std::string> cardNames() override
	{
		std::vector<std::string> names;
		for (const auto& [name, image] : cardFiles)
			names.push_back(name);
		return names;
	}
	bp::CardView lookAtCard(const std::string& name) override
	{
		bp::CardView view;
		if (!cardFiles.count(name))
		{
			view.why = "The card is not there any more";
			return view;
		}
		PS5Mc::Bytes image = cardFiles[name];
		if (image.empty())
		{
			view.why = "A folder card: its saves are the folders inside it, to be copied as they are";
			return view;
		}
		view.undo = cardsBefore.count(name) != 0;
		const bool fresh = PS5Mc::Card::Unformatted(image);
		if (fresh)
			PS5Mc::Card::Format(&image, cardNow());
		PS5Mc::Card card;
		if (!card.Open(std::move(image), &view.why))
			return view;
		view.state = fresh ? bp::CardView::New : bp::CardView::Listed;
		view.freeBytes = card.FreeBytes();
		view.totalBytes = card.TotalBytes();
		for (const PS5Mc::Card::Listed& one : card.List())
			view.saves.push_back({one.name, one.title, one.modified.Text(), one.bytes});
		return view;
	}
	std::string cardsBusy() override
	{
		return (run.valid || isStarting) ? std::string("Close the game first: it has the memory cards, and would write over the change.") : std::string();
	}
	void findSaveFiles(std::function<void(std::vector<bp::SaveFileFound>)> then) override
	{
		std::vector<bp::SaveFileFound> found;
		if (saveFilesThere)
		{
			for (const auto& [name, bytes] : saveFiles)
				found.push_back({"/mnt/usb0/saves/" + name, name, "USB drive 1", bytes.size()});
		}
		then(std::move(found));
	}
	std::string saveFilePlaces() override
	{
		return "Save files (.psu, .max, .cbs, .sps, .xps, .psv) are looked for in a folder named saves on every USB drive that is in and on every "
			   "share of network.cfg, and in /data/homebrew/PPSA99249/saves (which the console's FTP server reaches).";
	}
	std::vector<std::string> exportPlaces() override { return {"The saves folder of Crux2", "USB drive 1", "Share: games"}; }
	void importSave(const std::string& card, const bp::SaveFileFound& file) override
	{
		changeCard(card, "The save was not brought in", [&](PS5Mc::Card& to, std::string* said) {
			PS5Mc::Save save;
			if (!PS5Mc::ReadSaveFile(saveFiles[file.name], &save, cardNow(), said) || !to.Write(save, cardNow(), said))
				return false;
			*said = "\"" + save.name + "\" is on " + bp::cardTitle(card) + " now.";
			return true;
		});
	}
	void exportSave(const std::string& card, const std::string& name, int place) override
	{
		PS5Mc::Card from;
		PS5Mc::Save save;
		std::string why;
		if (!from.Open(cardFiles[card], &why) || !from.Read(name, &save, &why))
		{
			bp::message("The save was not written out", why);
			return;
		}
		writtenOut[exportPlaces()[static_cast<size_t>(place)] + "/" + name + ".psu"] = PS5Mc::WritePSU(save);
		bp::toast("\"" + name + "\" was written as " + name + ".psu.");
	}
	void copySave(const std::string& card, const std::string& name, const std::string& toCard) override
	{
		PS5Mc::Card from;
		PS5Mc::Save save;
		std::string why;
		if (!from.Open(cardFiles[card], &why) || !from.Read(name, &save, &why))
		{
			bp::message("The save was not copied", why);
			return;
		}
		changeCard(toCard, "The save was not copied", [&](PS5Mc::Card& to, std::string* said) {
			if (!to.Write(save, cardNow(), said))
				return false;
			*said = "\"" + name + "\" is on " + bp::cardTitle(toCard) + " too now.";
			return true;
		});
	}
	void deleteSave(const std::string& card, const std::string& name) override
	{
		changeCard(card, "The save was not deleted", [&](PS5Mc::Card& from, std::string* said) {
			if (!from.Remove(name, cardNow(), said))
				return false;
			*said = "\"" + name + "\" was deleted from " + bp::cardTitle(card) + ".";
			return true;
		});
	}
	void undoCard(const std::string& card) override
	{
		if (!cardsBefore.count(card))
			return;
		std::swap(cardFiles[card], cardsBefore[card]);
		pages++;
		bp::toast(bp::cardTitle(card) + " is as it was before the last change.");
	}
	void renameCard(const std::string& card, const std::string& title) override
	{
		const std::string name = title + ".ps2";
		if (title.empty() || cardFiles.count(name))
		{
			bp::toast(title.empty() ? "A card's name cannot be empty." : "There is a card named \"" + title + "\" already.");
			return;
		}
		cardFiles[name] = std::move(cardFiles[card]);
		cardFiles.erase(card);
		if (cardsBefore.count(card))
		{
			cardsBefore[name] = std::move(cardsBefore[card]);
			cardsBefore.erase(card);
		}
		pages++;
		bp::toast("The card is \"" + title + "\" now.");
	}
	void duplicateCard(const std::string& card) override
	{
		cardFiles[bp::cardTitle(card) + " copy.ps2"] = cardFiles[card];
		pages++;
		bp::toast("\"" + bp::cardTitle(card) + " copy\" is a copy of " + bp::cardTitle(card) + " now: choose it as a slot's card.");
	}
	void deleteCard(const std::string& card) override
	{
		cardFiles.erase(card);
		cardsBefore.erase(card);
		pages++;
		bp::toast("\"" + bp::cardTitle(card) + "\" was deleted.");
	}

	// ---- v1.5.0: the card manager's sources, made up: a PlayStation 2 with the save files' saves
	// on its card, and a USB drive with the save files and a card image.
	std::string ps2 = "";
	PS5Mc::Bytes FoundImage()
	{
		PS5Mc::Bytes image(PS5Mc::Card::SIZE_8MB_ECC, 0);
		PS5Mc::Card::Format(&image, cardNow());
		PS5Mc::Card card;
		std::string why;
		card.Open(image, &why);
		for (const auto& [name, bytes] : saveFiles)
		{
			PS5Mc::Save save;
			if (PS5Mc::ReadSaveFile(bytes, &save, cardNow(), &why))
				card.Write(save, cardNow(), &why);
			break;
		}
		return card.Image();
	}
	std::vector<std::string> cardSources() override { return {"A PlayStation 2 on the network", "The saves folder of Crux2", "USB drive 1", "Share: games"}; }
	std::string ps2Address() override { return ps2; }
	void findIn(int source, const std::string& address, std::function<void(std::vector<bp::CardFound>, std::string)> then) override
	{
		std::vector<bp::CardFound> found;
		if (source == 0)
		{
			ps2 = address;
			for (const auto& [name, bytes] : saveFiles)
				found.push_back({bp::CardFound::Ps2Save, "/mc/0/" + name, name, "Memory card 1", bytes.size(), address});
			then(std::move(found), std::string());
			return;
		}
		if (source == 2 && saveFilesThere)
		{
			for (const auto& [name, bytes] : saveFiles)
				found.push_back({bp::CardFound::SaveFile, "/mnt/usb0/saves/" + name, name, "USB drive 1, saves", bytes.size(), {}});
			found.push_back({bp::CardFound::CardImage, "/mnt/usb0/VMC/Old card.ps2", "Old card.ps2", "USB drive 1, VMC", PS5Mc::Card::SIZE_8MB_ECC, {}});
		}
		then(std::move(found), std::string());
	}
	void bringIn(const std::string& card, const bp::CardFound& found) override
	{
		bringInCount++;
		importSave(card, {found.path, found.name, found.where, found.bytes});
	}
	void bringInCard(const bp::CardFound&, const std::string& title) override
	{
		cardFiles[title + ".ps2"] = FoundImage();
		pages++;
		bp::toast("\"" + title + "\" was brought in.");
	}
	bp::CardView lookAtImage(const bp::CardFound&) override
	{
		bp::CardView view;
		PS5Mc::Card card;
		if (!card.Open(FoundImage(), &view.why))
			return view;
		view.state = bp::CardView::Listed;
		view.freeBytes = card.FreeBytes();
		view.totalBytes = card.TotalBytes();
		for (const PS5Mc::Card::Listed& one : card.List())
			view.saves.push_back({one.name, one.title, one.modified.Text(), one.bytes});
		return view;
	}
	void copyFromImage(const bp::CardFound&, const std::string& name, const std::string& toCard) override
	{
		PS5Mc::Card from;
		PS5Mc::Save save;
		std::string why;
		if (!from.Open(FoundImage(), &why) || !from.Read(name, &save, &why))
			return;
		changeCard(toCard, "The save was not copied", [&](PS5Mc::Card& to, std::string* said) {
			if (!to.Write(save, cardNow(), said))
				return false;
			*said = "\"" + name + "\" is on " + bp::cardTitle(toCard) + " now.";
			return true;
		});
	}
	void createCard(const std::string& title, int megabytes) override
	{
		PS5Mc::Bytes image(static_cast<size_t>(std::max(megabytes, 8) / 8) * PS5Mc::Card::SIZE_8MB_ECC, 0);
		PS5Mc::Card::Format(&image, cardNow());
		cardFiles[title + ".ps2"] = std::move(image);
		pages++;
		bp::toast("\"" + title + "\" was made.");
	}
	void formatCard(const std::string& card) override
	{
		changeCard(card, "The card was not formatted", [&](PS5Mc::Card& c, std::string* said) {
			PS5Mc::Bytes image(c.Image().size(), 0);
			PS5Mc::Card::Format(&image, cardNow());
			std::string why;
			c.Open(std::move(image), &why);
			*said = bp::cardTitle(card) + " is formatted.";
			return true;
		});
		formatCount++;
	}
	void exportCard(const std::string& card, int place) override
	{
		writtenOut[exportPlaces()[static_cast<size_t>(place)] + "/Crux2/memory cards/" + card] = cardFiles[card];
		bp::toast("\"" + bp::cardTitle(card) + "\" was written out.");
	}
	int slotOf(const std::string& card) override { return card == "Mcd001.ps2" ? 1 : card == "Mcd002.ps2" ? 2 : 0; }
	// The card manager's grouping: a card named by a serial of the library is that game's.
	std::string cardSerial(const std::string& card) override
	{
		const std::string title = bp::cardTitle(card);
		for (const bp::GameInfo& g : games())
		{
			if (!g.serial.empty() && title.compare(0, g.serial.size(), g.serial) == 0)
				return g.serial;
		}
		return std::string();
	}
	std::string cardGame(const std::string& card) override
	{
		const std::string serial = cardSerial(card);
		for (const bp::GameInfo& g : games())
		{
			if (!serial.empty() && g.serial == serial)
				return g.title;
		}
		return std::string();
	}
	bool systemCard(const std::string& card) override { return card == "Mcd001.ps2" || card == "Mcd002.ps2"; }
	int bringInCount = 0, formatCount = 0;
	bp::Page biosPage(int) override
	{
		bp::Page page{"\xef\x8b\x9b", "BIOS", {}};
		bp::Option bios;
		bios.kind = bp::Option::StringList;
		bios.label = "BIOS";
		bios.desc = "The console's own program, which every game needs";
		bios.section = "Filenames";
		bios.key = "BIOS";
		bios.names = {"The first one found", "USA v02.20 (my-bios-a.bin)", "Europe v01.60 (my-bios-b.bin)"};
		bios.strings = {"", "my-bios-a.bin", "my-bios-b.bin"};
		page.rows.push_back(bios);
		return page;
	}
	// The PlayStation 2's network adapter and hard drive, with invented values.
	bp::Page networkPage(int) override
	{
		bp::Page page{"\xef\x9b\xbf", "Network", {}};
		page.rows.push_back(head("NETWORK ADAPTER"));
		bp::Option on;
		on.kind = bp::Option::Bool;
		on.label = "Network adapter";
		on.desc = "The PlayStation 2's adapter, for games that go online or play over the local network";
		on.section = "DEV9/Eth";
		on.key = "EthEnable";
		page.rows.push_back(on);
		bp::Option state;
		state.kind = bp::Option::Info;
		state.label = "The console's connection";
		state.info = [] { return std::string("192.0.2.17, through 192.0.2.1"); };
		page.rows.push_back(state);
		// The first name server, chosen in one step, as the console's page has it.
		bp::Option first;
		first.kind = bp::Option::Action;
		first.label = "First name server";
		first.desc = "Where the game asks for the addresses of the servers it plays on";
		first.info = [this] {
			if (getString(0, "DEV9/Eth", "ModeDNS1").value_or("Auto") != "Manual")
				return std::string("The console's own");
			const std::string now = getString(0, "DEV9/Eth", "DNS1").value_or("");
			return now == "45.7.228.197" ? std::string("PS2 Online  (45.7.228.197)") : (now.empty() ? std::string("Not set") : now);
		};
		first.action = [this] {
			const std::string now = getString(0, "DEV9/Eth", "DNS1").value_or("");
			const bool manual = getString(0, "DEV9/Eth", "ModeDNS1").value_or("Auto") == "Manual";
			bp::pick("First name server",
				{"PS2 Online   \xc2\xb7   45.7.228.197   \xc2\xb7   ps2online.com", "The console's own name server", "An address of your own..."},
				!manual ? 1 : (now == "45.7.228.197" ? 0 : 2), [this, now](int choice) {
					if (choice == 0)
					{
						setString(0, "DEV9/Eth", "DNS1", "45.7.228.197");
						setString(0, "DEV9/Eth", "ModeDNS1", "Manual");
					}
					else if (choice == 1)
					{
						setString(0, "DEV9/Eth", "ModeDNS1", "Auto");
					}
					else if (choice == 2)
					{
						bp::editAddress("First name server's address", now, "192.0.2.1", [this](const std::string& entered) {
							int numbers[4];
							g_check.expect(entered.empty() || bp::parseAddress(entered, numbers), "an address entered is an address");
							setString(0, "DEV9/Eth", "DNS1", entered);
							setString(0, "DEV9/Eth", "ModeDNS1", entered.empty() ? "Auto" : "Manual");
							addresses++;
						});
					}
				});
		};
		page.rows.push_back(first);
		{
			bp::Option address;
			address.kind = bp::Option::Action;
			address.label = "Second name server";
			address.desc = "Asked when the first does not answer";
			address.info = [this] {
				const std::string now = getString(0, "DEV9/Eth", "DNS2").value_or("");
				return now.empty() ? std::string("Not set") : now;
			};
			address.action = [this, label = address.label] {
				bp::editAddress(label, getString(0, "DEV9/Eth", "DNS2").value_or(""), "192.0.2.1", [this](const std::string& entered) {
					int numbers[4];
					g_check.expect(entered.empty() || bp::parseAddress(entered, numbers), "an address entered is an address");
					setString(0, "DEV9/Eth", "DNS2", entered);
					addresses++;
				});
			};
			page.rows.push_back(address);
		}
		page.rows.push_back(head("HARD DRIVE"));
		bp::Option drive;
		drive.kind = bp::Option::Bool;
		drive.label = "Hard drive";
		drive.desc = "The PlayStation 2's hard drive, kept in one file";
		drive.section = "DEV9/Hdd";
		drive.key = "HddEnable";
		page.rows.push_back(drive);
		bp::Option make;
		make.kind = bp::Option::Action;
		make.label = "Make a new hard drive";
		make.desc = "An empty drive of 40 GB; the game formats it";
		make.info = [] { return std::string("None yet"); };
		make.action = [] { bp::toast("A hard drive of 40 GB was made."); };
		page.rows.push_back(make);
		return page;
	}
	bp::Page patchesPage(bool cheats) override
	{
		bp::Page page{cheats ? "\xef\x83\xa7" : "\xef\x91\xa2", cheats ? "Cheats" : "Patches", {}};
		for (const std::string& name : cheats ? cheatNames : patchNames)
		{
			bp::Option o;
			o.kind = bp::Option::Bool;
			o.label = name;
			o.desc = cheats ? "An invented cheat" : "An invented patch";
			o.section = cheats ? "Cheats" : "Patches";
			o.key = name;
			o.flags = bp::Option::OneGame;
			page.rows.push_back(o);
		}
		return page;
	}
	// Backups on an invented drive and share: one made by hand, one automatic, one cut short.
	bp::Page backupPage() override
	{
		bp::Page page{"\xef\x83\xae", "Backup", {}};
		page.rows.push_back(head("MEMORY CARDS AND SAVED STATES"));
		bp::Option where;
		where.kind = bp::Option::StringList;
		where.label = "Backups go to";
		where.desc = "Kept there in a folder named Crux2-backup, one folder for each backup";
		where.section = "PS5";
		where.key = "BackupPlace";
		where.names = {"USB drive 1", "Share: games"};
		where.strings = {"/mnt/usb0", "/smb/games"};
		where.defString = "/mnt/usb0";
		page.rows.push_back(where);
		bp::Option states;
		states.kind = bp::Option::Bool;
		states.label = "Saved states too";
		states.desc = "Off: a backup holds the memory cards only";
		states.section = "PS5";
		states.key = "BackupStates";
		states.defBool = true;
		page.rows.push_back(states);
		bp::Option automatic = states;
		automatic.label = "Back up the memory cards when a game is closed";
		automatic.desc = "Whenever a card was written to since the last time. The newest 10 of these are kept";
		automatic.key = "AutoBackup";
		automatic.defBool = false;
		page.rows.push_back(automatic);
		bp::Option now;
		now.kind = bp::Option::Action;
		now.label = "Back up now";
		now.desc = "Copies every memory card and saved state to the place above";
		now.flags = bp::Option::NoGame;
		now.info = [this] { return backupDoing; };
		now.action = [this] {
			backups++;
			bp::toast("Backed up 2 memory card files and 14 saved states (212 MB). On USB drive 1.");
		};
		page.rows.push_back(now);
		page.rows.push_back(head("BACKUPS ON USB DRIVE 1"));
		const auto snapshot = [&](const char* date, const char* holds, bool automaticOne) {
			bp::Option row;
			row.kind = bp::Option::Action;
			row.label = date;
			row.desc = std::string(automaticOne ? "Made when a game was closed. Holds " : "Holds ") + holds + ". Cross copies it back to the console";
			row.flags = bp::Option::NoGame;
			const std::string text = automaticOne ? "Automatic" : "212 MB";
			row.info = [text] { return text; };
			const std::string when = date;
			row.action = [this, when] {
				bp::confirm("Restore the backup of " + when + "?",
					"Its memory cards and saved states are copied back to the console, in place of the ones of the same name.\n\n"
					"What they replace is not lost: it is moved to the folder backup-before-restore in Crux2's folder.",
					[this](bool yes) {
						if (yes)
						{
							restores++;
							bp::message("The backup was restored", "Restored 2 memory card files and 14 saved states. What they replaced is in the folder backup-before-restore.");
						}
					},
					"Restore it", "Leave it");
			};
			page.rows.push_back(row);
		};
		snapshot("6 October 2026, 14:32", "2 memory card files, 14 saved states, 212 MB", false);
		snapshot("5 October 2026, 22:10", "2 memory card files, 16 MB", true);
		{
			bp::Option cut;
			cut.kind = bp::Option::Info;
			cut.label = "4 October 2026, 09:01";
			cut.info = [] { return std::string("Not finished: it cannot be restored"); };
			page.rows.push_back(cut);
		}
		bp::Option again;
		again.kind = bp::Option::Action;
		again.label = "Look again";
		again.desc = "Reads the list of backups of the place once more";
		again.action = [] { bp::toast("Looking for backups...", 3.f); };
		page.rows.push_back(again);
		return page;
	}
	// The logs: an invented one, with long lines, warnings and the report of a crash.
	bp::Page logPage() override
	{
		bp::Page page{"\xef\x85\x9c", "Log", {}};
		page.rows.push_back(head("WHAT CRUX2 WROTE DOWN"));
		bp::Option run;
		run.kind = bp::Option::Action;
		run.label = "This run";
		run.desc = "What Crux2 has noted since it was started. Opens at its end";
		run.action = [this] {
			logsShown++;
			std::vector<std::string> lines;
			lines.push_back("[   0.000] PCSX2 v2.9.103");
			lines.push_back("[   0.001] build: the preview");
			for (int i = 0; i < 140; i++)
			{
				char line[512];
				if (i % 17 == 5)
					std::snprintf(line, sizeof(line),
						"[%8.3f] warning: a line that is far too long for the sheet, so that it has to go on on the next one and be told apart "
						"from the lines around it by where it starts: vfs: connected to smb://nas/games as guest, SMB 2.1 (reads of up to 1048576 "
						"bytes), and so on and on until it has said all of it",
						1.0 + i * 0.731);
				else if (i % 23 == 7)
					std::snprintf(line, sizeof(line), "[%8.3f] error: something did not work (%d)", 1.0 + i * 0.731, i);
				else
					std::snprintf(line, sizeof(line), "[%8.3f] game list: %d game(s), %.1f s", 1.0 + i * 0.731, 40 + i, 0.2 + i * 0.01);
				lines.push_back(line);
			}
			lines.push_back("");
			lines.push_back("==== Crux2 stopped: SIGSEGV ====");
			lines.push_back("  signal 11, code 1, fault address 0x0000000000000010");
			lines.push_back("==== end of the report ====");
			bp::showText("The log of this run", std::move(lines));
		};
		page.rows.push_back(run);
		bp::Option before = run;
		before.label = "The run before";
		before.desc = "The same of the run before this one";
		before.info = [] { return std::string("Ended in a crash"); };
		before.action = [] { bp::showText("The log of the run before", {}); };
		page.rows.push_back(before);
		page.rows.push_back(head("TAKING THEM ALONG"));
		bp::Option copy;
		copy.kind = bp::Option::Action;
		copy.label = "Copy the logs to USB drive 1";
		copy.desc = "Puts the three logs in a folder named Crux2-logs there";
		copy.action = [] { bp::toast("Copied 3 logs to the folder Crux2-logs/2026-10-06_14-40-02. On USB drive 1."); };
		page.rows.push_back(copy);
		return page;
	}
	// RetroAchievements: an invented account, signed in to on the keyboard.
	bp::Page achievementsPage() override
	{
		bp::Page page{"\xef\x82\x91", "Achievements", {}};
		page.rows.push_back(head("RETROACHIEVEMENTS"));
		bp::Option account;
		account.kind = bp::Option::Info;
		account.label = "Account";
		account.info = [this] { return signedIn.empty() ? std::string("Not signed in") : signedIn; };
		page.rows.push_back(account);
		bp::Option sign;
		sign.kind = bp::Option::Action;
		sign.label = signedIn.empty() ? "Sign in" : "Sign out";
		sign.desc = "With the name and the password of your account at retroachievements.org";
		sign.flags = bp::Option::NoGame;
		sign.action = [this] {
			if (!signedIn.empty())
			{
				signedIn.clear();
				pages++;
				return;
			}
			bp::editText("Your name at RetroAchievements", "", false, [this](const std::string& name) {
				if (name.empty())
					return;
				bp::editText("The password of " + name, "", true, [this, name](const std::string& password) {
					if (password.empty())
						return;
					signedIn = name;
					pages++;
					bp::toast("Signed in as " + name + ".");
				});
			});
		};
		page.rows.push_back(sign);
		bp::Option on;
		on.kind = bp::Option::Bool;
		on.label = "Achievements";
		on.desc = "Games you play are checked for the achievements RetroAchievements has for them";
		on.section = "Achievements";
		on.key = "Enabled";
		on.flags = bp::Option::NoGame;
		page.rows.push_back(on);
		return page;
	}
	// What the database recommends for the open game: an invented change of its own setting.
	std::vector<bp::Option> gameRows() override
	{
		std::vector<bp::Option> rows;
		bp::Option row;
		row.kind = bp::Option::Action;
		row.label = "Recommended settings";
		row.desc = "Sets for this game what PCSX2's game database recommends for it (the blending accuracy first), and says what it changed";
		row.flags = bp::Option::OneGame;
		row.info = [this] { return recommended ? std::string("Nothing to change") : std::string("1 change"); };
		row.action = [this] {
			if (recommended)
			{
				bp::message("Recommended for this game", "This game has what the database recommends already. Nothing was changed.");
				return;
			}
			recommended = true;
			setInt(bp::Own, "EmuCore/GS", "accurate_blending_unit", 3);
			bp::message("Recommended for this game",
				"Set for this game only:\n  -  Blending accuracy: Basic, now High\n\nPCSX2's compatibility list says of this game: Playable.\n\n"
				"To undo: \"Back to the Settings for everything\" on this page, or each option in Graphics.");
		};
		rows.push_back(row);
		return rows;
	}
	bp::Page aboutPage() override
	{
		bp::Page page{"\xef\x81\x9a", "About", {}};
		const auto line = [&](const char* label, const char* value) {
			bp::Option o;
			o.kind = bp::Option::Info;
			o.label = label;
			const std::string v = value;
			o.info = [v] { return v; };
			page.rows.push_back(o);
		};
		line("Crux2", "the preview");
		line("Emulator", "PCSX2, by the PCSX2 Dev Team");
		bp::Option quit;
		quit.kind = bp::Option::Action;
		quit.label = "Quit PCSX2";
		quit.desc = "Closes the title";
		quit.action = [this] { this->quit(); };
		page.rows.push_back(quit);
		return page;
	}
	std::vector<bp::Option> portRows() override
	{
		std::vector<bp::Option> rows;
		rows.push_back(head("PLAYSTATION 5"));
		bp::Option usb;
		usb.kind = bp::Option::Bool;
		usb.label = "USB drives";
		usb.desc = "A USB tab for games in a folder named pcsx2 or ps2 on a drive";
		usb.section = "PS5";
		usb.key = "USBDrives";
		usb.defBool = true;
		usb.flags = bp::Option::Restart;
		rows.push_back(usb);
		return rows;
	}

	// ---- controller buttons
	std::string binding(int, const bp::Option& option) override
	{
		const auto it = bindings.find(option.section + "/" + option.key);
		return it == bindings.end() ? std::string() : it->second;
	}
	void listen(int, const bp::Option& option) override
	{
		g_check.expect(option.kind == bp::Option::Bind, "a button is waited for only for an option that takes one");
		listenLeft = 5.f;
		listenKey = option.section + "/" + option.key;
	}
	float listening() override { return listenLeft; }
	void cancelListen() override { listenLeft = 0; }
	void clearBinding(int, const bp::Option& option) override { bindings.erase(option.section + "/" + option.key); }

	// ---- the rest
	bp::Pad pad() override { return padState; }
	void quit() override { quits++; }
	void log(const std::string&) override {}
	const char* appName() override { return "Crux2"; }
	// The moments of the start-up: each once, in their order, and none after the library opened.
	void sound(bp::Sound what) override
	{
		const int n = static_cast<int>(what);
		g_check.expect(n >= 0 && n < 3 && !sounded[n], "a moment of the start-up is told once");
		for (int later = n + 1; later < 3; later++)
			g_check.expect(!sounded[later], "the moments of the start-up come in their order");
		if (what == bp::Sound::StartupSkipped)
			g_check.expect(sounded[0], "a start-up that was cut short had begun");
		if (n >= 0 && n < 3)
			sounded[n] = true;
	}
	bool sounded[3] = {};
	int addresses = 0;
	int backups = 0, restores = 0, logsShown = 0;
	bool undo = false;
	int undone = 0, cheevos = 0, overlay = 0;
	int loadedSlot = 0; // the slot of the state last loaded into the running game
	std::string signedIn;
	bool recommended = false;
	std::string backupDoing;

	// The console's disc drive: nothing in it, a game, or a disc that is no game.
	bp::DiscState drive;
	void putDisc(int what)
	{
		const unsigned generation = drive.generation + 1;
		drive = bp::DiscState();
		drive.generation = generation;
		drive.driveFound = true;
		if (what == 1)
		{
			drive.present = drive.playable = true;
			drive.game.path = "disc:/dev/cd0";
			drive.game.title = "Lantern Bay";
			drive.game.serial = "SLUS-29777";
			drive.game.region = "NTSC-U";
			drive.game.crc = 0x29777AAA;
			drive.game.compatibility = 5;
			drive.game.size = 4300000000ull;
			drive.games.push_back(drive.game);
			// Its cover, title screen and picture of play, as fetched for a disc.
			if (covers.count(drive.game.path) == 0)
				covers[drive.game.path] = inventCover(2977, 256, 364);
			if (pictures.count(drive.game.path + "\n0") == 0)
			{
				pictures[drive.game.path + "\n0"] = inventScene(41);
				pictures[drive.game.path + "\n1"] = inventScene(42);
			}
		}
		else if (what == 3)
		{
			// Two drives: the console's own with a DVD, a USB DVD drive with a CD.
			drive.present = drive.playable = true;
			bp::GameInfo dvd;
			dvd.path = dvd.fileName = "/dev/cd0";
			dvd.title = "Lantern Bay";
			dvd.serial = "SLUS-29777";
			dvd.region = "NTSC-U";
			dvd.source = 3;
			dvd.size = 4300000000ull;
			bp::GameInfo cd = dvd;
			cd.path = cd.fileName = "/dev/cd1";
			cd.title = "Pocket Orbit";
			cd.serial = "SLUS-29780";
			cd.cd = true;
			cd.size = 600000000ull;
			drive.games = {dvd, cd};
			drive.game = dvd;
		}
		else if (what == 2)
		{
			drive.present = true;
			drive.note = "This is a film on DVD, not a PlayStation 2 game.";
		}
	}
	bp::DiscState disc() override { return drive; }
	// What the Disc tab says of the game: made up, as an encyclopedia would have it.
	bp::GameAbout about(const bp::GameInfo& game) override
	{
		bp::GameAbout a;
		if (game.serial == "SLUS-29777")
		{
			a.found = true;
			a.summary = "Lantern Bay (\xe7\x81\xaf\xe5\x8f\xb0\xe3\x81\xae\xe6\xb9\xbe, Hepburn: T\xc5\x8d" "dai no Wan) is a 2003 action-adventure game developed by Harbour Light Studio and published by Northwind Interactive for "
						"the PlayStation 2. The player guides a lighthouse keeper's daughter through a fog-bound fishing town, relighting the "
						"lamps of the bay while the tide rises and falls on a day-and-night cycle. It was praised for its watercolour look and "
						"its score, and later became a cult classic.";
			a.developer = "Harbour Light Studio";
			a.publisher = "Northwind Interactive";
			a.released = "2003";
			a.genre = "Action-adventure game";
			a.source = "Wikipedia, \"Lantern Bay\" (CC BY-SA 4.0)";
		}
		else
		{
			a.looking = true;
		}
		return a;
	}
	void makeImage(const bp::GameInfo& game) override
	{
		imagesAsked++;
		bp::pick("Which kind of image?", {"ISO", "CHD, smallest", "CHD, quickest to load", "CSO"}, 0, [](int) {});
	}
	int imagesAsked = 0;

	// One frame of the world: what was asked of the machine happens, a start completes, a game on
	// its way down is down, a wait for a button ends.
	void tick(float dt)
	{
		for (auto& [frames, action] : later)
			frames--;
		while (!later.empty() && later.front().first <= 0)
		{
			const std::function<void()> action = std::move(later.front().second);
			later.pop_front();
			action();
		}
		if (run.stopping && --stopLeft <= 0)
		{
			run = bp::Running();
			if (stoppingSaves)
				putState(stoppingSerial, -1);
			bp::gameStopped();
		}
		if (isStarting && ++startFrames > (pending.source == 2 ? 220 : 40))
		{
			isStarting = false;
			if (startWillFail)
			{
				bp::startFailed("The disc image could not be opened (an invented reason).");
			}
			else
			{
				run = bp::Running();
				run.valid = true;
				run.title = pending.title;
				run.serial = pending.serial;
				run.path = pending.path;
				run.crc = pending.crc;
				run.canSaveState = pending.crc != 0;
				run.sessionSeconds = 754;
				bp::gameStarted();
			}
		}
		if (listenLeft > 0)
		{
			listenLeft -= dt;
			if (listenLeft < 4.2f && padState.held != 0)
			{
				bindings[listenKey] = "Button " + std::to_string(padState.held);
				listenLeft = 0;
			}
		}
		if (scanBusy && ++scanFrames > 90)
		{
			scanBusy = false;
			scanFrames = 0;
		}
	}

	unsigned long long writes = 0;

	std::string stoppingSerial;
	bool stoppingSaves = false;
	int stopLeft = 0;

private:
	bp::GameInfo pending;
	int scanFrames = 0;
};

// ---------------------------------------------------------------------- driving

struct Harness
{
	PreviewBackend backend;
	int width, height;
	std::string out;
	unsigned frames = 0;

	Harness(int w, int h) : width(w), height(h) { backend.inventCards(); }

	bool optionsWas = false;
	int over = 0, under = 0, nowhere = 0;

	void frame(uint32_t held, Frame* target, bool validate)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
		io.DeltaTime = 1.f / 60.f;
		backend.padState.held = held;
		// The quick menu's hotkey, as the emulator fires it: when its button is let go while a
		// game is loaded, whether the interface had the press or not.
		const bool optionsNow = (held & bp::Pad::Options) != 0;
		if (optionsWas && !optionsNow && backend.run.valid)
			backend.after(1, [] { bp::openQuickMenu(); });
		optionsWas = optionsNow;
		backend.tick(io.DeltaTime);
		ImGui::NewFrame();
		bp::render();
		if (g_overlay)
			g_overlay();
		ImGui::Render();
		rasterise(ImGui::GetDrawData(), target, validate);
		frames++;

		// What must hold whatever was pressed, the machine's lag allowed for (half a second):
		// a game stands still while a screen is over it, runs while none is, and with no game
		// there is the library (or the settings).
		const bool overGame = (bp::screen == bp::Screen::Quick || bp::screen == bp::Screen::Settings || bp::screen == bp::Screen::Disc);
		over = (backend.run.valid && overGame && !backend.run.paused) ? over + 1 : 0;
		under = (backend.run.valid && bp::screen == bp::Screen::None && !bp::active() && backend.run.paused) ? under + 1 : 0;
		nowhere = (!backend.run.valid && !backend.run.stopping && !backend.starting() &&
					  bp::screen != bp::Screen::Library && bp::screen != bp::Screen::Settings) ?
					  nowhere + 1 :
					  0;
		g_check.expect(over < 30, "a game stands still while a screen is over it");
		g_check.expect(under < 30, "a game runs while no screen is over it");
		g_check.expect(nowhere < 30, "with no game there is the library or the settings");
	}

	void wait(int n, uint32_t held = 0)
	{
		for (int i = 0; i < n; i++)
			frame(held, nullptr, true);
	}

	// A button pressed and let go.
	void press(uint32_t button, int settle = 14)
	{
		wait(2, button);
		wait(settle);
	}

	void shot(const char* name)
	{
		Frame picture(width, height);
		frame(0, &picture, false);
		const std::string path = out + "/" + name + ".ppm";
		if (!writePPM(path, picture))
			std::fprintf(stderr, "cannot write %s\n", path.c_str());
		else
			std::printf("  %s\n", path.c_str());
	}
};

bool loadFonts(const std::string& folder);
std::string g_fontFolder;

// What the emulator does when a setting asks for another renderer: the graphics device, and ImGui
// with it, is made anew under whatever screen is up. ImGui's clock and frame count start again,
// its fonts and textures are other ones.
void remakeContext()
{
	for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
	{
		if (tex->BackendUserData != nullptr)
			g_live.erase(static_cast<Pixels*>(tex->BackendUserData));
	}
	ImGui::DestroyContext();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	loadFonts(g_fontFolder);
}

bool loadFonts(const std::string& folder)
{
	static std::vector<unsigned char> files[4];
	const char* names[4] = {"Roboto-Regular.ttf", "Roboto-Bold.ttf", "fa-solid-900.ttf", "Saira-ExtraBold.ttf"};
	for (int i = 0; i < 4; i++)
	{
		const std::string path = folder + "/" + names[i];
		FILE* f = std::fopen(path.c_str(), "rb");
		if (f == nullptr)
		{
			std::fprintf(stderr, "cannot read %s\n", path.c_str());
			return false;
		}
		std::fseek(f, 0, SEEK_END);
		const long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		files[i].resize(static_cast<size_t>(size));
		const size_t got = std::fread(files[i].data(), 1, files[i].size(), f);
		std::fclose(f);
		if (got != files[i].size())
			return false;
	}
	ImGuiIO& io = ImGui::GetIO();
	ImFont* fonts[2];
	for (int i = 0; i < 2; i++)
	{
		// As the title's own fonts are made (ImGui/ImGuiManager.cpp): the text face, then the
		// icons merged into it.
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		fonts[i] = io.Fonts->AddFontFromMemoryTTF(files[i].data(), static_cast<int>(files[i].size()), 15.f, &cfg, nullptr);
		static const ImWchar exclude[] = {0x1, 0xdfff, 0xf900, 0x10ffff, 0, 0};
		ImFontConfig icons;
		icons.MergeMode = true;
		icons.PixelSnapH = true;
		icons.GlyphMinAdvanceX = 18.f;
		icons.GlyphMaxAdvanceX = 18.f;
		icons.GlyphExcludeRanges = exclude;
		icons.FontDataOwnedByAtlas = false;
		io.Fonts->AddFontFromMemoryTTF(files[2].data(), static_cast<int>(files[2].size()), 15.f * 0.9f, &icons, nullptr);
	}
	bp::setFonts(fonts[0], fonts[1]);
	{
		// The face the name is set in.
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		bp::setBrandFont(io.Fonts->AddFontFromMemoryTTF(files[3].data(), static_cast<int>(files[3].size()), 15.f, &cfg, nullptr));
	}
	return true;
}

using P = bp::Pad;

void shots(Harness& h)
{
	PreviewBackend& b = h.backend;
	// The start-up animation, then the library.
	// (Seconds of the animation; its clock starts after three frames.)
	int at = 0;
	const auto until = [&](float seconds, const char* name) {
		const int frame = 3 + static_cast<int>(seconds * 60 + 0.5f);
		h.wait(frame - at);
		at = frame;
		h.shot(name);
	};
	until(0.55f, "00a-startup-0.55");
	until(1.14f, "00b-startup-1.14");
	until(1.75f, "00c-startup-1.75");
	until(2.40f, "00d-startup-2.40");
	until(3.40f, "00e-startup-3.40");
	until(4.50f, "00f-startup-4.50");
	until(4.85f, "00g-startup-4.85");
	until(5.30f, "00h-startup-5.30");
	h.wait(120);
	g_check.expect(b.sounded[0] && !b.sounded[1] && b.sounded[2], "a start-up left alone begins and opens the library");
	// Circle, with nothing to go back to: the question.
	h.press(P::Circle, 30);
	h.shot("01-quit-question");
	h.press(P::Circle, 20);
	g_check.expect(b.quits == 0, "the question left with Circle closes nothing");
	h.press(P::Circle, 20);
	h.press(P::Cross, 20);
	g_check.expect(b.quits == 0, "the question opens on No");
	h.press(P::Circle, 20);
	h.press(P::Left, 5);
	h.press(P::Cross, 20);
	g_check.expect(b.quits == 1, "Yes closes the title");
	b.quits = 0;
	// A disc is put in the drive: it is said, and it has its tab.
	b.putDisc(1);
	h.wait(40);
	h.shot("01b-disc-put-in");
	h.press(P::L1, 40);
	h.shot("01c-disc-tab");
	h.press(P::Triangle, 40);
	h.shot("01d-disc-details");
	// The disc's details have its image to make; so does Square on the tab.
	h.press(P::Right, 6);
	h.press(P::Right, 6);
	h.press(P::Right, 20);
	h.shot("01d2-disc-details-image");
	h.press(P::Cross, 20);
	g_check.expect(b.imagesAsked == 1, "Make an image in the disc's details asks for an image");
	h.shot("01d3-disc-image-kinds");
	h.press(P::Circle, 20);
	h.press(P::Circle, 30);
	h.press(P::Square, 20);
	g_check.expect(b.imagesAsked == 2, "Square on the Disc tab asks for an image");
	h.press(P::Circle, 30);
	b.putDisc(0);
	h.wait(40);
	h.shot("01e-disc-taken-out");
	b.putDisc(2);
	h.wait(20);
	h.press(P::L1, 40);
	h.shot("01f-disc-not-a-game");
	// A USB DVD drive beside the console's own: a CD in it, a DVD in the other.
	// (The Disc tab is open from the shot before.)
	b.putDisc(3);
	h.wait(40);
	h.shot("01g-two-drives");
	b.putDisc(0);
	h.wait(30);
	h.press(P::Right);
	h.press(P::Right);
	h.wait(30);
	h.shot("02-library-shelves");
	h.press(P::Down, 30);
	h.press(P::Right);
	h.press(P::R2, 4);
	h.shot("03-library-letter");
	h.wait(60);
	h.press(P::Up, 30);

	// A game's details, its states, the way to start it.
	h.press(P::Left);
	h.press(P::Left);
	h.press(P::Triangle, 30);
	h.shot("04-details");
	h.press(P::Right);
	h.press(P::Cross, 20);
	h.press(P::Down);
	h.shot("05-details-states");
	h.press(P::Circle);
	h.press(P::Right);
	h.press(P::Right);
	h.press(P::Cross, 20);
	h.shot("06-details-more");
	h.press(P::Cross, 20);
	h.shot("06b-details-start-with");
	h.press(P::Circle);
	// The game's other pictures, in the cover's place.
	h.press(P::Down, 20);
	h.shot("06c-details-title-screen");
	h.press(P::Down, 20);
	h.shot("06d-details-in-play");
	h.press(P::Down, 10);
	h.press(P::Circle, 30);

	// The other views.
	h.press(P::TouchPad, 40);
	h.press(P::Down);
	h.press(P::Right);
	h.shot("07-library-grid");
	h.press(P::TouchPad, 40);
	h.press(P::Down);
	h.shot("08-library-list");
	h.press(P::TouchPad, 40);
	h.press(P::Right);
	h.press(P::Right);
	h.press(P::Right, 40);
	h.shot("08b-library-flow");
	h.press(P::TouchPad, 40);
	h.press(P::Right);
	h.press(P::Right, 40);
	h.shot("08c-library-bookcase");
	h.press(P::TouchPad, 40);
	h.press(P::Down);
	h.press(P::Down, 30);
	h.shot("08d-library-table");
	// (Each of the four views of v1.9.0 is left on the game it was come to with: what follows starts
	// the game focused before them.)
	h.press(P::TouchPad, 40);
	h.press(P::Right, 60);
	h.shot("08e-library-showcase");
	h.press(P::Left, 10);
	h.press(P::TouchPad, 40);
	h.press(P::Down, 60);
	h.shot("08f-library-wheel");
	h.press(P::Up, 10);
	h.press(P::TouchPad, 40);
	h.press(P::Right, 40);
	h.shot("08g-library-wall");
	h.press(P::Left, 10);
	h.press(P::TouchPad, 40);
	h.press(P::Right, 50);
	h.shot("08h-library-discs");
	h.press(P::Left, 10);
	h.press(P::TouchPad, 40);
	g_check.expect(b.getInt(0, "PS5UI", "View").value_or(-1) == 0, "the touch pad goes through the ten views and comes back to the shelves");

	// The other tabs.
	h.press(P::R1, 40);
	h.shot("09-usb-not-available");
	h.press(P::R1, 40);
	h.shot("10-network");
	h.press(P::L1, 30);
	h.press(P::L1, 40);

	// The settings: eleven pages (v1.0.0). The walk goes to a page and a row by their names.
	const auto go = [&](const char* page, const char* row = nullptr) {
		const std::string what = std::string("the settings have ") + page + (row ? std::string(" > ") + row : std::string());
		g_check.expect(bp::settingsFocus(page, row), what.c_str());
		h.wait(20);
	};
	h.press(P::Options, 40);
	h.shot("11-settings-library");
	go("Interface");
	h.shot("11a-settings-interface");
	go("Interface", "Skin");
	h.press(P::Cross, 20);
	h.shot("12-settings-pick");
	h.press(P::Circle);
	h.press(P::Circle);
	go("Statistics");
	h.shot("12b-settings-statistics");
	go("Graphics", "Look (shader preset)");
	h.shot("12c-settings-picture");
	h.press(P::Cross, 20);
	h.shot("12d-settings-looks");
	h.press(P::Circle);
	go("Emulation", "Frame pacing");
	h.shot("12e-settings-motion");
	go("Graphics", "Blending accuracy");
	h.shot("13-settings-picture-quality");
	h.press(P::Circle);
	go("Interface", "Go dark when nothing is pressed");
	h.shot("13-0-settings-screen-care");
	go("Interface", "Text size");
	h.shot("13-0b-settings-seeing");
	// The settings' levels (v1.9.0): Triangle goes from Normal to Advanced to Expert and back.
	go("Graphics", "Internal resolution");
	h.shot("13-0c-settings-normal");
	h.press(P::Triangle, 30);
	g_check.expect(b.getInt(0, "PS5UI", "SettingsLevel").value_or(-1) == 1, "Triangle shows the advanced settings");
	go("Graphics", "Internal resolution");
	h.shot("13-0d-settings-advanced");
	h.press(P::Triangle, 30);
	g_check.expect(b.getInt(0, "PS5UI", "SettingsLevel").value_or(-1) == 2, "Triangle again shows every setting");
	go("Graphics", "Internal resolution");
	h.shot("13-0e-settings-expert");
	h.press(P::Triangle, 30);
	g_check.expect(b.getInt(0, "PS5UI", "SettingsLevel").value_or(-1) == 0, "and again goes back to the normal settings");
	// The interface smaller (v1.9.0).
	b.setInt(0, "PS5UI", "InterfaceSize", 80);
	bp::loadOptions();
	go("Interface", "Interface size");
	h.shot("13-0f-settings-at-80");
	b.setInt(0, "PS5UI", "InterfaceSize", 100);
	bp::loadOptions();
	// Memory cards (v1.5.0): saves brought in from a USB drive, from a PlayStation 2 over the network
	// and from a card image; a card made, one brought in whole, one formatted; a save copied, deleted
	// and brought back, written out; a card written out, copied, deleted and renamed.
	{
		const auto openCard = [&](const char* title) {
			go("Memory Cards", title);
			h.press(P::Cross, 20);
		};
		const auto row = [&](int which) {
			for (int i = 0; i < which; i++)
				h.press(P::Down, 4);
			h.press(P::Cross, 20);
		};
		const auto yes = [&] {
			h.press(P::Left, 6);
			h.press(P::Cross, 20);
		};
		g_check.expect(b.cardHolds("Mcd001.ps2") == "BESLES-50321CITY,BESLES-51188RACE,BASLUS-20744OPT" && b.cardHolds("Mcd002.ps2") == "?",
			"the cards begin as they were invented");
		go("Memory Cards", "Mcd001");
		h.shot("13-1-settings-memory-cards");
		h.press(P::Cross, 20);
		h.shot("13-3-card-menu");
		row(0); // bring saves in
		h.shot("13-2-from-where");
		row(2); // a USB drive
		h.shot("13-4-found-on-usb");
		row(0);
		g_check.expect(b.cardHolds("Mcd001.ps2") == "BESLES-50321CITY,BESLES-51188RACE,BASLUS-20744OPT,BESLES-52002MOON", "a save is brought in from a file");
		h.shot("13-5-save-brought-in");
		// What is no save file is said to be none, and the card stays.
		openCard("Mcd001");
		row(0);
		row(2);
		row(1);
		h.shot("13-6-not-a-save-file");
		g_check.expect(b.cardHolds("Mcd001.ps2") == "BESLES-50321CITY,BESLES-51188RACE,BASLUS-20744OPT,BESLES-52002MOON", "what is no save file changes nothing");
		h.press(P::Cross, 20);
		// From a PlayStation 2 on the network: its address asked, its saves listed.
		openCard("Mcd002");
		row(0);
		row(0);
		h.shot("13-6a-ps2-address");
		h.press(P::Cross, 30);
		h.shot("13-6b-ps2-saves");
		g_check.expect(b.ps2 == "192.168.1.20", "the PlayStation 2's address is asked, and remembered");
		row(0);
		g_check.expect(b.cardHolds("Mcd002.ps2") == "BESLES-52002MOON", "a save is brought in from a PlayStation 2, onto a card nobody formatted yet");
		// A new card: its size, then its name.
		go("Memory Cards", "New card...");
		h.press(P::Cross, 20);
		h.shot("13-6c-new-card-size");
		row(0);
		h.shot("13-6d-new-card-name");
		h.press(P::Options, 20);
		g_check.expect(b.cardFiles.count("Card 1.ps2") == 1 && b.cardHolds("Card 1.ps2").empty(), "a new card is made, empty");
		// A save of a card image on a USB drive, onto it.
		openCard("Card 1");
		row(0);
		row(2);
		row(2);
		h.shot("13-6e-card-image-saves");
		row(0);
		g_check.expect(b.cardHolds("Card 1.ps2") == "BESLES-52002MOON", "a save of a card image is brought in");
		// A whole card brought in, and formatted.
		go("Memory Cards", "Bring a whole card in...");
		h.press(P::Cross, 20);
		h.shot("13-6f-card-from-where");
		row(1);
		row(0);
		h.press(P::Options, 20);
		g_check.expect(b.cardHolds("Old card.ps2") == "BESLES-52002MOON", "a whole card is brought in");
		openCard("Old card");
		row(3);
		h.shot("13-6g-format-question");
		h.press(P::Cross, 20);
		g_check.expect(b.cardHolds("Old card.ps2") == "BESLES-52002MOON", "a card is not formatted by the answer the question opens on");
		openCard("Old card");
		row(3);
		yes();
		g_check.expect(b.cardHolds("Old card.ps2").empty() && b.formatCount == 1, "a card is formatted after Format");
		// A save, copied to another card.
		openCard("Mcd001");
		row(1);
		h.shot("13-7-save-menu");
		row(0);
		h.shot("13-8-other-cards");
		row(1);
		g_check.expect(b.cardHolds("Mcd002.ps2").find("BESLES-50321CITY") != std::string::npos, "a save is copied to another card");
		g_check.expect(b.cardHolds("Mcd001.ps2").find("BESLES-50321CITY") != std::string::npos, "and stays on the card it came from");
		// Deleted: asked first, and not by the button that opened the question.
		openCard("Mcd001");
		row(1);
		row(2);
		h.shot("13-9-delete-question");
		h.press(P::Cross, 20);
		g_check.expect(b.cardHolds("Mcd001.ps2").find("CITY") != std::string::npos, "a save is not deleted by the answer the question opens on");
		openCard("Mcd001");
		row(1);
		row(2);
		yes();
		g_check.expect(b.cardHolds("Mcd001.ps2") == "BESLES-51188RACE,BASLUS-20744OPT,BESLES-52002MOON", "a save is deleted after Delete");
		// The last change undone: the save is there again.
		openCard("Mcd001");
		h.shot("13-10-card-menu-with-undo");
		row(4);
		h.shot("13-11-undo-question");
		yes();
		g_check.expect(b.cardHolds("Mcd001.ps2") == "BESLES-50321CITY,BESLES-51188RACE,BASLUS-20744OPT,BESLES-52002MOON", "the last change is undone");
		// Written out as a file, to the second of the places.
		openCard("Mcd001");
		row(2);
		row(1);
		h.shot("13-12-write-out-where");
		row(1);
		g_check.expect(b.writtenOut.count("USB drive 1/BESLES-51188RACE.psu") == 1, "a save is written out as a file to the place chosen");
		{
			PS5Mc::Save back;
			std::string why;
			g_check.expect(PS5Mc::ReadSaveFile(b.writtenOut["USB drive 1/BESLES-51188RACE.psu"], &back, PS5Mc::Stamp(), &why) && back.name == "BESLES-51188RACE" &&
							   back.files.size() == 2 && back.files[1].data.size() == 420000,
				"and the file reads as that save");
		}
		// The whole card written out.
		openCard("Mcd001");
		row(6);
		h.shot("13-12b-card-out-where");
		row(0);
		g_check.expect(b.writtenOut.count("USB drive 1/Crux2/memory cards/Mcd001.ps2") == 1, "a whole card is written out");
		// The card itself: a copy, the copy deleted, a new name.
		openCard("Mcd002");
		row(7);
		g_check.expect(b.cardFiles.count("Mcd002 copy.ps2") == 1 && b.cardHolds("Mcd002 copy.ps2") == b.cardHolds("Mcd002.ps2"), "a card is copied with what is on it");
		openCard("Mcd002 copy");
		row(7);
		h.shot("13-13-delete-card-question");
		h.press(P::Cross, 20);
		g_check.expect(b.cardFiles.count("Mcd002 copy.ps2") == 1, "a card is not deleted by the answer the question opens on");
		openCard("Mcd002 copy");
		row(7);
		yes();
		g_check.expect(b.cardFiles.count("Mcd002 copy.ps2") == 0, "a card is deleted after Delete the card");
		openCard("Mcd002");
		row(6);
		h.shot("13-14-rename-card");
		h.press(P::Cross, 6); // one more letter
		h.press(P::Options, 20);
		g_check.expect(b.cardFiles.count("Mcd002.ps2") == 0 && b.cardHolds("Mcd002q.ps2").find("MOON") != std::string::npos, "a card is renamed, and holds what it held");
		h.shot("13-15-card-renamed");
		// A folder card is not looked into, and says so.
		openCard("Saves");
		h.shot("13-16-folder-card");
		h.press(P::Circle, 10);
		// Nothing found: where things are looked for is said.
		b.saveFilesThere = false;
		openCard("Mcd001");
		row(0);
		row(2);
		h.shot("13-17-nothing-found");
		h.press(P::Cross, 20);
		b.saveFilesThere = true;
		h.press(P::Circle);
	}
	// The card manager (v1.6.0): a game's card by its title and cover, the saves on the right, a
	// save's menu and the card's.
	{
		b.cardFiles["SLUS-29001.ps2"] = b.cardFiles["Mcd001.ps2"];
		b.pages++;
		go("Memory Cards", "Open the card manager");
		h.press(P::Cross, 30);
		g_check.expect(bp::cardManagerOpen(), "the card manager opens");
		h.shot("13m-1-card-manager");
		h.press(P::Down, 8); // Bring a card in
		h.press(P::Down, 8); // over the GAMES heading, to the game's card
		h.shot("13m-2-game-card");
		h.press(P::Cross, 20);
		h.shot("13m-3-card-saves");
		h.press(P::Down, 8);
		h.press(P::Cross, 30);
		h.shot("13m-4-save-menu");
		h.press(P::Circle, 20); // the save's menu goes
		g_check.expect(bp::cardManagerOpen(), "a menu over the manager goes back to it");
		h.press(P::Circle, 20); // back to the cards
		h.press(P::Triangle, 30);
		h.shot("13m-5-card-options");
		h.press(P::Circle, 20);
		for (int i = 0; i < 4; i++)
			h.press(P::Down, 6);
		h.shot("13m-6-system-cards");
		h.press(P::Circle, 30);
		g_check.expect(!bp::cardManagerOpen(), "Circle on the cards closes the manager");
		b.cardFiles.erase("SLUS-29001.ps2");
		b.pages++;
	}
	// Backups: one is made, one restored after the question.
	go("Memory Cards", "Backups go to");
	h.shot("13a-settings-backup");
	for (int i = 0; i < 3; i++)
		h.press(P::Down, 6);
	h.press(P::Cross, 20);
	g_check.expect(b.backups == 1, "Back up now makes a backup");
	h.press(P::Down, 6);
	h.press(P::Cross, 30);
	h.shot("13a2-settings-restore-question");
	h.press(P::Cross, 20);
	g_check.expect(b.restores == 0, "a restore is not started by the button that opened the question");
	h.press(P::Cross, 20);
	h.press(P::Left, 6);
	h.press(P::Cross, 30);
	g_check.expect(b.restores == 1, "a restore is started by Yes");
	h.shot("13a3-settings-restored");
	h.press(P::Cross, 20);
	h.press(P::Circle);
	// The network adapter and the hard drive; an address is changed number by number.
	go("Network & HDD", "Network adapter");
	h.shot("13b-settings-network");
	h.press(P::Down, 6);
	h.press(P::Down, 6);
	// The first name server: PS2 Online's to begin with; the console's own, and back.
	h.press(P::Cross, 20);
	h.shot("13b2-name-server-presets");
	h.press(P::Down, 4);
	h.press(P::Cross, 20);
	g_check.expect(b.getString(0, "DEV9/Eth", "ModeDNS1").value_or("") == "Auto", "the console's own name server can be chosen");
	h.press(P::Cross, 20);
	h.press(P::Up, 4);
	h.press(P::Cross, 20);
	g_check.expect(b.getString(0, "DEV9/Eth", "ModeDNS1").value_or("") == "Manual" && b.getString(0, "DEV9/Eth", "DNS1").value_or("") == "45.7.228.197",
		"and the service's again");
	// An address of one's own, changed number by number.
	h.press(P::Cross, 20);
	h.press(P::Down, 4);
	h.press(P::Down, 4);
	h.press(P::Cross, 30);
	h.press(P::Up, 4);
	h.press(P::Right, 4);
	h.press(P::R1, 4);
	h.wait(20);
	h.shot("13c-settings-address");
	h.press(P::Cross, 30);
	g_check.expect(b.addresses == 1 && b.getString(0, "DEV9/Eth", "DNS1").value_or("") == "46.17.228.197", "an address is changed number by number");
	h.shot("13d-settings-address-set");
	h.press(P::Circle);
	// RetroAchievements: signed in to on the keyboard, a name and then a password.
	go("Achievements", "Account");
	h.shot("13e-settings-achievements");
	h.press(P::Down, 6);
	h.press(P::Cross, 30);
	h.shot("13f-keyboard");
	h.press(P::Down, 4);  // from q down to a
	h.press(P::Cross, 4); // a
	h.press(P::L1, 4);    // capitals
	h.press(P::Right, 4);
	h.press(P::Cross, 4); // S
	h.press(P::R1, 4);    // signs
	h.press(P::Cross, 4); // ]  (the key under the directions, on the page of signs)
	h.press(P::Square, 4); // ...and away again
	h.press(P::R1, 4);
	h.press(P::Triangle, 4); // a space
	h.press(P::Square, 4);
	h.wait(10);
	h.shot("13g-keyboard-typed");
	h.press(P::Options, 30);
	for (int i = 0; i < 5; i++)
		h.press(P::Cross, 4); // qqqqq
	h.wait(10);
	h.shot("13h-keyboard-password");
	h.press(P::L2, 10);
	h.shot("13i-keyboard-password-shown");
	// Down to the wide keys and along to Done.
	for (int i = 0; i < 3; i++)
		h.press(P::Down, 4);
	for (int i = 0; i < 4; i++)
		h.press(P::Right, 4);
	h.press(P::Cross, 30);
	g_check.expect(b.signedIn == "aS", "a name and a password typed on the keyboard sign in");
	h.shot("13j-achievements-signed-in");
	h.press(P::Circle);
	go("Controllers", "Controller");
	h.press(P::Down);
	h.press(P::Down);
	h.shot("14-settings-controllers");
	h.press(P::Cross, 30);
	h.shot("15-settings-press-a-button");
	h.wait(60);
	h.wait(3, P::Square);
	h.wait(30);
	h.press(P::Circle);
	// The log: its end first, then up through it, and its start.
	go("Debug", "This run");
	h.shot("15b-settings-log");
	h.press(P::Cross, 40);
	g_check.expect(b.logsShown == 1, "a log is opened by its row");
	h.shot("15c-log-end");
	h.press(P::L1, 20);
	h.press(P::Up, 20);
	h.shot("15d-log-page-up");
	h.press(P::L2, 30);
	h.shot("15e-log-start");
	h.press(P::Circle, 20);
	g_check.expect(!bp::dialogOpen(), "Circle closes the log");
	h.press(P::Down, 6);
	h.press(P::Cross, 30);
	h.shot("15f-log-empty");
	h.press(P::Circle, 20);
	h.press(P::Circle);
	h.press(P::Circle, 40);

	// A game with a state from when it was closed: go on, or start over. (The shelf of the games
	// last played, whose first is the one with such a state.)
	h.press(P::Up, 30);
	h.press(P::Up, 30);
	for (int i = 0; i < 6; i++)
		h.press(P::Left, 4);
	h.wait(30);
	h.press(P::Cross, 20);
	h.shot("16-resume-or-start-over");
	h.press(P::Circle, 20);

	// Its own options.
	h.press(P::Triangle, 20);
	h.press(P::Right);
	h.press(P::Right);
	h.press(P::Cross, 40);
	h.shot("17-game-options");
	// What the database recommends for it: set, and said.
	h.press(P::Cross, 10);
	for (int i = 0; i < 12 && !b.recommended; i++)
	{
		h.press(P::Down, 6);
		h.press(P::Cross, 20);
		if (!b.recommended && bp::dialogOpen())
			h.press(P::Circle, 10); // (the question of another row: left unanswered)
	}
	g_check.expect(b.recommended, "the Recommended row is reached and sets the game's own option");
	h.wait(20);
	h.shot("17b-game-recommended");
	h.press(P::Cross, 20);
	h.shot("17c-game-recommended-done");
	h.press(P::Circle, 20);
	go("Graphics", "Texture filtering");
	h.press(P::Right);
	h.press(P::Down);
	h.press(P::Right);
	h.press(P::Right);
	h.wait(20);
	h.shot("18-game-options-picture");
	h.press(P::Circle);
	go("Patches");
	h.press(P::Cross);
	h.press(P::Cross);
	h.press(P::Down);
	h.shot("19-game-options-patches");
	h.press(P::Circle);
	h.press(P::Circle, 30);
	h.press(P::Circle, 30);

	// A game from the network: the loading screen with its progress.
	h.press(P::R1, 30);
	h.press(P::R1, 30);
	h.press(P::Up, 20);
	h.press(P::Cross, 2);
	h.wait(100);
	h.shot("20-loading");
	h.wait(200);
	// The quick menu.
	bp::openQuickMenu();
	h.wait(40);
	h.shot("21-quick-menu");
	h.press(P::Down);
	h.press(P::Cross, 60);
	bp::openQuickMenu();
	h.wait(30);
	h.press(P::Down);
	h.press(P::Down);
	h.press(P::Down);
	h.press(P::Right);
	h.press(P::Left);
	h.shot("22-quick-menu-state-slot");
	h.press(P::Down);
	h.press(P::Cross, 50);
	h.shot("23-change-disc");
	h.press(P::Circle, 30);
	// The game's patches and cheats, straight from the quick menu: its options open on that page,
	// with the focus in its rows; leaving them is back in the quick menu.
	for (int i = 0; i < 2; i++)
		h.press(P::Down, 4);
	h.wait(10);
	h.press(P::Cross, 40);
	h.shot("23b-patches-and-cheats-from-the-quick-menu");
	g_check.expect(bp::screen == bp::Screen::Settings && b.gameOpen, "Patches and cheats in the quick menu opens the game's options");
	h.press(P::Circle, 20);
	h.press(P::Circle, 30);
	g_check.expect(bp::screen == bp::Screen::Quick && !b.gameOpen, "and leaving them is back in the quick menu");
	h.press(P::Down, 4);
	h.wait(10);
	h.press(P::Cross, 40);
	h.shot("24-settings-over-the-game");
	h.press(P::Circle, 30);
	bp::message("The state could not be loaded", "It was made by another version of Crux2 (an invented reason).");
	bp::toast("Saved to slot 1.");
	h.wait(30);
	h.shot("25-message-and-toast");
	h.press(P::Cross, 20);
	// Settings, Fast forward, Restart game, Quit game.
	for (int i = 0; i < 3; i++)
		h.press(P::Down, 4);
	h.wait(20);
	h.press(P::Cross, 20);
	h.shot("26-quit-game");
	h.press(P::Down);
	h.press(P::Cross, 4);
	h.shot("26b-closing");
	h.wait(60);
	h.shot("27-back-in-the-library");
	// Cross and Circle exchanged (Settings > Interface): Cross is the way back, which in the
	// library asks whether to quit, and Circle chooses, here the answer the question opens on.
	{
		b.setBool(bp::Shared, "PS5UI", "SwapConfirm", true);
		bp::loadOptions();
		h.press(P::Cross, 20);
		g_check.expect(bp::dialogOpen() && bp::screen == bp::Screen::Library, "with the buttons exchanged, Cross asks whether to quit");
		h.shot("27b-buttons-exchanged");
		h.press(P::Circle, 20);
		g_check.expect(!bp::dialogOpen() && bp::screen == bp::Screen::Library && !b.run.valid, "and Circle chooses the answer");
		b.setBool(bp::Shared, "PS5UI", "SwapConfirm", false);
		bp::loadOptions();
		h.press(P::Circle, 20);
		g_check.expect(bp::dialogOpen(), "back as they were, Circle asks whether to quit");
		h.press(P::Cross, 20);
		g_check.expect(!bp::dialogOpen(), "and Cross chooses");
	}

	// A start that fails.
	b.startWillFail = true;
	h.press(P::Cross, 260);
	h.shot("28-start-failed");
	h.press(P::Cross, 20);
	b.startWillFail = false;
	h.press(P::L1, 30);
	h.press(P::L1, 40);

	// ---- the list's menu: a search typed on the keyboard, the order, favourites, hidden games
	h.press(P::Square, 30);
	h.shot("50-list-menu");
	h.press(P::Cross, 30);
	// "neon": n is on the fourth row, e and o on the second (the keyboard opens on q).
	const auto key = [&](int& kx, int& ky, int toX, int toY) {
		while (ky < toY)
		{
			h.press(P::Down, 3);
			ky++;
		}
		while (ky > toY)
		{
			h.press(P::Up, 3);
			ky--;
		}
		while (kx < toX)
		{
			h.press(P::Right, 3);
			kx++;
		}
		while (kx > toX)
		{
			h.press(P::Left, 3);
			kx--;
		}
		h.press(P::Cross, 3);
	};
	{
		int kx = 0, ky = 1;
		key(kx, ky, 5, 3);
		key(kx, ky, 2, 1);
		key(kx, ky, 8, 1);
		key(kx, ky, 5, 3);
	}
	h.wait(10);
	h.shot("51-keyboard-search");
	h.press(P::Options, 40);
	h.shot("52-search-found");
	h.press(P::TouchPad, 40);
	h.shot("52b-search-found-grid");
	for (int i = 0; i < 9; i++)
		h.press(P::TouchPad, 30);
	h.wait(20);
	// A favourite, from the details of the game that was found.
	h.press(P::Triangle, 30);
	h.press(P::L1, 30);
	{
		int favourites = 0;
		for (const bp::GameInfo& g : b.list)
			favourites += g.favourite ? 1 : 0;
		g_check.expect(favourites == 1, "L1 in a game's details makes it a favourite");
	}
	h.shot("53-details-favourite");
	h.press(P::Circle, 20);
	h.press(P::Circle, 40); // the search goes: all games again
	h.shot("54-favourites-shelf");
	// The order: the game last played first.
	h.press(P::Square, 20);
	h.shot("54b-list-menu");
	h.press(P::Down, 4);
	h.press(P::Down, 4);
	h.press(P::Cross, 20);
	h.shot("55-order");
	h.press(P::Down, 4);
	h.press(P::Cross, 30);
	g_check.expect(b.getInt(0, "PS5UI", "Sort").value_or(0) == 1, "the order chosen is kept");
	h.press(P::TouchPad, 40);
	h.shot("55b-grid-last-played-first");
	for (int i = 0; i < 4; i++)
		h.press(P::TouchPad, 30);
	h.shot("55c-table-last-played-first");
	for (int i = 0; i < 5; i++)
		h.press(P::TouchPad, 30);
	h.press(P::Square, 20);
	h.press(P::Down, 4);
	h.press(P::Down, 4);
	h.press(P::Cross, 20);
	h.press(P::Up, 4);
	h.press(P::Cross, 30);
	g_check.expect(b.getInt(0, "PS5UI", "Sort").value_or(0) == 0, "and changed back");

	// The quick views: R3 goes through the three that are there to begin with, and back to all.
	{
		const auto shown = [&] {
			h.press(P::TouchPad, 30); // (the grid says how many games there are)
			h.press(P::TouchPad, 30);
		};
		(void)shown;
		h.press(P::R3, 40);
		h.shot("55d-quick-view-favourites");
		h.press(P::R3, 40);
		g_check.expect(b.getInt(0, "PS5UI", "Sort").value_or(0) == 1, "the quick view \"Last played\" brings its order");
		h.shot("55e-quick-view-last-played");
		h.press(P::R3, 40);
		h.shot("55f-quick-view-never-played");
		h.press(P::R3, 40);
		g_check.expect(b.getInt(0, "PS5UI", "Sort").value_or(0) == 0, "after the last quick view the order is the one from before");
		// Filters, set by hand: a region, and only games with saved states.
		h.press(P::Square, 20);
		for (int i = 0; i < 4; i++)
			h.press(P::Down, 4);
		h.press(P::Cross, 20);
		h.shot("55g-filters");
		h.press(P::Cross, 20);
		h.press(P::Down, 4);
		h.press(P::Cross, 20); // America
		for (int i = 0; i < 3; i++)
			h.press(P::Down, 4);
		h.press(P::Cross, 20); // with saved states
		h.shot("55h-filters-set");
		h.press(P::Circle, 30);
		h.shot("55i-filtered");
		// Kept as a quick view of its own.
		h.press(P::Square, 20);
		h.press(P::Down, 4);
		h.press(P::Cross, 20);
		h.shot("55j-quick-views");
		for (int i = 0; i < 3; i++)
			h.press(P::Down, 4);
		h.press(P::Cross, 20);
		{
			int kx = 0, ky = 1;
			key(kx, ky, 3, 2);
			key(kx, ky, 5, 3);
		}
		h.press(P::Options, 30);
		g_check.expect(b.getString(0, "PS5UI", "QuickView4").has_value(), "what is shown is kept as a fourth quick view");
		h.press(P::Circle, 40);
		g_check.expect(b.getInt(0, "PS5UI", "Sort").value_or(0) == 0, "Circle shows all games again");
		// A collection: made from a game's details, and a shelf of its own.
		h.press(P::Triangle, 30);
		for (int i = 0; i < 3; i++)
			h.press(P::Right, 4);
		h.press(P::Cross, 20);
		h.press(P::Down, 4);
		h.press(P::Down, 4);
		h.press(P::Cross, 20);
		h.shot("55k-collections-of-a-game");
		h.press(P::Cross, 20);
		{
			int kx = 0, ky = 1;
			key(kx, ky, 2, 3);
			key(kx, ky, 0, 2);
			key(kx, ky, 3, 1);
		}
		h.press(P::Options, 30);
		g_check.expect(!b.getString(0, "PS5UI", "Collections").value_or("").empty() && !b.getString(0, "PS5UI", "Collection1").value_or("").empty(),
			"a collection is made and the game is in it");
		h.press(P::Circle, 30);
		h.shot("55l-collection-shelf");
		// Renamed, and given its own name back.
		h.press(P::Triangle, 30);
		for (int i = 0; i < 3; i++)
			h.press(P::Right, 4);
		h.press(P::Cross, 20);
		h.press(P::Down, 4);
		h.press(P::Cross, 20);
		h.shot("55m-rename");
		{
			int kx = 0, ky = 1;
			key(kx, ky, 9, 1);
		}
		h.press(P::Options, 30);
		{
			int renamed = 0;
			for (const bp::GameInfo& g : b.list)
				renamed += g.renamed ? 1 : 0;
			g_check.expect(renamed == 1, "More > Rename gives the game another title");
		}
		h.press(P::Cross, 20); // More again: its own name is the fifth row now
		for (int i = 0; i < 4; i++)
			h.press(P::Down, 4);
		h.press(P::Cross, 20);
		{
			int renamed = 0;
			for (const bp::GameInfo& g : b.list)
				renamed += g.renamed ? 1 : 0;
			g_check.expect(renamed == 0, "and \"Its own name again\" takes it back");
		}
		h.press(P::Circle, 30);
		// A game at random: its details open.
		h.press(P::Square, 20);
		for (int i = 0; i < 7; i++)
			h.press(P::Down, 4);
		h.press(P::Cross, 40);
		h.shot("55n-random-game");
		h.press(P::Circle, 30);
	}
	// A game hidden, found among the hidden ones, and listed again.
	h.press(P::Triangle, 30);
	h.press(P::R1, 30);
	h.shot("56-hide-question");
	h.press(P::Left, 6);
	h.press(P::Cross, 40);
	{
		int hidden = 0;
		for (const bp::GameInfo& g : b.list)
			hidden += g.hidden ? 1 : 0;
		g_check.expect(hidden == 1, "R1 in a game's details, answered yes, hides it");
	}
	h.press(P::Square, 20);
	for (int i = 0; i < 3; i++)
		h.press(P::Down, 4);
	h.press(P::Cross, 20);
	h.press(P::Down, 4);
	h.press(P::Down, 4);
	h.press(P::Cross, 40);
	h.shot("57-hidden-games");
	h.press(P::Triangle, 30);
	h.press(P::R1, 40);
	{
		int hidden = 0;
		for (const bp::GameInfo& g : b.list)
			hidden += g.hidden ? 1 : 0;
		g_check.expect(hidden == 0, "R1 on a hidden game lists it again");
	}
	h.shot("57b-none-hidden");
	h.press(P::Circle, 40);

	// ---- a game with achievements, and a load to take back
	b.undo = true;
	b.cheevos = 3;
	// (The game played last has a state from when it was closed: it is started from its beginning.)
	h.press(P::Cross, 20);
	h.press(P::Down, 4);
	h.press(P::Cross, 2);
	h.wait(320);
	bp::openQuickMenu();
	h.wait(40);
	h.shot("58-quick-menu-more");
	// (This game has a state saved by itself: "Load the auto-save" is the row after the slot's.)
	for (int i = 0; i < 4; i++)
		h.press(P::Down, 4);
	h.press(P::Cross, 30);
	g_check.expect(b.loadedSlot == -2 && !bp::active(), "Load the auto-save loads the state saved by itself, and the game goes on");
	bp::openQuickMenu();
	h.wait(30);
	for (int i = 0; i < 5; i++)
		h.press(P::Down, 4);
	h.press(P::Cross, 30);
	g_check.expect(b.undone == 1 && !bp::active(), "Undo the last load is run, and the game goes on");
	bp::openQuickMenu();
	h.wait(30);
	for (int i = 0; i < 6; i++)
		h.press(P::Down, 4);
	h.press(P::Cross, 20);
	g_check.expect(b.overlay == 1 && bp::active(), "Achievements opens the emulator's list, and the game stays held");
	// While that list is up the screens draw nothing and take no button.
	h.press(P::Cross, 10);
	h.press(P::Down, 10);
	g_check.expect(b.overlay == 1, "the screens leave the controller to the list");
	b.overlay = 0;
	h.wait(20);
	h.press(P::Down, 4);
	h.press(P::Cross, 20);
	g_check.expect(b.overlay == 2, "and Leaderboards its leaderboards");
	b.overlay = 0;
	h.wait(20);
	// Out of the game again.
	for (int i = 0; i < 8; i++)
		h.press(P::Up, 4);
	h.wait(10);
	h.press(P::Cross, 20);
	h.press(P::Down);
	h.press(P::Cross, 4);
	h.wait(80);
	b.undo = false;
	b.cheevos = 0;

	// The skins.
	for (int skin = 1; skin < 5; skin++)
	{
		b.setInt(0, "PS5UI", "Look", skin);
		h.press(P::Options, 30);
		h.press(P::Circle, 50);
		char name[32];
		std::snprintf(name, sizeof(name), "3%d-skin", skin);
		h.shot(name);
	}

	// The thirty skins of the PS5 homebrew UI kit: the library in each, and the settings in a few of
	// the light ones (whose lines are dark), with larger text in one.
	for (int skin = 6; skin < bp::SkinCount; skin++)
	{
		b.setInt(0, "PS5UI", "Look", skin);
		h.press(P::Options, 30);
		h.press(P::Circle, 50);
		char name[48];
		std::snprintf(name, sizeof(name), "35-kit-skin-%02d-%s", skin, bp::skins[skin].name);
		h.shot(name);
		if (bp::skins[skin].light && (skin % 4) == 0)
		{
			h.press(P::Options, 40);
			go("Interface", "Skin");
			std::snprintf(name, sizeof(name), "35-kit-skin-%02d-settings", skin);
			h.shot(name);
			h.press(P::Circle);
			h.press(P::Circle, 40);
		}
	}
	b.setInt(0, "PS5UI", "TextSize", 2);
	b.setInt(0, "PS5UI", "Look", 0);
	h.press(P::Options, 40);
	go("Emulation", "Frame pacing");
	h.shot("36-largest-text");
	h.press(P::Circle);
	h.press(P::Circle, 40);
	b.setInt(0, "PS5UI", "TextSize", 0);

	// ---- pictures behind the screens: one's own, and the focused game's
	b.setInt(0, "PS5UI", "Look", 0);
	b.setInt(0, "PS5UI", "Backdrop", static_cast<int>(bp::BackOwn) + 1);
	b.setInt(0, "PS5UI", "WallEvery", 10);
	h.press(P::Options, 30);
	h.press(P::Circle, 50);
	h.wait(150);
	h.shot("45-background-my-pictures");
	// (Ten seconds each: the next one fades in over this one.)
	{
		const int left = 600 - static_cast<int>(std::fmod(bp::timeNow, 10.0) * 60.0);
		h.wait(left + 45);
		h.shot("45b-background-the-next-fading-in");
	}
	b.setInt(0, "PS5UI", "Backdrop", static_cast<int>(bp::BackPlay) + 1);
	h.press(P::Options, 30);
	h.press(P::Circle, 50);
	h.press(P::TouchPad, 60);
	h.wait(60);
	h.shot("46-background-the-games-picture");
	for (int i = 0; i < 9; i++)
		h.press(P::TouchPad, 20);
	b.setInt(0, "PS5UI", "Backdrop", 0);
	b.setBool(0, "PS5UI", "GreyBars", true);
	b.setBool(0, "PS5UI", "OsdMove", true);
	b.setInt(0, "PS5UI", "GameDim", 5);
	h.press(P::Options, 30);
	h.press(P::Circle, 50);

	// ---- care for the screen: nothing pressed for a while
	bp::careSetIdle(16);
	h.wait(150);
	h.shot("47-hints-faded");
	g_check.expect(bp::steadyAlpha() < 0.3f, "the hints go faint when nothing is pressed");
	bp::careSetIdle(181);
	h.wait(240);
	h.shot("48-gone-dark");
	g_check.expect(!bp::careAsleep(), "dark is not yet the screen saver");
	bp::careSetIdle(361);
	h.wait(300);
	h.shot("49-screen-saver");
	g_check.expect(bp::careAsleep(), "after twice the time the screen saver is up");
	h.press(P::Cross, 30);
	g_check.expect(!bp::careAsleep() && !b.starting() && !b.run.valid && !bp::dialogOpen() && bp::steadyAlpha() > 0.99f,
		"the press that ends the screen saver does nothing else");
	h.shot("49b-awake-again");
	// Under a game: dark grey bars beside its picture, the figures sent to a corner, and the
	// picture darkened when nothing was pressed for long.
	b.start(b.list[3], INT_MIN, 0);
	h.wait(360);
	g_check.expect(b.run.valid && bp::screen == bp::Screen::None, "a game runs with no screen over it");
	g_check.expect(b.corner >= 0, "the figures are sent to a corner");
	h.shot("49c-game-with-grey-bars");
	// The on-screen display's panel (v1.0.2), with made-up figures, over the game.
	{
		static float history[150];
		for (int i = 0; i < 150; i++)
			history[i] = 16.7f + 2.0f * std::sin(i * 0.7f) + ((i % 37) == 0 ? 14.0f : 0.0f);
		g_overlay = [] {
			using namespace PS5OsdDraw;
			Panel p;
			const auto add = [&p](const char* n, const char* v, const char* u, ImU32 nc, ImU32 vc) { p.rows.push_back({n, v, u, nc, vc}); };
			add("GPU", "71", "%", GPU_NAME, HOT_VALUE);
			add("time", "11.8", "ms", GPU_NAME, HOT_VALUE);
			p.sections.push_back(p.rows.size());
			add("EE", "48", "%", CPU_NAME, HOT_VALUE);
			add("GS", "86", "%", CPU_NAME, HOT_VALUE);
			add("VU", "62", "%", CPU_NAME, HOT_VALUE);
			add("temp", "61", "\xc2\xb0" "C", CPU_NAME, HOT_VALUE);
			add("clock", "3500", "MHz", CPU_NAME, HOT_VALUE);
			add("RAM", "7387", "MB", CPU_NAME, HOT_VALUE);
			p.sections.push_back(p.rows.size());
			add("Vulkan", "60", "FPS", FRAME_NAME, COOL_VALUE);
			add("Frametime", "16.7", "ms", FRAME_NAME, COOL_VALUE);
			add("Max", "62", "FPS", FRAME_NAME, COOL_VALUE);
			add("Average", "59", "FPS", FRAME_NAME, COOL_VALUE);
			add("Min", "32", "FPS", FRAME_NAME, COOL_VALUE);
			add("1% Low", "43", "FPS", FRAME_NAME, COOL_VALUE);
			add("0.1% Low", "32", "FPS", FRAME_NAME, COOL_VALUE);
			p.graph = true;
			p.history = history;
			p.history_size = 150;
			const ImGuiIO& io = ImGui::GetIO();
			const float scale = io.DisplaySize.y / 1080.0f;
			PS5OsdDraw::Draw(ImGui::GetForegroundDrawList(), bp::brand(), std::round(15.0f * 1.3f * scale * 1.6f), p, 3, scale,
				std::round(10.0f * scale), std::round(10.0f * scale), io.DisplaySize.x, io.DisplaySize.y);
		};
		h.wait(2);
		h.shot("49c2-osd-panel");
		g_overlay = nullptr;
	}
	bp::careSetIdle(301);
	h.wait(240);
	h.shot("49d-game-darkened");
	h.wait(4, P::Left);
	h.wait(30);
	bp::openQuickMenu();
	h.wait(40);
	// The screen saver over the paused game: the quick menu's own button ends it, and the menu
	// stays up.
	bp::careSetIdle(361);
	h.wait(300);
	g_check.expect(bp::careAsleep(), "the screen saver comes over a paused game too");
	h.press(P::Options, 30);
	g_check.expect(!bp::careAsleep() && bp::screen == bp::Screen::Quick && b.run.paused,
		"the quick menu's button ends the screen saver and leaves the menu up");
	h.wait(100);
	h.press(P::Up, 14); // (from Resume round to the last row: Quit game)
	h.press(P::Cross, 20);
	h.press(P::Down);
	h.press(P::Cross, 4);
	h.wait(80);
	g_check.expect(!b.run.valid, "the game is closed again");
	b.setBool(0, "PS5UI", "GreyBars", false);
	b.setBool(0, "PS5UI", "OsdMove", false);
	b.setInt(0, "PS5UI", "GameDim", 0);

	// The graphics device made anew under the settings (a setting asked for another renderer):
	// the page is still there.
	b.setInt(0, "PS5UI", "Look", 0);
	h.press(P::Options, 40);
	h.press(P::Down);
	h.press(P::Down);
	h.press(P::Cross, 30);
	remakeContext();
	h.wait(40);
	h.shot("40-settings-after-the-device-was-made-anew");
	h.press(P::Down, 20);
	h.shot("41-and-a-press-later");
}

// Every row of every settings page, changed in every way a controller can; then a long time of
// random buttons across all the screens.
void crawl(Harness& h, unsigned seed)
{
	PreviewBackend& b = h.backend;
	h.wait(200);

	const auto walk = [&](const char* what) {
		// Through the categories: into each, down every row, with every button on each.
		for (int category = 0; category < 16; category++)
		{
			h.press(P::Cross, 3);
			for (int row = 0; row < 90; row++)
			{
				h.press(P::Right, 2);
				h.press(P::Left, 2);
				h.press(P::Left, 2);
				h.press(P::Square, 2);
				h.press(P::Cross, 3);
				// (A list or a question may be up: one choice down and taken, or left.)
				h.press(P::Down, 2);
				h.press((row % 2) ? P::Cross : P::Circle, 3);
				h.press(P::Circle, 2);
				h.press(P::Cross, 2);
				h.press(P::Circle, 2);
				h.wait(b.listening() > 0 ? 320 : 0);
				// Back into the rows if that left them, and on.
				h.press(P::Cross, 2);
				h.press(P::Circle, 2);
				h.press(P::Right, 2);
				h.press(P::Down, 2);
			}
			h.press(P::Circle, 3);
			h.press(P::Circle, 0);
			if (bp::active() && b.listening() <= 0)
				h.press(P::Down, 3);
		}
		std::printf("crawl: %s walked, %llu values written\n", what, b.writes);
	};

	h.press(P::Options, 20);
	walk("the settings of all games");
	// Out of the settings, whatever is up.
	for (int i = 0; i < 6; i++)
		h.press(P::Circle, 4);
	h.press(P::Options, 10);
	h.press(P::Options, 10);

	std::mt19937 rng(seed);
	const uint32_t buttons[] = {P::Up, P::Down, P::Left, P::Right, P::Cross, P::Cross, P::Circle, P::Square, P::Triangle, P::L1, P::R1,
		P::L2, P::R2, P::Options, P::TouchPad, P::R3};
	unsigned started = 0, menus = 0;
	for (int step = 0; step < 60000; step++)
	{
		uint32_t held = buttons[rng() % (sizeof(buttons) / sizeof(buttons[0]))];
		if (rng() % 9 == 0)
			held |= buttons[rng() % (sizeof(buttons) / sizeof(buttons[0]))];
		Frame small(480, 270);
		const bool draw = (step % 400 == 0);
		const int hold = 1 + static_cast<int>(rng() % 3);
		for (int i = 0; i < hold; i++)
			h.frame(held, draw ? &small : nullptr, !draw);
		h.wait(static_cast<int>(rng() % 4));
		// A game that runs with no screen over it: the button for the quick menu, now and then.
		if (b.run.valid && !bp::active())
		{
			started++;
			if (rng() % 3 == 0)
			{
				h.frame(P::Options, nullptr, true);
				h.wait(3 + static_cast<int>(rng() % 3));
				menus++;
			}
			else if (rng() % 40 == 0)
			{
				// The machine goes down by itself (an error).
				b.run = bp::Running();
				bp::gameStopped();
			}
		}
		else if (b.run.valid && rng() % 300 == 0)
		{
			// The same, while a screen is over the game.
			b.run = bp::Running();
			bp::gameStopped();
		}
		// How long the machine takes to do what it is asked.
		if (step % 97 == 0)
			b.lag = 1 + static_cast<int>(rng() % 6);
		// The graphics device is made anew, under whatever is on.
		if (step % 4000 == 1999)
			remakeContext();
		// A disc is put in the console's drive, or taken out of it, under whatever is on.
		if (step % 1300 == 650)
			b.putDisc(static_cast<int>(rng() % 3));
		if (step % 700 == 350 && !b.list.empty())
		{
			// A game was played, or the list scanned again: the same games, other times.
			b.list[rng() % b.list.size()].lastPlayed = 1790000000 + static_cast<time_t>(step);
			b.generation++;
		}
		if (step % 5000 == 2500)
		{
			// The world changes under the screens: games come and go, the options are turned.
			b.list.erase(b.list.begin() + static_cast<std::ptrdiff_t>(rng() % b.list.size()));
			b.generation++;
			b.wantUsb = !b.wantUsb;
			b.busyCard = !b.busyCard;
			b.setInt(0, "PS5UI", "Motion", static_cast<int>(rng() % 3));
			b.setInt(0, "PS5UI", "Backdrop", static_cast<int>(rng() % 10));
			// (And as if the controller had been left alone for some minutes, now and then.)
			if (rng() % 3 == 0)
				bp::careSetIdle(static_cast<float>(rng() % 500));
			b.startWillFail = (rng() % 4 == 0);
			bp::toast("Something happened in the background.");
			// (Over a running game the host says it as a line of the game's own overlay.)
			if (rng() % 2 && bp::active())
				bp::message("A message", "From the emulator, while whatever screen is on.");
		}
	}
	std::printf("crawl: %u frames, %llu triangles, a game ran %u times, the quick menu was opened %u times\n", h.frames, g_triangles, started, menus);
	g_check.expect(g_badTextures == 0, "every draw names a picture that exists");
	g_check.expect(g_badIndices == 0, "every triangle's corners are in its list");
	g_check.expect(started > 20, "the random walk started games");
	g_check.expect(menus > 5, "the random walk opened the quick menu");
	g_check.expect(b.sounded[2], "the library opened, and said so");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s shots <fonts folder> <out folder> | startup <fonts folder> <out folder> <1..4> | crawl <fonts folder> [seed]\n", argv[0]);
		return 2;
	}
	const std::string mode = argv[1];
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	g_fontFolder = argv[2];
	if (!loadFonts(argv[2]))
		return 2;

	Harness h(1920, 1080);
	h.backend.populate();
	// Which start-up plays: the first unless told (the title itself plays another one each start).
	{
		int kind = 1;
		if (mode == "startup" && argc > 4)
			kind = std::clamp(std::atoi(argv[4]), 1, 4);
		else if (mode == "crawl")
			kind = 1 + static_cast<int>((argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1u) % 4);
		h.backend.setInt(0, "PS5UI", "SplashKind", kind);
	}
	bp::initialize(&h.backend);
	if (mode == "shots")
	{
		if (argc < 4)
			return 2;
		h.out = argv[3];
		shots(h);
	}
	else if (mode == "startup")
	{
		// One of the four start-ups, as pictures: startup <fonts> <out> <1..4>
		if (argc < 5)
			return 2;
		h.out = argv[3];
		static const float times[] = {0.4f, 0.9f, 1.14f, 1.3f, 1.75f, 2.3f, 3.4f, 4.25f, 4.6f, 4.85f, 5.3f, 6.2f};
		int at = 0;
		for (const float seconds : times)
		{
			const int frame = 3 + static_cast<int>(seconds * 60 + 0.5f);
			h.wait(frame - at);
			at = frame;
			char name[48];
			std::snprintf(name, sizeof(name), "startup%s-%.2f", argv[4], static_cast<double>(seconds));
			h.shot(name);
		}
		g_check.expect(h.backend.sounded[0] && !h.backend.sounded[1] && h.backend.sounded[2], "a start-up left alone begins and opens the library");
	}
	else
	{
		crawl(h, argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1u);
	}
	bp::shutdown();
	ImGui::DestroyContext();
	if (g_badTextures != 0 || g_badIndices != 0)
		std::fprintf(stderr, "FAILED: %u draws with a picture that is gone, %u triangles out of range\n", g_badTextures, g_badIndices);
	std::printf("preview: %s, %u frames, %u failed\n", mode.c_str(), h.frames, g_check.failed);
	return (g_check.failed == 0 && g_badTextures == 0 && g_badIndices == 0) ? 0 : 1;
}
