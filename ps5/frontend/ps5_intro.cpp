// Crux2 - the startup video (ps5_intro.h). libvpx (BSD-3-Clause, the ports release's,
// ps5/tools/build-deps.sh) decodes the VP9 frames; they are turned into RGBA here.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Made from OGBox's ps5/src/ogbox_intro.cpp (OGBox by Press5elect), as its 5934f6b left it: libvpx
// decodes with threads of its own, row-based threading and its faster loop filter; the frames are
// turned into RGBA by a few threads, a stripe each; 12 frames are kept ahead, and the clock (and the
// sound) waits for the first 8 (ready()). The sound is the interface's to play (ps5_ui_audio.h).

#include "ps5_intro.h"

#include "ps5_priority.h"
#include "ps5_ui_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#ifdef __PROSPERO__
#include "ps5_platform.h"
#define INTRO_LOG(...) ps5px_log("intro: " __VA_ARGS__)
#else
#define INTRO_LOG(...) (std::fprintf(stderr, "intro: " __VA_ARGS__), std::fputc('\n', stderr))
#endif

namespace PS5Intro
{
namespace
{
double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint32_t Rd32(const std::uint8_t* p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t Rd16(const std::uint8_t* p)
{
	return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

bool ReadWhole(const std::string& path, std::vector<std::uint8_t>& out)
{
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	out.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
	const bool ok = size > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
	std::fclose(f);
	return ok;
}

inline std::uint8_t Clamp8(int v)
{
	return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

// I420 (BT.601, studio range: what the encoder was given) into RGBA, rows y0 to y1.
void ToRgba(const vpx_image_t* img, std::uint8_t* out, std::uint32_t w, std::uint32_t y0, std::uint32_t y1)
{
	for (std::uint32_t y = y0; y < y1; y++)
	{
		const std::uint8_t* py = img->planes[VPX_PLANE_Y] + static_cast<std::size_t>(y) * img->stride[VPX_PLANE_Y];
		const std::uint8_t* pu = img->planes[VPX_PLANE_U] + static_cast<std::size_t>(y >> 1) * img->stride[VPX_PLANE_U];
		const std::uint8_t* pv = img->planes[VPX_PLANE_V] + static_cast<std::size_t>(y >> 1) * img->stride[VPX_PLANE_V];
		std::uint8_t* o = out + static_cast<std::size_t>(y) * w * 4;
		for (std::uint32_t x = 0; x < w; x += 2)
		{
			const int u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
			const int r = 409 * v + 128, g = -100 * u - 208 * v + 128, b = 516 * u + 128;
			for (std::uint32_t k = 0; k < 2 && x + k < w; k++)
			{
				const int c = 298 * (py[x + k] - 16);
				o[0] = Clamp8((c + r) >> 8);
				o[1] = Clamp8((c + g) >> 8);
				o[2] = Clamp8((c + b) >> 8);
				o[3] = 255;
				o += 4;
			}
		}
	}
}

constexpr int SLOTS = 12;      // decoded ahead: a fifth of a second at 60 frames a second
constexpr int PREROLL = 8;     // decoded before the clock starts (ready() waits for them)
constexpr int CONVERTERS = 3;  // threads that turn a frame into RGBA, a stripe each (1280 x 720: OGBox's 4 were for 1080p)
constexpr unsigned DECODE_THREADS = 4;
constexpr double FADE = 0.25;  // seconds: the sound's fade when skipped (the interface's SkipStartup)
} // namespace

std::string Folder()
{
#ifdef __PROSPERO__
	return std::string(ps5px_title_dir()) + "/resources/intro";
#else
	return ".";
#endif
}

struct Player::Impl
{
	std::vector<std::uint8_t> file; // the IVF
	struct Packet
	{
		std::size_t offset;
		std::uint32_t size;
		double time; // seconds
	};
	std::vector<Packet> packets;
	std::uint32_t width = 0, height = 0;
	double length = 0.0;

	struct Slot
	{
		std::vector<std::uint8_t> rgba;
		int index = -1; // the frame it holds; -1: free
	};
	Slot slots[SLOTS];
	int shown = -1; // the slot frame() last gave
	std::mutex mu;
	std::condition_variable cv;
	std::thread decoder, sound;
	std::atomic<bool> quit{false};
	std::atomic<int> wanted{0};   // the frame the clock is at: frames before it are not converted
	std::atomic<int> decoded{-1}; // the newest frame decoded and converted
	std::atomic<bool> done{false}; // the decoder is through
	std::uint64_t serial = 0;

	// The conversion's helpers: each frame, stripes 1..CONVERTERS-1 (the decoder does stripe 0).
	std::thread converters[CONVERTERS - 1];
	std::mutex cmu;
	std::condition_variable cgo, cdone;
	const vpx_image_t* cimg = nullptr;
	std::uint8_t* cout = nullptr;
	std::uint64_t cjob = 0;
	int cleft = 0;
	bool cquit = false;

	double startedAt = -1.0;
	double skippedAt = -1.0;

	void converter(int stripe)
	{
		std::uint64_t seen = 0;
		for (;;)
		{
			std::unique_lock<std::mutex> lock(cmu);
			cgo.wait(lock, [&] { return cquit || cjob != seen; });
			if (cquit)
				return;
			seen = cjob;
			const vpx_image_t* img = cimg;
			std::uint8_t* out = cout;
			lock.unlock();
			convertStripe(img, out, stripe);
			lock.lock();
			if (--cleft == 0)
				cdone.notify_one();
		}
	}

	void convertStripe(const vpx_image_t* img, std::uint8_t* out, int stripe)
	{
		const std::uint32_t h = std::min<std::uint32_t>(img->d_h, height), w = std::min<std::uint32_t>(img->d_w, width);
		const std::uint32_t rows = ((h + CONVERTERS - 1) / CONVERTERS + 1) & ~1u; // even: a chroma row is two
		const std::uint32_t y0 = std::min(h, rows * static_cast<std::uint32_t>(stripe)), y1 = std::min(h, y0 + rows);
		ToRgba(img, out, w, y0, y1);
	}

	void convert(const vpx_image_t* img, std::uint8_t* out)
	{
		{
			const std::lock_guard<std::mutex> lock(cmu);
			cimg = img;
			cout = out;
			cleft = CONVERTERS - 1;
			cjob++;
		}
		cgo.notify_all();
		convertStripe(img, out, 0);
		std::unique_lock<std::mutex> lock(cmu);
		cdone.wait(lock, [&] { return cleft == 0; });
	}

	bool parseIvf()
	{
		if (file.size() < 32 || std::memcmp(file.data(), "DKIF", 4) != 0 || std::memcmp(file.data() + 8, "VP90", 4) != 0)
			return false;
		const std::uint32_t header = Rd16(file.data() + 6);
		width = Rd16(file.data() + 12);
		height = Rd16(file.data() + 14);
		const std::uint32_t den = Rd32(file.data() + 16), num = Rd32(file.data() + 20);
		if (!width || !height || !den || !num || width > 4096 || height > 4096)
			return false;
		for (std::size_t at = header; at + 12 <= file.size();)
		{
			const std::uint32_t size = Rd32(file.data() + at);
			const std::uint64_t pts = Rd32(file.data() + at + 4) | static_cast<std::uint64_t>(Rd32(file.data() + at + 8)) << 32;
			if (size > file.size() - at - 12)
				break;
			packets.push_back({at + 12, size, static_cast<double>(pts) * num / den});
			at += 12 + static_cast<std::size_t>(size);
		}
		if (packets.empty())
			return false;
		length = packets.back().time + (packets.size() > 1 ? packets.back().time - packets[packets.size() - 2].time : 0.0);
		return true;
	}

	// The sound: any file the interface's sound reads (Ogg Vorbis here), brought to 16-bit stereo
	// at 48 kHz and to the start-up sound's level, then handed to the interface's sound.
	void loadSound(std::string path)
	{
		PS5UIAudio::DSP::Sound sound;
		PS5UIAudio::DSP::FileInfo info;
		std::string error;
		auto stereo = std::make_shared<std::vector<PS5UIAudio::s16>>();
		PS5UIAudio::DSP::StartupInfo made;
		if (!PS5UIAudio::DSP::FileExists(path.c_str()))
		{
			INTRO_LOG("no sound (%s is missing): the video plays without it", path.c_str());
			return;
		}
		const bool loaded = PS5UIAudio::DSP::LoadSound(path.c_str(), 30.0, &sound, &info, &error);
		// (The reader leaves out a lossy file's quiet lead-in, up to 0.15 s, for the music's loops:
		// here it is kept, or the sound would run ahead of the picture by that much.)
		sound.frames += sound.first;
		sound.first = 0;
		if (!loaded || !PS5UIAudio::DSP::RenderPlain(sound, 30.0, 0.0, stereo.get(), &made))
		{
			INTRO_LOG("its sound cannot be used (%s): the video plays without it", error.empty() ? "nothing to hear" : error.c_str());
			return;
		}
		INTRO_LOG("sound: %s, %u Hz, %.2f s", info.format.c_str(), info.rate, made.seconds);
		PS5UIAudio::SetIntroSound(std::move(stereo));
	}

	void decode()
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Interface);
		vpx_codec_ctx_t codec;
		vpx_codec_dec_cfg_t cfg{};
		cfg.threads = DECODE_THREADS;
		cfg.w = width;
		cfg.h = height;
		if (vpx_codec_dec_init(&codec, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK)
		{
			INTRO_LOG("the VP9 decoder cannot start: no video");
			done.store(true);
			return;
		}
		// row-based threads (within a tile column too) and the faster loop filter
		vpx_codec_control(&codec, VP9D_SET_ROW_MT, 1);
		vpx_codec_control(&codec, VP9D_SET_LOOP_FILTER_OPT, 1);
		for (int c = 1; c < CONVERTERS; c++)
			converters[c - 1] = std::thread([this, c] { converter(c); });
		unsigned late = 0, frames = 0;
		double decodeTime = 0.0, convertTime = 0.0, slowest = 0.0;
		for (std::size_t i = 0; i < packets.size() && !quit.load(); i++)
		{
			const double t0 = Now();
			if (vpx_codec_decode(&codec, file.data() + packets[i].offset, packets[i].size, nullptr, 0) != VPX_CODEC_OK)
			{
				INTRO_LOG("frame %zu cannot be decoded (%s): the video ends there", i, vpx_codec_error(&codec));
				break;
			}
			vpx_codec_iter_t iter = nullptr;
			const vpx_image_t* img = vpx_codec_get_frame(&codec, &iter);
			if (!img || img->fmt != VPX_IMG_FMT_I420)
				continue;
			const double t1 = Now();
			decodeTime += t1 - t0;
			if (static_cast<int>(i) < wanted.load() && i + 1 < packets.size())
			{
				late++; // behind the clock: decoded (the next needs it), not shown
				continue;
			}
			std::unique_lock<std::mutex> lock(mu);
			Slot* free = nullptr;
			cv.wait(lock, [&] {
				for (Slot& slot : slots)
				{
					if (slot.index < 0)
					{
						free = &slot;
						return true;
					}
				}
				return quit.load();
			});
			if (quit.load() || !free)
				break;
			lock.unlock();
			free->rgba.resize(static_cast<std::size_t>(width) * height * 4);
			const double t2 = Now();
			convert(img, free->rgba.data());
			const double t3 = Now();
			convertTime += t3 - t2;
			slowest = std::max(slowest, (t1 - t0) + (t3 - t2));
			frames++;
			lock.lock();
			free->index = static_cast<int>(i);
			decoded.store(static_cast<int>(i));
		}
		{
			const std::lock_guard<std::mutex> lock(cmu);
			cquit = true;
		}
		cgo.notify_all();
		for (std::thread& t : converters)
		{
			if (t.joinable())
				t.join();
		}
		vpx_codec_destroy(&codec);
		done.store(true);
		INTRO_LOG("decoding %.1f ms a frame, turning into RGBA %.1f ms (%d threads), the slowest frame %.1f ms; "
				  "%u frames shown of %zu, %u decoded behind the clock (not shown)",
			1000.0 * decodeTime / static_cast<double>(std::max<std::size_t>(1, packets.size())),
			1000.0 * convertTime / std::max(1u, frames), CONVERTERS, 1000.0 * slowest, frames, packets.size(), late);
	}
};

Player::Player() : d(new Impl) {}

Player::~Player()
{
	close();
}

bool Player::open(const std::string& folder)
{
	const std::string video = folder + "/intro.ivf";
	if (!ReadWhole(video, d->file) || !d->parseIvf())
	{
		INTRO_LOG("no video (%s %s): the start-up animation instead", video.c_str(), d->file.empty() ? "missing" : "unreadable");
		d->file.clear();
		d->packets.clear();
		return false;
	}
	INTRO_LOG("%ux%u, %zu frames, %.2f s", d->width, d->height, d->packets.size(), d->length);
	PS5UIAudio::IntroComing();
	d->sound = std::thread([impl = d.get(), path = folder + "/intro.ogg"] { impl->loadSound(path); });
	d->decoder = std::thread([impl = d.get()] { impl->decode(); });
	return true;
}

bool Player::ready() const
{
	return d->file.empty() || d->done.load() ||
		d->decoded.load() >= std::min<int>(PREROLL, static_cast<int>(d->packets.size())) - 1;
}

void Player::start()
{
	if (d->startedAt >= 0.0 || d->file.empty())
		return;
	d->startedAt = Now();
	PS5UIAudio::PlayIntro();
}

bool Player::started() const
{
	return d->startedAt >= 0.0;
}

double Player::elapsed() const
{
	return d->startedAt < 0.0 ? 0.0 : Now() - d->startedAt;
}

double Player::duration() const
{
	return d->length;
}

bool Player::finished() const
{
	if (d->file.empty())
		return true;
	if (d->skippedAt >= 0.0)
		return Now() - d->skippedAt >= FADE;
	// (A decoder that could not start has nothing to show.)
	if (d->done.load() && d->decoded.load() < 0)
		return true;
	return elapsed() >= d->length;
}

void Player::skip()
{
	if (d->skippedAt < 0.0)
	{
		d->skippedAt = Now();
		PS5UIAudio::SkipStartup();
	}
}

const std::uint8_t* Player::frame(std::uint32_t* w, std::uint32_t* h, std::uint64_t* serial)
{
	if (d->file.empty())
		return nullptr;
	const double t = elapsed();
	int target = 0;
	while (target + 1 < static_cast<int>(d->packets.size()) && d->packets[target + 1].time <= t)
		target++;
	d->wanted.store(target);
	const std::lock_guard<std::mutex> lock(d->mu);
	int best = -1;
	for (int s = 0; s < SLOTS; s++)
	{
		if (d->slots[s].index >= 0 && d->slots[s].index <= target && (best < 0 || d->slots[s].index > d->slots[best].index))
			best = s;
	}
	if (best < 0)
		best = d->shown; // none at the clock yet: the last one stays
	bool freed = false;
	for (int s = 0; s < SLOTS; s++) // what is before the one shown is done with
	{
		if (s != best && d->slots[s].index >= 0 && (best < 0 || d->slots[s].index < d->slots[best].index))
		{
			d->slots[s].index = -1;
			freed = true;
		}
	}
	if (freed)
		d->cv.notify_all();
	if (best < 0 || d->slots[best].index < 0)
		return nullptr;
	if (best != d->shown)
	{
		d->shown = best;
		d->serial++;
	}
	*w = d->width;
	*h = d->height;
	*serial = d->serial;
	return d->slots[best].rgba.data();
}

void Player::close()
{
	if (!d)
		return;
	d->quit.store(true);
	{
		const std::lock_guard<std::mutex> lock(d->mu);
		d->cv.notify_all();
	}
	if (d->decoder.joinable())
		d->decoder.join();
	if (d->sound.joinable())
		d->sound.join();
	for (Impl::Slot& slot : d->slots)
	{
		slot.index = -1;
		std::vector<std::uint8_t>().swap(slot.rgba);
	}
	d->shown = -1;
	// (Its memory goes; finished() says yes from here on.)
	std::vector<std::uint8_t>().swap(d->file);
	d->packets.clear();
}
} // namespace PS5Intro
