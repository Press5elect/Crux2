// Crux2 - the interface's sound, on a PC: the files read, the start-up sound made, the mixer run
// through an output that keeps what it is given; the interface's sounds, the playlist and the
// crossfade between its tracks, and the sounds mixed into a game's stream.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   uiaudio_test <work folder> [a start-up sound of one's own.wav]
//
// The work folder gets the test's own sound files (made here, and by ffmpeg when it is there) and
// what was heard as WAV files, to listen to.
#include "ps5_ui_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace PS5UIAudio;

static int g_failed = 0, g_checks = 0;
#define EXPECT(condition, what) \
	do \
	{ \
		g_checks++; \
		if (!(condition)) \
		{ \
			g_failed++; \
			std::printf("FAILED: %s (line %d)\n", what, __LINE__); \
		} \
	} while (0)

static void writeWav(const std::string& path, const std::vector<s16>& samples, unsigned rate, unsigned channels)
{
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return;
	const std::uint32_t bytes = static_cast<std::uint32_t>(samples.size() * 2);
	const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
	const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f);
	u32(36 + bytes);
	std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16);
	u16(1);
	u16(static_cast<std::uint16_t>(channels));
	u32(rate);
	u32(rate * channels * 2);
	u16(static_cast<std::uint16_t>(channels * 2));
	u16(16);
	std::fwrite("data", 1, 4, f);
	u32(bytes);
	std::fwrite(samples.data(), 2, samples.size(), f);
	std::fclose(f);
}

static std::vector<s16> tone(double hz, double seconds, unsigned rate, unsigned channels, double level = 0.4)
{
	std::vector<s16> out;
	const std::size_t frames = static_cast<std::size_t>(seconds * rate);
	for (std::size_t i = 0; i < frames; i++)
		for (unsigned c = 0; c < channels; c++)
			out.push_back(static_cast<s16>(std::sin(2 * M_PI * hz * (c + 1) * i / rate) * level * 32767));
	return out;
}

// What the mixer is heard through: keeps everything, and takes it as fast as it comes.
struct Tape final : Output
{
	std::mutex mutex;
	std::vector<s16> heard;
	int opens = 0, closes = 0;
	bool refuse = false;
	double speed = 0.0; // > 0: taken at this many times the console's pace (0: as fast as it comes)
	bool open() override
	{
		opens++;
		return !refuse;
	}
	bool write(const s16* frames) override
	{
		{
			const std::lock_guard<std::mutex> lock(mutex);
			heard.insert(heard.end(), frames, frames + BLOCK_FRAMES * 2);
		}
		if (speed > 0.0)
			std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(1e6 * BLOCK_FRAMES / DSP::OUTPUT_RATE / speed)));
		return true;
	}
	void close() override { closes++; }
	std::size_t frames()
	{
		const std::lock_guard<std::mutex> lock(mutex);
		return heard.size() / 2;
	}
	void waitFor(std::size_t more)
	{
		const std::size_t until = frames() + more;
		for (int i = 0; i < 20000 && frames() < until; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
};

static double rms(const std::vector<s16>& v, std::size_t from, std::size_t to)
{
	double sum = 0;
	to = std::min(to, v.size() / 2);
	if (to <= from)
		return 0;
	for (std::size_t i = from; i < to; i++)
		sum += double(v[i * 2]) * v[i * 2] + double(v[i * 2 + 1]) * v[i * 2 + 1];
	return std::sqrt(sum / (2.0 * (to - from))) / 32768.0;
}

// ------------------------------------------------------------------ helpers for the newer parts

static std::shared_ptr<DSP::Sound> soundOf(const std::vector<s16>& interleaved, unsigned rate, unsigned channels)
{
	auto sound = std::make_shared<DSP::Sound>();
	sound->rate = rate;
	const std::size_t frames = interleaved.size() / channels;
	std::vector<s16> stereo(frames * 2);
	for (std::size_t i = 0; i < frames; i++)
	{
		stereo[i * 2] = interleaved[i * channels];
		stereo[i * 2 + 1] = interleaved[i * channels + (channels > 1 ? 1 : 0)];
	}
	sound->append(stereo.data(), frames);
	sound->frames = frames;
	return sound;
}

static float peakOf(const std::vector<float>& v)
{
	float peak = 0.0f;
	for (const float x : v)
		peak = std::max(peak, std::fabs(x));
	return peak;
}

static float clipPeak(const CueBank::Clip& clip)
{
	int peak = 0;
	for (const s16 x : clip.stereo)
		peak = std::max(peak, std::abs(static_cast<int>(x)));
	return peak / 32768.0f;
}

// Renders `seconds` of a mixer into `into`.
static void renderFor(Mixer& mixer, double seconds, std::vector<s16>* into)
{
	const std::size_t blocks = static_cast<std::size_t>(seconds * DSP::OUTPUT_RATE / BLOCK_FRAMES);
	s16 block[BLOCK_FRAMES * 2];
	for (std::size_t b = 0; b < blocks; b++)
	{
		mixer.render(block);
		into->insert(into->end(), block, block + BLOCK_FRAMES * 2);
	}
}

static void makeDir(const std::string& path)
{
	const std::string command = "mkdir -p '" + path + "'";
	if (std::system(command.c_str()) != 0)
		std::printf("  (could not make %s)\n", path.c_str());
}

// ------------------------------------------------------------------ the interface's sounds

static void TestCues(const std::string& work)
{
	const unsigned R = DSP::OUTPUT_RATE;
	// ---- the recordings, and the kit's fallback from one set to the other
	const std::shared_ptr<CueBank> bank = CueBank::Load(work + "/resources/sounds");
	std::printf("  the interface's sounds: %d recordings read, %d refused\n", bank->files, bank->rejected);
	EXPECT(bank->files == 36 && bank->rejected == 0, "the 36 recordings of both sets are read");
	EXPECT(!bank->records(SoundSet::Glass, CueId::FavouriteOn) && bank->find(SoundSet::Glass, CueId::FavouriteOn) == &bank->clips[1][static_cast<unsigned>(CueId::FavouriteOn)],
		"glass has no favourite: the paper set's is played");
	EXPECT(!bank->records(SoundSet::Paper, CueId::Welcome) && bank->find(SoundSet::Paper, CueId::Welcome) == &bank->clips[0][static_cast<unsigned>(CueId::Welcome)],
		"paper has no welcome: the glass set's is played");
	bool all = true;
	for (unsigned c = 0; c < CUE_COUNT; c++)
		for (const SoundSet set : {SoundSet::Glass, SoundSet::Paper})
		{
			const auto* clips = bank->find(set, static_cast<CueId>(c));
			all = all && clips != nullptr && !clips->empty() && !(*clips)[0].tone;
		}
	EXPECT(all, "every one of the seventeen cues has a recording in both sets");
	EXPECT(bank->find(SoundSet::Off, CueId::Select) == nullptr, "Off plays nothing");
	{
		CueBank none;
		none.makeTones();
		const auto* clips = none.find(SoundSet::Glass, CueId::Error);
		EXPECT(clips != nullptr && (*clips)[0].tone && clipPeak((*clips)[0]) > 0.2f && clipPeak((*clips)[0]) < 0.32f, "without recordings, a tone");
	}

	// ---- a cue's level is the recording's times the volume
	{
		CueVoices voices;
		voices.setBank(bank);
		voices.setSet(SoundSet::Glass);
		const float recorded = clipPeak((*bank->find(SoundSet::Glass, CueId::Welcome))[0]);
		for (const float volume : {1.0f, 0.5f})
		{
			voices.setVolume(volume);
			std::vector<float> out(R * 2 * 2, 0.0f);
			EXPECT(voices.post(CueId::Welcome), "a cue is asked for");
			voices.mix(out.data(), R * 2, 2, R);
			const float peak = peakOf(out);
			std::printf("  Welcome at volume %.1f: peak %.3f (the recording's %.3f)\n", static_cast<double>(volume), static_cast<double>(peak),
				static_cast<double>(recorded));
			EXPECT(std::fabs(peak - recorded * volume) < 0.08f * recorded * volume + 0.002f, "a cue is heard at the recording's level times the volume");
		}
		voices.setSet(SoundSet::Off);
		EXPECT(!voices.post(CueId::Select), "with the set Off nothing is asked for");
	}

	// ---- many at once, over loud music: never at the rail, and no more than eight voices
	{
		CueVoices voices;
		voices.setBank(bank);
		voices.setSet(SoundSet::Glass);
		voices.setVolume(1.0f);
		Mixer mixer;
		mixer.setCues(&voices);
		mixer.setVolume(0.8f, 1.0f);
		mixer.setBackground(soundOf(tone(110, 3.0, R, 2, 0.95), R, 2), false);
		mixer.startBackground();
		std::vector<s16> heard;
		renderFor(mixer, 1.6, &heard);
		const CueId loud[12] = {CueId::Welcome, CueId::Launch, CueId::Resume, CueId::Notify, CueId::Saved, CueId::Open,
			CueId::FavouriteOn, CueId::Tab, CueId::Error, CueId::Back, CueId::ModalOpen, CueId::ModalClose};
		for (const CueId cue : loud)
			voices.post(cue);
		renderFor(mixer, 0.02, &heard);
		EXPECT(voices.started == 12 && voices.voices() == CueVoices::MAX_VOICES && voices.stolen == 4,
			"twelve cues at once: eight are heard, the four oldest give way");
		std::printf("  twelve cues at once: %llu started, %u voices, %llu gave way\n", voices.started, voices.voices(), voices.stolen);
		renderFor(mixer, 1.5, &heard);
		int peak = 0;
		bool finite = true;
		for (const s16 v : heard)
		{
			peak = std::max(peak, std::abs(static_cast<int>(v)));
			finite = finite && v > -32768;
		}
		std::printf("  loud music and eight cues: peak %d of 32767\n", peak);
		EXPECT(peak < 32767 && finite, "cues over loud music are bent, never clipped");
		writeWav(work + "/cues-over-loud-music.wav", heard, R, 2);
	}

	// ---- one cue asked for again and again (Focus while scrolling)
	{
		CueVoices voices;
		voices.setBank(bank);
		voices.setSet(SoundSet::Glass);
		std::vector<float> out(BLOCK_FRAMES * 2 * 8, 0.0f);
		for (int i = 0; i < 30; i++)
			voices.post(CueId::Focus);
		voices.mix(out.data(), BLOCK_FRAMES, 2, R);
		EXPECT(voices.started == 1 && voices.rateLimited == 29, "thirty Focus in one frame are one");
		// Sixty a second, a frame apart, for two seconds: thinned to about one every 45 ms, and
		// never more than two playing.
		unsigned most = 0;
		const unsigned long long before = voices.started;
		for (int frame = 0; frame < 120; frame++)
		{
			voices.post(CueId::Focus);
			for (int b = 0; b < 3; b++) // 800 frames: 768 + 32
			{
				std::fill(out.begin(), out.end(), 0.0f);
				voices.mix(out.data(), b < 2 ? 256 : 288, 2, R);
				most = std::max(most, voices.voices());
			}
		}
		const unsigned long long started = voices.started - before;
		std::printf("  Focus sixty times a second for two seconds: %llu heard, at most %u at once\n", started, most);
		EXPECT(started >= 36 && started <= 46 && most <= CueVoices::MAX_SAME, "a scroll's Focus is thinned out and does not pile up");
		// A long cue asked for faster than it ends: no more than two of it at once.
		{
			CueVoices again;
			again.setBank(bank);
			again.setSet(SoundSet::Glass);
			unsigned at_once = 0;
			for (int i = 0; i < 6; i++)
			{
				again.post(CueId::Notify);
				std::fill(out.begin(), out.end(), 0.0f);
				again.mix(out.data(), 1920, 2, R); // 40 ms apart; the recording is 0.76 s
				at_once = std::max(at_once, again.voices());
			}
			std::printf("  Notify six times 40 ms apart: %llu started, at most %u at once, %llu gave way\n", again.started, at_once, again.stolen);
			EXPECT(again.started == 6 && at_once == CueVoices::MAX_SAME && again.stolen == 4, "one cue never takes more than two voices");
		}

		// The variations: at random, never the same twice in a row, and all of them.
		for (const CueId cue : {CueId::Focus, CueId::Slider, CueId::Toggle})
		{
			const unsigned variations = static_cast<unsigned>(bank->find(SoundSet::Glass, cue)->size());
			int previous = -1;
			bool repeated = false;
			unsigned used = 0;
			std::vector<float> tenth(R / 10 * 2);
			for (int i = 0; i < 60; i++)
			{
				voices.post(cue);
				std::fill(tenth.begin(), tenth.end(), 0.0f);
				voices.mix(tenth.data(), R / 10, 2, R); // a tenth of a second apart
				const int now = voices.lastVariation(cue);
				repeated = repeated || now == previous;
				used |= 1u << now;
				previous = now;
			}
			std::printf("  %s: %u variations, used %u of them, %s\n", CueName(cue), variations, static_cast<unsigned>(__builtin_popcount(used)),
				repeated ? "a repeat" : "never twice in a row");
			EXPECT(variations >= 2 && !repeated && used == (1u << variations) - 1, "variations at random, all used, never twice in a row");
		}
	}

	// ---- requests that waited too long, and a full ring
	{
		CueVoices voices;
		voices.setBank(bank);
		voices.setSet(SoundSet::Paper);
		voices.post(CueId::Select);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		std::vector<float> out(BLOCK_FRAMES * 2, 0.0f);
		voices.mix(out.data(), BLOCK_FRAMES, 2, R);
		EXPECT(voices.started == 0 && voices.stale == 1 && peakOf(out) == 0.0f, "a request nobody rendered for a quarter of a second is dropped");
		int queued = 0;
		for (int i = 0; i < 100; i++)
			queued += voices.post(CueId::Tab) ? 1 : 0;
		EXPECT(queued == static_cast<int>(CueVoices::QUEUE), "a full ring drops what does not fit, without waiting");
		// Many threads at once: every request is either queued or refused, none is lost or doubled.
		voices.stopAll();
		std::atomic<int> accepted{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++)
			threads.emplace_back([&] {
				for (int i = 0; i < 40; i++)
					accepted += voices.post(CueId::Tab) ? 1 : 0;
			});
		for (std::thread& t : threads)
			t.join();
		unsigned long long before = voices.rateLimited + voices.started;
		voices.mix(out.data(), BLOCK_FRAMES, 2, R);
		EXPECT(accepted == static_cast<int>(CueVoices::QUEUE) && voices.rateLimited + voices.started - before == CueVoices::QUEUE,
			"four threads asking at once: the ring holds exactly what it took");
	}

	// ---- every cue of both sets, a little apart, to listen to
	for (const SoundSet set : {SoundSet::Glass, SoundSet::Paper})
	{
		CueVoices voices;
		voices.setBank(bank);
		voices.setSet(set);
		Mixer mixer;
		mixer.setCues(&voices);
		std::vector<s16> heard;
		for (unsigned c = 0; c < CUE_COUNT; c++)
		{
			voices.post(static_cast<CueId>(c));
			renderFor(mixer, c >= static_cast<unsigned>(CueId::Notify) ? 1.6 : 0.6, &heard);
		}
		writeWav(work + "/cues-" + SoundSetFolder(set) + ".wav", heard, R, 2);
		EXPECT(rms(heard, 0, heard.size() / 2) > 0.005, "the cues are heard through the mixer");
	}
}

// ------------------------------------------------------------------ the playlist's rules

static void TestPlaylistRules(const std::string& work)
{
	const std::vector<std::string> own = {"m/a.ogg", "m/b.mp3"}, kit = {"k/1.ogg", "k/2.ogg", "k/3.ogg"};
	EXPECT((Playlist::Sources(own, "bg.mp3", kit, true) == std::vector<std::string>{"m/a.ogg", "m/b.mp3", "bg.mp3"}),
		"the music folder's tracks, then the old background file");
	EXPECT((Playlist::Sources({}, "bg.mp3", kit, true) == std::vector<std::string>{"bg.mp3"}), "the background file alone: no kit music");
	EXPECT(Playlist::Sources({}, "", kit, true) == kit, "nothing of one's own: the kit's music");
	EXPECT(Playlist::Sources({}, "", kit, false).empty(), "nothing of one's own and the kit's off: no music");
	EXPECT(Playlist::NameOf(work + "/music/First Light.ogg") == "First Light" && Playlist::NameOf("noext") == "noext", "a track's name");

	// The folder: sound files only, in name order without regard to capitals.
	const std::string folder = work + "/list-test";
	makeDir(folder + "/e.ogg");
	for (const char* name : {"B.wav", "a.mp3", "._a.mp3", "c.txt", "d.FLAC"})
		writeWav(folder + "/" + name, tone(300, 0.1, 8000, 1), 8000, 1);
	const std::vector<std::string> listed = Playlist::ListFolder(folder);
	EXPECT((listed == std::vector<std::string>{folder + "/a.mp3", folder + "/B.wav", folder + "/d.FLAC"}),
		"the music folder: its sound files by name, not a Mac's ._ files, not folders");
	EXPECT(Playlist::ListFolder(work + "/not-there").empty(), "no music folder: no tracks");

	Playlist list;
	list.set({"x", "y", "z"}, MusicOrder::InOrder, 1);
	std::string got;
	for (int i = 0; i < 5; i++)
		got += list.next();
	EXPECT(got == "xyzxy", "in order, round and round");
	list.set({"x", "y", "z"}, MusicOrder::InOrder, 1, "y");
	EXPECT(list.next() == "z", "a new list goes on after the track that plays");

	// Shuffled: each round has every track once, a round never begins with the track the one
	// before ended with, and another seed is another order.
	bool rounds_whole = true, repeat = false;
	std::string previous;
	std::vector<std::string> orders;
	for (std::uint64_t seed = 1; seed <= 6; seed++)
	{
		const std::vector<std::string> tracks = {"a", "b", "c", "d", "e"};
		list.set(tracks, MusicOrder::Shuffle, seed * 7919);
		std::string first_round;
		for (int round = 0; round < 20; round++)
		{
			std::string seen;
			for (int i = 0; i < 5; i++)
			{
				const std::string& t = list.next();
				repeat = repeat || t == previous;
				previous = t;
				seen += t;
			}
			if (round == 0)
				first_round = seen;
			std::string sorted = seen;
			std::sort(sorted.begin(), sorted.end());
			rounds_whole = rounds_whole && sorted == "abcde";
		}
		orders.push_back(first_round);
		previous.clear();
	}
	std::sort(orders.begin(), orders.end());
	const auto distinct = std::unique(orders.begin(), orders.end()) - orders.begin();
	std::printf("  shuffled: %d different first rounds of six seeds\n", static_cast<int>(distinct));
	EXPECT(rounds_whole && !repeat && distinct >= 4, "shuffled: every track once a round, never twice in a row, another order each seed");
	list.set({"x", "y"}, MusicOrder::Shuffle, 3, "x");
	EXPECT(list.next() == "y", "shuffled, a new list does not begin with the track that plays");
	list.set({}, MusicOrder::Shuffle, 3);
	EXPECT(list.next().empty(), "an empty list gives nothing");
}

// ------------------------------------------------------------------ from one track into the next

static void TestCrossfade(const std::string& work)
{
	const unsigned R = DSP::OUTPUT_RATE;
	Mixer mixer;
	mixer.setVolume(0.8f, 0.5f);
	mixer.setPlaylistLength(2);
	// (Two rates: one of them is resampled on the way.)
	mixer.queueTrack(soundOf(tone(220, 4.0, 44100, 2, 0.5), 44100, 2), false, "a.wav", true);
	mixer.startBackground();
	EXPECT(mixer.hasTrack() && mixer.wantsNextTrack(), "a short track asks for the next at once");
	std::vector<s16> heard;
	renderFor(mixer, 0.5, &heard);
	const unsigned serial = mixer.heardSerial();
	mixer.queueTrack(soundOf(tone(330, 4.0, R, 2, 0.5), R, 2), false, "b.wav", false);
	EXPECT(!mixer.wantsNextTrack() && !mixer.crossfading(), "with the next one queued, nothing more is asked for, and it waits");
	std::size_t fade_began = 0, fade_ended = 0;
	for (int b = 0; b < static_cast<int>(6.0 * R / BLOCK_FRAMES); b++)
	{
		const bool before = mixer.crossfading();
		renderFor(mixer, BLOCK_FRAMES / static_cast<double>(R), &heard);
		if (!before && mixer.crossfading())
			fade_began = heard.size() / 2;
		if (before && !mixer.crossfading())
			fade_ended = heard.size() / 2;
	}
	writeWav(work + "/playlist-crossfade.wav", heard, R, 2);
	const double began = fade_began / static_cast<double>(R), ended = fade_ended / static_cast<double>(R);
	std::printf("  crossfade from %.2f s to %.2f s (a 4 s track, a third of it)\n", began, ended);
	EXPECT(began > 2.5 && began < 2.8 && std::fabs((ended - began) - 4.0 / 3.0) < 0.02, "the next track comes in over the last third of a short one");
	EXPECT(mixer.heardSerial() == serial + 1 && mixer.trackPath() == "b.wav", "and is the track after it");
	// No click: the steepest step between frames is that of the higher tone, never more.
	int worst = 0;
	for (std::size_t i = fade_began - R / 2; i + 1 < fade_ended + R / 2; i++)
		worst = std::max(worst, std::abs(heard[(i + 1) * 2] - heard[i * 2]));
	// (Halfway, a cosine of the 220 Hz tone and a sine of the 330 Hz one: their slopes add up to
	// at most that of a tone of sqrt(220^2 + 330^2) Hz.)
	const double slope = 0.5 * 0.5 * 32767 * 2 * M_PI * std::sqrt(220.0 * 220.0 + 330.0 * 330.0) / R;
	std::printf("  largest step across the crossfade: %d (the two tones at this level: at most about %.0f)\n", worst, slope);
	EXPECT(worst < slope * 1.05 + 20, "the crossfade has no step in it");
	// Equal power: the loudness through the fade stays within 1.5 dB of what it was.
	const double level = rms(heard, fade_began - R / 2, fade_began);
	double low = 1e9, high = 0;
	for (std::size_t at = fade_began; at + R / 10 <= fade_ended; at += R / 10)
	{
		const double r = rms(heard, at, at + R / 10);
		low = std::min(low, r);
		high = std::max(high, r);
	}
	std::printf("  loudness through the crossfade: %.1f%% .. %.1f%% of before\n", 100 * low / level, 100 * high / level);
	EXPECT(low > level * 0.84 && high < level * 1.19, "the crossfade keeps the loudness");

	// A track asked for at once (the next button) comes in over three seconds at most.
	mixer.queueTrack(soundOf(tone(260, 20.0, R, 2, 0.5), R, 2), false, "c.wav", true);
	renderFor(mixer, 0.02, &heard);
	EXPECT(mixer.crossfading(), "a track asked for at once comes in at once");
	// Let go (a game starts): the track and the place are remembered, the memory goes.
	std::unique_ptr<DSP::LoopPlayer> a, b;
	mixer.releaseMusic(&a, &b);
	EXPECT(!mixer.hasTrack() && a != nullptr && mixer.trackPath() == "c.wav", "let go in the middle of a crossfade, the one coming in is remembered");
	// (And read again from where it was: seek.)
	DSP::LoopPlayer player(soundOf(tone(260, 20.0, 32000, 2, 0.5), 32000, 2), false);
	player.seek(12.5);
	EXPECT(std::fabs(player.seconds() - 12.5) < 1e-6, "a track is taken up again at the second it was let go");
	player.seek(41.0);
	EXPECT(std::fabs(player.seconds() - 1.0) < 1e-3, "a place past its end is in a later round");
}

// ------------------------------------------------------------------ over the game, at its rate

static void TestGameMixIn(const std::string& work)
{
	// The quick menu's sounds go into a game's stream at another rate than the recordings'.
	const unsigned rate = 44100, block = 441;
	SetSoundSet(SoundSet::Glass);
	SetSoundVolume(1.0f);
	std::vector<float> game, mixed;
	const auto stream = [&](double seconds) {
		std::vector<float> frames(block * 2);
		const std::size_t blocks = static_cast<std::size_t>(seconds * rate / block);
		for (std::size_t b = 0; b < blocks; b++)
		{
			for (unsigned i = 0; i < block; i++)
			{
				const std::size_t n = game.size() / 2 + i;
				frames[i * 2] = frames[i * 2 + 1] = static_cast<float>(0.5 * std::sin(2 * M_PI * 440.0 * n / rate));
			}
			game.insert(game.end(), frames.begin(), frames.end());
			MixIntoGame(frames.data(), block, 2, rate);
			mixed.insert(mixed.end(), frames.begin(), frames.end());
		}
	};
	stream(0.2);
	EXPECT(game == mixed, "with the menu closed the game's sound is not touched");
	SetGameMixIn(true);
	EXPECT(GameMixInWanted(), "with the menu open the stream mixes the interface's sounds in");
	Cue(CueId::Launch);
	stream(2.0);
	// Where the cue is heard, in the stream's frames: its length at the stream's rate.
	std::size_t first = mixed.size(), last = 0;
	float difference = 0.0f;
	for (std::size_t i = 0; i < mixed.size() / 2; i++)
	{
		const float d = std::fabs(mixed[i * 2] - game[i * 2]);
		difference = std::max(difference, d);
		if (d > 1e-4f)
		{
			first = std::min(first, i);
			last = i;
		}
	}
	const double heard = (last - first) / static_cast<double>(rate);
	// How long the recording is heard at its own rate (to its last sample above the same level).
	double recorded = 0.0;
	{
		const std::shared_ptr<CueBank> bank = CueBank::Load(work + "/resources/sounds");
		const CueBank::Clip& clip = (*bank->find(SoundSet::Glass, CueId::Launch))[0];
		std::size_t end = 0, begin = clip.frames;
		for (std::size_t i = 0; i < clip.frames; i++)
			if (std::abs(clip.stereo[i * 2]) > 3)
				begin = std::min(begin, i), end = i;
		recorded = (end - begin) / static_cast<double>(clip.rate);
	}
	std::printf("  Launch over a 44.1 kHz game: heard for %.3f s (the recording at 48 kHz: %.3f s, +-3%% jitter), at most %.3f added\n", heard,
		recorded, static_cast<double>(difference));
	EXPECT(first < static_cast<std::size_t>(0.2 * rate) + 2 * block && std::fabs(heard / recorded - 1.0) < 0.04,
		"the cue plays in the game's stream, brought to its rate");
	float peak = 0.0f;
	for (const float x : mixed)
		peak = std::max(peak, std::fabs(x));
	EXPECT(peak <= 1.0f, "the sum is bent below full scale");
	{
		std::vector<s16> out(mixed.size());
		for (std::size_t i = 0; i < mixed.size(); i++)
			out[i] = static_cast<s16>(std::lrint(mixed[i] * 32767.0f));
		writeWav(work + "/game-mix-44k.wav", out, rate, 2);
	}
	// Eight channels: only the front pair gets the sounds.
	{
		std::vector<float> eight(256 * 8, 0.25f);
		Cue(CueId::Notify);
		MixIntoGame(eight.data(), 256, 8, 48000);
		bool others = true, front = false;
		for (unsigned i = 0; i < 256; i++)
		{
			for (unsigned c = 2; c < 8; c++)
				others = others && eight[i * 8 + c] == 0.25f;
			front = front || eight[i * 8] != 0.25f;
		}
		EXPECT(front && others, "in a stream of eight channels the sounds go to the front pair only");
	}
	// Closed: for a moment more (the Resume sound is heard out), then not at all.
	SetGameMixIn(false);
	EXPECT(GameMixInWanted(), "just after the menu closed, the last sound is still mixed in");
	std::this_thread::sleep_for(std::chrono::milliseconds(2600));
	EXPECT(!GameMixInWanted(), "and some seconds later nothing is");
	game.clear();
	mixed.clear();
	Cue(CueId::Select);
	stream(0.3);
	EXPECT(game == mixed, "a cue asked for with the menu closed is not heard over the game");
	SetSoundVolume(0.8f);
}

// ------------------------------------------------------------------ the playlist, as the title plays it

static void TestPlaylistThroughTheThreads(const std::string& work)
{
	const unsigned R = DSP::OUTPUT_RATE;
	// A title folder with a music folder of three short tracks, and an old background file.
	const std::string folder = work + "/title-with-music";
	makeDir(folder + "/music");
	writeWav(folder + "/music/one.wav", tone(300, 2.0, R, 2, 0.4), R, 2);
	writeWav(folder + "/music/Two.wav", tone(400, 2.0, 44100, 2, 0.4), 44100, 2);
	writeWav(folder + "/music/three.wav", tone(500, 2.0, R, 1, 0.4), R, 1);
	writeWav(folder + "/background.wav", tone(600, 2.0, R, 2, 0.4), R, 2);

	Tape tape;
	tape.speed = 4.0;
	Testing::SetOutput(&tape);
	Testing::SetFolder(folder);
	SetEnabled(true, true);
	SetMusic(MusicOrder::InOrder, true);
	Start();
	StartBackground();
	EXPECT(Testing::WaitForTrack(10), "the first track is read");
	const std::vector<std::string> paths = Testing::PlaylistPaths();
	EXPECT((paths == std::vector<std::string>{folder + "/music/one.wav", folder + "/music/three.wav", folder + "/music/Two.wav", folder + "/background.wav"}),
		"the music folder's tracks by name, then background.wav");
	// The names shown, in the order they come.
	std::vector<std::string> names;
	unsigned serial = 0;
	for (int i = 0; i < 1300 && names.size() < 6; i++)
	{
		const TrackInfo now = CurrentTrack();
		if (!now.name.empty() && now.serial != serial)
		{
			names.push_back(now.name);
			serial = now.serial;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	std::string order;
	for (const std::string& n : names)
		order += n + " ";
	std::printf("  the playlist as heard: %s\n", order.c_str());
	EXPECT(names.size() >= 5 && names[0] == "one" && names[1] == "three" && names[2] == "Two" && names[3] == "background" && names[4] == "one",
		"in order, crossfaded one into the next, round and round");

	// A game: the music goes, and comes back with the same track.
	const std::string before = CurrentTrack().name;
	Stop();
	EXPECT(CurrentTrack().name.empty(), "while a game runs no track is held");
	Start();
	EXPECT(Testing::WaitForTrack(10), "after the game the music is read again");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT(CurrentTrack().name == before, "and goes on with the track it played");

	// The kit's music, when the user has none; none when that is off.
	const std::string bare = work + "/title-bare";
	makeDir(bare + "/resources/music");
	writeWav(bare + "/resources/music/kit-a.wav", tone(350, 2.0, R, 2, 0.3), R, 2);
	writeWav(bare + "/resources/music/kit-b.wav", tone(450, 2.0, R, 2, 0.3), R, 2);
	Testing::SetFolder(bare);
	Testing::Rebuild();
	std::vector<std::string> kit;
	for (int i = 0; i < 500; i++)
	{
		kit = Testing::PlaylistPaths();
		if (!kit.empty() && kit[0].find("kit-") != std::string::npos)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT((kit == std::vector<std::string>{bare + "/resources/music/kit-a.wav", bare + "/resources/music/kit-b.wav"}), "nothing of one's own: the kit's music");
	bool kit_heard = false;
	for (int i = 0; i < 500 && !kit_heard; i++)
	{
		kit_heard = CurrentTrack().name.rfind("kit-", 0) == 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT(kit_heard, "and the track that played (not in the new list) gives way to it");
	SetMusic(MusicOrder::InOrder, false);
	bool silent = false;
	for (int i = 0; i < 500 && !silent; i++)
	{
		silent = Testing::PlaylistPaths().empty() && CurrentTrack().name.empty();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT(silent, "the kit's music turned off, and none of one's own: no music");
	std::printf("  %s\n", Describe().c_str());

	// The sounds wake a mixing thread that had nothing to do.
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	const std::size_t quiet = tape.frames();
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	EXPECT(tape.frames() == quiet, "with no music nothing is written");
	Cue(CueId::Welcome);
	tape.waitFor(R / 2);
	{
		const std::lock_guard<std::mutex> lock(tape.mutex);
		EXPECT(rms(tape.heard, quiet, quiet + R / 2) > 0.01, "a cue wakes the mixing thread and is heard at once");
	}
	Stop();
	SetMusic(MusicOrder::Shuffle, true);
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	const std::string work = argv[1];
	const unsigned R = DSP::OUTPUT_RATE;

	// ---- files of every kind are read
	writeWav(work + "/t-mono8k.wav", tone(300, 1.0, 8000, 1), 8000, 1);
	writeWav(work + "/t-51.wav", tone(200, 1.0, 44100, 6), 44100, 6);
	{
		DSP::Sound s;
		DSP::FileInfo info;
		std::string error;
		EXPECT(DSP::LoadSound((work + "/t-mono8k.wav").c_str(), 60, &s, &info, &error) && info.rate == 8000 && info.channels == 1 && s.frames == 8000,
			"an 8 kHz mono WAV is read");
		DSP::Sound s6;
		EXPECT(DSP::LoadSound((work + "/t-51.wav").c_str(), 60, &s6, &info, &error) && info.channels == 6 && s6.frames == 44100, "a 5.1 WAV is read");
		DSP::Sound cut;
		EXPECT(DSP::LoadSound((work + "/t-51.wav").c_str(), 0.5, &cut, &info, &error) && info.cut && cut.frames <= 22050 + 64, "a long file is cut at the limit");
		// Garbage and truncated files are refused or give what is there, and nothing breaks.
		{
			std::FILE* f = std::fopen((work + "/t-garbage.wav").c_str(), "wb");
			for (int i = 0; i < 5000; i++)
				std::fputc((i * 7919) & 0xff, f);
			std::fclose(f);
		}
		DSP::Sound g;
		EXPECT(!DSP::LoadSound((work + "/t-garbage.wav").c_str(), 60, &g, &info, &error) && !error.empty(), "garbage named .wav is refused, with a reason");
		for (const char* ext : {"mp3", "ogg", "flac"})
		{
			const std::string good = work + "/t-good." + ext, bad = work + "/t-bad." + ext, shortened = work + "/t-short." + ext;
			if (std::FILE* f = std::fopen(good.c_str(), "rb"))
			{
				std::fclose(f);
				DSP::Sound d;
				const bool ok = DSP::LoadSound(good.c_str(), 60, &d, &info, &error);
				EXPECT(ok && d.frames > info.rate && d.frames < info.rate * 3ull, ext);
				if (ok)
					std::printf("  %s: %s, %u Hz, %u ch, %.2f s, rms %.3f\n", ext, info.format.c_str(), info.rate, info.channels, info.seconds,
						[&] { std::vector<s16> v; for (std::uint64_t i = 0; i < d.frames; i++) { v.push_back(d.frame(i)[0]); v.push_back(d.frame(i)[1]); } return rms(v, 0, v.size() / 2); }());
				// The same file cut in half, and garbage under its name.
				std::vector<unsigned char> bytes;
				if (std::FILE* in = std::fopen(good.c_str(), "rb"))
				{
					int c;
					while ((c = std::fgetc(in)) != EOF)
						bytes.push_back(static_cast<unsigned char>(c));
					std::fclose(in);
				}
				if (std::FILE* out = std::fopen(shortened.c_str(), "wb"))
				{
					std::fwrite(bytes.data(), 1, bytes.size() / 2, out);
					std::fclose(out);
				}
				DSP::Sound h;
				DSP::LoadSound(shortened.c_str(), 60, &h, &info, &error); // either answer is fine; it must not break
				if (std::FILE* out = std::fopen(bad.c_str(), "wb"))
				{
					for (std::size_t i = 0; i < bytes.size(); i++)
						std::fputc(static_cast<int>((i * 2654435761u) >> 13) & 0xff, out);
					std::fclose(out);
				}
				DSP::Sound b;
				EXPECT(!DSP::LoadSound(bad.c_str(), 60, &b, &info, &error) || b.frames < 48000 * 60, "garbage under a sound's name does no harm");
			}
			else
			{
				std::printf("  (no %s test file: ffmpeg was not there to make one)\n", ext);
			}
		}
	}

	// ---- the start-up sound: the effects alone, and where its landings are
	std::vector<s16> alone;
	DSP::StartupInfo info;
	EXPECT(DSP::RenderStartup(nullptr, &alone, &info) && info.peak > 0.3f && info.peak <= 1.0f && info.seconds > 4.5, "the start-up sound is made without a bed");
	writeWav(work + "/startup-effects-alone.wav", alone, R, 2);
	{
		// Each letter's landing is the loudest moment near its time.
		for (int i = 0; i < DSP::STARTUP_LETTERS; i++)
		{
			const double at = DSP::STARTUP_LETTER_START[i] + DSP::STARTUP_LETTER_FLIGHT + DSP::STARTUP_OFFSET;
			const std::size_t frame = static_cast<std::size_t>(at * R);
			const double before = rms(alone, frame - 2400, frame - 480), after = rms(alone, frame, frame + 1920);
			EXPECT(after > before * 1.25, "a letter's landing is heard at its time");
		}
		bool finite = true;
		for (const s16 v : alone)
			finite = finite && v > -32768;
		EXPECT(finite, "nothing in it is at the rail");
	}
	if (argc > 2)
	{
		DSP::Sound bed;
		DSP::FileInfo fi;
		std::string error;
		std::vector<s16> with;
		if (DSP::LoadSound(argv[2], 30, &bed, &fi, &error) && DSP::RenderStartup(&bed, &with, &info))
		{
			writeWav(work + "/startup-with-own-sound.wav", with, R, 2);
			std::printf("  with a bed of one's own (%s, %u Hz, %.2f s): %.2f s, peak %.2f, rms %.3f\n", fi.format.c_str(), fi.rate, fi.seconds, info.seconds,
				static_cast<double>(info.peak), rms(with, 0, with.size() / 2));
		}
		else
		{
			std::printf("  the bed given could not be used: %s\n", error.c_str());
			g_failed++;
		}
	}

	// ---- a start-up sound played as it is
	{
		std::vector<s16> plain;
		DSP::Sound six;
		const std::vector<s16> source = tone(330, 6.0, 32097, 1, 0.2);
		// (As a stereo sound at its own rate.)
		{
			std::vector<s16> both(source.size() * 2);
			for (std::size_t i = 0; i < source.size(); i++)
				both[i * 2] = both[i * 2 + 1] = source[i];
			six.rate = 32097;
			EXPECT(six.append(both.data(), source.size()), "a sound is kept");
			six.frames = source.size();
		}
		EXPECT(DSP::RenderPlain(six, 4.0, 1.0, &plain, &info), "a start-up sound is taken as it is");
		EXPECT(std::fabs(info.seconds - 4.0) < 0.01, "and cut to the seconds asked for");
		EXPECT(info.peak > 0.70f && info.peak < 0.74f, "its peak brought to the start-up sound's level");
		EXPECT(rms(plain, R * 1, R * 2) > 0.3, "it is heard in full before its end");
		EXPECT(rms(plain, static_cast<std::size_t>(3.9 * R), static_cast<std::size_t>(4.0 * R) - 1) < 0.05, "and has faded by its end");
		EXPECT(plain.size() >= 2 && std::abs(plain[plain.size() - 2]) < 64, "to silence");
		// A sound shorter than what is asked for is not faded early.
		EXPECT(DSP::RenderPlain(six, 9.0, 2.0, &plain, &info) && std::fabs(info.seconds - 6.0) < 0.01, "a shorter one is played to its own end");
		EXPECT(rms(plain, static_cast<std::size_t>(5.5 * R), static_cast<std::size_t>(5.9 * R)) > 0.3, "at its level until then");
		DSP::Sound nothing;
		EXPECT(!DSP::RenderPlain(nothing, 8.0, 1.8, &plain, &info), "and nothing is made of nothing");
	}
	if (const char* const builtin = std::getenv("UIAUDIO_BUILTIN"))
	{
		// The sound the title comes with: its hit is where the start-up animations have theirs.
		DSP::Sound sound;
		DSP::FileInfo fi;
		std::string error;
		std::vector<s16> plain;
		if (DSP::LoadSound(builtin, 30, &sound, &fi, &error) &&
			DSP::RenderPlain(sound, DSP::STARTUP_PLAIN_SECONDS, DSP::STARTUP_PLAIN_FADE, &plain, &info))
		{
			std::printf("  the title's own start-up sound (%s, %u Hz, %.2f s): %.2f s played, peak %.2f\n", fi.format.c_str(), fi.rate, fi.seconds, info.seconds,
				static_cast<double>(info.peak));
			const std::size_t hit = static_cast<std::size_t>(1.12 * R);
			EXPECT(rms(plain, hit, hit + R / 5) > 1.6 * rms(plain, hit - R / 5, hit), "the title's start-up sound has its hit at 1.12 s");
			EXPECT(std::fabs(info.seconds - DSP::STARTUP_PLAIN_SECONDS) < 0.01, "and is played for eight seconds");
			EXPECT(rms(plain, static_cast<std::size_t>(7.9 * R), static_cast<std::size_t>(8.0 * R) - 1) < 0.01, "ending in silence");
		}
		else
		{
			std::printf("  the title's start-up sound could not be used: %s\n", error.c_str());
			g_failed++;
		}
	}

	// ---- the whole sequence through the mixer's thread
	writeWav(work + "/startup.wav", tone(404, 5.9, 32097, 1, 0.5), 32097, 1);
	writeWav(work + "/background.wav", tone(220, 2.0, 44100, 2, 0.5), 44100, 2);
	Tape tape;
	Testing::SetOutput(&tape);
	Testing::SetFolder(work);
	SetVolume(0.8f, 0.4f);
	Start();
	Start();
	Testing::WaitUntilLoaded();
	EXPECT(tape.frames() == 0, "nothing is written while there is nothing to hear");
	PlayStartup();
	tape.waitFor(static_cast<std::size_t>(4.6 * R));
	StartBackground(); // the library opens: the background sound waits for the start-up's tail
	tape.waitFor(static_cast<std::size_t>(5.0 * R));
	Stop(); // a game starts
	EXPECT(tape.opens == 1 && tape.closes == 1, "the port is closed when the sound is stopped");
	const std::size_t stopped = tape.frames();
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	EXPECT(tape.frames() == stopped, "nothing is written after the stop");
	Start(); // the game is gone
	tape.waitFor(static_cast<std::size_t>(3.0 * R));
	Stop();
	Stop();
	EXPECT(tape.opens == 2 && tape.closes == 2, "and opened again when it is started again");
	{
		const std::vector<s16>& h = tape.heard;
		writeWav(work + "/sequence.wav", h, R, 2);
		EXPECT(rms(h, R * 1, R * 2) > 0.02, "the start-up sound is heard");
		EXPECT(rms(h, static_cast<std::size_t>(7.5 * R), static_cast<std::size_t>(8.5 * R)) > 0.02, "the background sound comes after it");
		// The stop fades: the last block before it is quieter than a second earlier, and ends at nothing.
		EXPECT(std::abs(h[(stopped - 1) * 2]) < 200, "the stop ends in silence, not in a step");
		EXPECT(rms(h, stopped + R * 2, stopped + R * 3 - 512) > 0.02, "the background sound is back after the game");
		// No step in the loop: a 220 Hz tone at this level never moves more than this between frames.
		int worst = 0;
		for (std::size_t i = static_cast<std::size_t>(7.8 * R); i + 1 < stopped - R / 4; i++)
			worst = std::max(worst, std::abs(h[(i + 1) * 2] - h[i * 2]));
		std::printf("  largest step from frame to frame in the looping background sound: %d (a 220 Hz tone at this level: about %d)\n", worst,
			static_cast<int>(0.5 * 0.4 * 32767 * 2 * M_PI * 220 / R * 1.3));
		EXPECT(worst < 0.5 * 0.4 * 32767 * 2 * M_PI * 220 / R * 2.5 + 40, "the loop's ends meet without a click");
	}

	// A port that does not open: silence, and nothing waits for ever.
	Tape closed;
	closed.refuse = true;
	Testing::SetOutput(&closed);
	Start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Stop();
	EXPECT(closed.heard.empty() && closed.closes == 0, "a port that does not open leaves the interface silent and working");

	TestCues(work);
	TestPlaylistRules(work);
	TestCrossfade(work);
	TestGameMixIn(work);
	TestPlaylistThroughTheThreads(work);

	std::printf("uiaudio: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
