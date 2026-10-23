// Crux2 - the interface's sound (see ps5_ui_audio.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The cue vocabulary, the two sound sets and their fallback rule, and the stand-in tones follow
// the ps5-homebrew-ui kit by BlackBearReloaded (src/audio/cues.cpp, GPL-3.0-or-later), whose
// recordings and music Crux2 ships; the mixing is Crux2's own.

#include "ps5_ui_audio.h"
#include "ps5_priority.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#ifdef __PROSPERO__
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned length, unsigned frequency, unsigned format);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void* samples);
void ps5px_log(const char* format, ...);
const char* ps5px_title_dir(void);
}
#define UIAUDIO_LOG(...) ps5px_log("interface sound: " __VA_ARGS__)
#else
#define UIAUDIO_LOG(...) (std::fprintf(stderr, "interface sound: " __VA_ARGS__), std::fputc('\n', stderr))
#endif

namespace PS5UIAudio
{
namespace
{
	constexpr double PI = 3.14159265358979323846;

	std::int64_t NowNanoseconds()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// What each cue is called (its recordings' file name), and the tone it is played as when no
	// set records it: the kit's own table (wave, start and end pitch, length, level).
	enum class Wave
	{
		Sine,
		Triangle,
		SoftSquare
	};
	struct CueInfo
	{
		const char* name;
		Wave wave;
		float start_hz, end_hz, seconds, gain;
	};
	constexpr CueInfo CUES[CUE_COUNT] = {
		{"focus", Wave::Sine, 660, 700, 0.035f, 0.25f},
		{"select", Wave::Triangle, 740, 880, 0.08f, 0.4f},
		{"back", Wave::Triangle, 520, 420, 0.08f, 0.35f},
		{"tab", Wave::Sine, 560, 640, 0.06f, 0.3f},
		{"open", Wave::Sine, 420, 640, 0.12f, 0.3f},
		{"modal_open", Wave::Triangle, 440, 560, 0.12f, 0.3f},
		{"modal_close", Wave::Triangle, 560, 440, 0.12f, 0.3f},
		{"toggle", Wave::Triangle, 700, 700, 0.05f, 0.3f},
		{"slider", Wave::Sine, 900, 900, 0.03f, 0.2f},
		{"error", Wave::SoftSquare, 200, 160, 0.14f, 0.3f},
		{"notify", Wave::Sine, 660, 990, 0.3f, 0.35f},
		{"launch", Wave::Sine, 300, 900, 0.35f, 0.35f},
		{"favorite_on", Wave::Sine, 880, 1320, 0.22f, 0.35f},
		{"favorite_off", Wave::Sine, 660, 440, 0.12f, 0.3f},
		{"saved", Wave::Sine, 700, 1050, 0.2f, 0.3f},
		{"welcome", Wave::Sine, 440, 880, 0.5f, 0.3f},
		{"resume", Wave::Sine, 500, 750, 0.2f, 0.3f},
	};

	// tanh, which the console's libc is not asked for (as in the DSP file).
	double Squash(double x)
	{
		const double e = std::exp(-2.0 * std::fabs(x));
		const double y = (1.0 - e) / (1.0 + e);
		return (x < 0.0) ? -y : y;
	}

	bool HasSoundExtension(const char* name)
	{
		const char* const dot = std::strrchr(name, '.');
		if (dot == nullptr)
			return false;
		char extension[8] = {};
		for (int i = 0; i < 7 && dot[i + 1] != '\0'; i++)
		{
			char c = dot[i + 1];
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			extension[i] = c;
		}
		return std::strcmp(extension, "wav") == 0 || std::strcmp(extension, "mp3") == 0 || std::strcmp(extension, "ogg") == 0 ||
			   std::strcmp(extension, "flac") == 0;
	}

	// Name order as a person reads it: without regard to capitals.
	bool LessByName(const std::string& a, const std::string& b)
	{
		const std::size_t n = std::min(a.size(), b.size());
		for (std::size_t i = 0; i < n; i++)
		{
			char x = a[i], y = b[i];
			if (x >= 'A' && x <= 'Z')
				x = static_cast<char>(x - 'A' + 'a');
			if (y >= 'A' && y <= 'Z')
				y = static_cast<char>(y - 'A' + 'a');
			if (x != y)
				return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
		}
		return a.size() < b.size();
	}
} // namespace

const char* CueName(CueId cue)
{
	const unsigned index = static_cast<unsigned>(cue);
	return index < CUE_COUNT ? CUES[index].name : "";
}

const char* SoundSetFolder(SoundSet set)
{
	switch (set)
	{
		case SoundSet::Glass:
			return "glass";
		case SoundSet::Paper:
			return "paper";
		default:
			return "";
	}
}

// --------------------------------------------------------------------------------- the recordings

bool CueBank::add(SoundSet set, CueId cue, const DSP::Sound& sound)
{
	if (set == SoundSet::Off || cue >= CueId::Count || sound.frames == 0 || sound.rate == 0)
		return false;
	Clip clip;
	clip.rate = sound.rate;
	clip.frames = static_cast<std::size_t>(sound.frames);
	clip.stereo.resize(clip.frames * 2);
	for (std::size_t i = 0; i < clip.frames; i++)
	{
		const s16* const frame = sound.frame(i);
		clip.stereo[i * 2] = frame[0];
		clip.stereo[i * 2 + 1] = frame[1];
	}
	clips[static_cast<unsigned>(set) - 1][static_cast<unsigned>(cue)].push_back(std::move(clip));
	return true;
}

void CueBank::makeTones()
{
	for (unsigned c = 0; c < CUE_COUNT; c++)
	{
		// A pitch sweep with a short attack and a release, as the kit's stand-in is.
		const CueInfo& info = CUES[c];
		const double attack = std::min(0.006, info.seconds * 0.2), release = std::min(0.12, info.seconds * 0.5);
		Clip clip;
		clip.tone = true;
		clip.frames = static_cast<std::size_t>(info.seconds * DSP::OUTPUT_RATE);
		clip.stereo.resize(clip.frames * 2);
		double phase = 0.0;
		for (std::size_t i = 0; i < clip.frames; i++)
		{
			const double t = static_cast<double>(i) / DSP::OUTPUT_RATE;
			const double progress = t / info.seconds;
			phase += (info.start_hz + (info.end_hz - info.start_hz) * progress) / DSP::OUTPUT_RATE;
			phase -= std::floor(phase);
			double value;
			switch (info.wave)
			{
				case Wave::Triangle:
					value = 4.0 * std::fabs(phase - 0.5) - 1.0;
					break;
				case Wave::SoftSquare:
					value = Squash(3.0 * std::sin(2.0 * PI * phase)) * 0.8;
					break;
				case Wave::Sine:
				default:
					value = std::sin(2.0 * PI * phase);
					break;
			}
			double envelope = std::min(1.0, t / attack);
			envelope = std::min(envelope, (info.seconds - t) / release);
			const s16 sample = static_cast<s16>(std::lrint(value * std::max(envelope, 0.0) * info.gain * 32767.0));
			clip.stereo[i * 2] = clip.stereo[i * 2 + 1] = sample;
		}
		tones[c].clear();
		tones[c].push_back(std::move(clip));
	}
}

bool CueBank::records(SoundSet set, CueId cue) const
{
	if (set == SoundSet::Off || cue >= CueId::Count)
		return false;
	return !clips[static_cast<unsigned>(set) - 1][static_cast<unsigned>(cue)].empty();
}

const std::vector<CueBank::Clip>* CueBank::find(SoundSet set, CueId cue) const
{
	if (set == SoundSet::Off || cue >= CueId::Count)
		return nullptr;
	const unsigned c = static_cast<unsigned>(cue), own = static_cast<unsigned>(set) - 1;
	// The set's own recordings; else the other set's (the kit's rule: a cue a set does not record
	// is played from the other sets); else the tone.
	if (!clips[own][c].empty())
		return &clips[own][c];
	for (unsigned other = 0; other < SOUND_SETS; other++)
	{
		if (!clips[other][c].empty())
			return &clips[other][c];
	}
	return tones[c].empty() ? nullptr : &tones[c];
}

std::shared_ptr<CueBank> CueBank::Load(const std::string& root)
{
	auto bank = std::make_shared<CueBank>();
	for (const SoundSet set : {SoundSet::Glass, SoundSet::Paper})
	{
		for (unsigned c = 0; c < CUE_COUNT; c++)
		{
			for (int n = 1; n < 100; n++)
			{
				char name[64];
				std::snprintf(name, sizeof(name), "/%s/%s_%02d.wav", SoundSetFolder(set), CUES[c].name, n);
				const std::string path = root + name;
				if (!DSP::FileExists(path.c_str()))
					break;
				DSP::Sound sound;
				DSP::FileInfo info;
				std::string error;
				// (The longest of them is under two seconds.)
				if (DSP::LoadSound(path.c_str(), 10.0, &sound, &info, &error) && bank->add(set, static_cast<CueId>(c), sound))
				{
					bank->files++;
				}
				else
				{
					bank->rejected++;
					UIAUDIO_LOG("%s cannot be used: %s", path.c_str(), error.c_str());
				}
			}
		}
	}
	bank->makeTones();
	return bank;
}

// ------------------------------------------------------------------------------------- the voices

CueVoices::CueVoices()
{
	for (unsigned i = 0; i < QUEUE; i++)
		m_queue[i].sequence.store(i, std::memory_order_relaxed);
	m_last_start.fill(-1e9);
	m_last_variation.fill(-1);
}

void CueVoices::setVolume(float volume)
{
	m_volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

bool CueVoices::post(CueId cue)
{
	if (cue >= CueId::Count || set() == SoundSet::Off)
		return false;
	// A bounded ring for many writers and one reader: a writer claims a place by moving the head
	// on, and publishes it by its sequence number; a place the reader has not freed yet is full.
	unsigned position = m_head.load(std::memory_order_relaxed);
	for (;;)
	{
		Request& request = m_queue[position % QUEUE];
		const unsigned sequence = request.sequence.load(std::memory_order_acquire);
		const int difference = static_cast<int>(sequence - position);
		if (difference == 0)
		{
			if (m_head.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
			{
				request.cue = static_cast<std::uint8_t>(cue);
				request.when = NowNanoseconds();
				request.sequence.store(position + 1, std::memory_order_release);
				return true;
			}
		}
		else if (difference < 0)
		{
			return false;
		}
		else
		{
			position = m_head.load(std::memory_order_relaxed);
		}
	}
}

bool CueVoices::pending() const
{
	const unsigned tail = m_tail;
	return m_queue[tail % QUEUE].sequence.load(std::memory_order_acquire) == tail + 1;
}

double CueVoices::MinimumGap(CueId cue)
{
	// Short enough that each step of a quick scroll is heard; long enough that a held direction
	// (thirty steps a second) does not become a buzz.
	switch (cue)
	{
		case CueId::Focus:
			return 0.045;
		case CueId::Slider:
			return 0.040;
		case CueId::Toggle:
		case CueId::Tab:
			return 0.050;
		case CueId::Error:
			return 0.080;
		default:
			return 0.030;
	}
}

std::uint32_t CueVoices::random()
{
	m_random = m_random * 1664525u + 1013904223u;
	return m_random >> 8;
}

unsigned CueVoices::voices() const
{
	unsigned count = 0;
	for (const Voice& voice : m_voices)
		count += (voice.clip != nullptr && voice.release < 0) ? 1u : 0u;
	return count;
}

void CueVoices::start(CueId cue)
{
	const unsigned c = static_cast<unsigned>(cue);
	const std::vector<CueBank::Clip>* const clips = m_bank ? m_bank->find(set(), cue) : nullptr;
	if (clips == nullptr || clips->empty())
		return;
	if (m_time - m_last_start[c] < MinimumGap(cue))
	{
		rateLimited++;
		return;
	}

	// A variation at random, never the one that played last.
	const int count = static_cast<int>(clips->size());
	int last = m_last_variation[c];
	if (last >= count)
		last = -1;
	int pick = 0;
	if (count > 1)
	{
		if (last < 0)
		{
			pick = static_cast<int>(random() % static_cast<std::uint32_t>(count));
		}
		else
		{
			pick = static_cast<int>(random() % static_cast<std::uint32_t>(count - 1));
			if (pick >= last)
				pick++;
		}
	}

	// Room for it: of one cue no more than MAX_SAME, of all no more than MAX_VOICES; the oldest
	// gives way, fading out in a few milliseconds rather than stopping with a click.
	const auto release_oldest = [&](bool same_cue) {
		Voice* oldest = nullptr;
		unsigned playing = 0;
		for (Voice& voice : m_voices)
		{
			if (voice.clip == nullptr || voice.release >= 0 || (same_cue && voice.cue != c))
				continue;
			playing++;
			if (oldest == nullptr || voice.age < oldest->age)
				oldest = &voice;
		}
		if (oldest != nullptr && playing >= (same_cue ? MAX_SAME : MAX_VOICES))
		{
			oldest->release = RELEASE_FRAMES;
			stolen++;
		}
	};
	release_oldest(true);
	release_oldest(false);

	Voice* slot = nullptr;
	for (Voice& voice : m_voices)
	{
		if (voice.clip == nullptr)
		{
			slot = &voice;
			break;
		}
	}
	if (slot == nullptr)
	{
		// (Every slot busy, the spare ones with fades: the fade nearest its end is cut.)
		for (Voice& voice : m_voices)
		{
			if (voice.release >= 0 && (slot == nullptr || voice.release < slot->release))
				slot = &voice;
		}
		if (slot == nullptr)
			return;
		dropped++;
	}

	Voice voice;
	voice.clip = &(*clips)[static_cast<std::size_t>(pick)];
	voice.position = 0.0;
	// +-3 %, as the kit does: a sound repeated many times does not sound mechanical.
	voice.pitch = 0.97f + 0.06f * static_cast<float>(random() & 0xffff) / 65535.0f;
	voice.gain = 1.0f;
	voice.cue = static_cast<std::uint8_t>(c);
	voice.age = ++m_clock;
	*slot = voice;
	started++;
	m_last_start[c] = m_time;
	m_last_variation[c] = pick;
	m_busy.store(true, std::memory_order_relaxed);
}

bool CueVoices::mix(float* frames, std::size_t count, unsigned stride, unsigned rate)
{
	if (rate == 0 || count == 0)
		return false;
	// The requests since the last time: those that waited too long (nothing was rendering) are
	// old news and are dropped.
	const std::int64_t now = NowNanoseconds();
	for (;;)
	{
		Request& request = m_queue[m_tail % QUEUE];
		if (request.sequence.load(std::memory_order_acquire) != m_tail + 1)
			break;
		const CueId cue = static_cast<CueId>(request.cue);
		const std::int64_t when = request.when;
		request.sequence.store(m_tail + QUEUE, std::memory_order_release);
		m_tail++;
		if (static_cast<double>(now - when) * 1e-9 > STALE_SECONDS)
		{
			stale++;
			continue;
		}
		start(cue);
	}

	const float volume = m_volume.load(std::memory_order_relaxed) * (1.0f / 32768.0f);
	bool added = false, playing = false;
	for (Voice& voice : m_voices)
	{
		if (voice.clip == nullptr)
			continue;
		const CueBank::Clip& clip = *voice.clip;
		// The recording's frames for one of the output's: its rate to the output's, and the jitter.
		const double step = static_cast<double>(clip.rate) / static_cast<double>(rate) * voice.pitch;
		const s16* const data = clip.stereo.data();
		const float gain = voice.gain * volume;
		float* out = frames;
		for (std::size_t i = 0; i < count; i++, out += stride)
		{
			const std::size_t index = static_cast<std::size_t>(voice.position);
			if (index >= clip.frames || voice.release == 0)
			{
				voice.clip = nullptr;
				break;
			}
			// (Between two frames: a straight line. These are short sounds near the output's rate.)
			const float fraction = static_cast<float>(voice.position - static_cast<double>(index));
			const std::size_t next = (index + 1 < clip.frames) ? index + 1 : index;
			float g = gain;
			if (voice.release > 0)
			{
				g *= static_cast<float>(voice.release) / RELEASE_FRAMES;
				voice.release--;
			}
			const float left = data[index * 2] + (data[next * 2] - data[index * 2]) * fraction;
			const float right = data[index * 2 + 1] + (data[next * 2 + 1] - data[index * 2 + 1]) * fraction;
			out[0] += left * g;
			out[1] += right * g;
			voice.position += step;
			added = true;
		}
		playing = playing || voice.clip != nullptr;
	}
	m_time += static_cast<double>(count) / static_cast<double>(rate);
	m_busy.store(playing, std::memory_order_relaxed);
	return added;
}

void CueVoices::stopAll()
{
	for (Voice& voice : m_voices)
		voice = Voice();
	for (;;)
	{
		Request& request = m_queue[m_tail % QUEUE];
		if (request.sequence.load(std::memory_order_acquire) != m_tail + 1)
			break;
		request.sequence.store(m_tail + QUEUE, std::memory_order_release);
		m_tail++;
	}
	m_busy.store(false, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------------- the playlist

const std::string Playlist::s_none;

std::vector<std::string> Playlist::Sources(const std::vector<std::string>& music_folder, const std::string& background,
	const std::vector<std::string>& kit, bool kit_music)
{
	std::vector<std::string> paths = music_folder;
	if (!background.empty())
		paths.push_back(background);
	if (paths.empty() && kit_music)
		paths = kit;
	return paths;
}

std::vector<std::string> Playlist::ListFolder(const std::string& folder)
{
	std::vector<std::string> names;
	DIR* const dir = opendir(folder.c_str());
	if (dir == nullptr)
		return names;
	while (const struct dirent* const entry = readdir(dir))
	{
		// (Hidden files, and the "._name" a Mac writes beside each file on a FAT or exFAT drive.)
		if (entry->d_name[0] == '.' || !HasSoundExtension(entry->d_name))
			continue;
		const std::string path = folder + "/" + entry->d_name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		names.push_back(entry->d_name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end(), LessByName);
	std::vector<std::string> paths;
	paths.reserve(names.size());
	for (const std::string& name : names)
		paths.push_back(folder + "/" + name);
	return paths;
}

std::string Playlist::NameOf(const std::string& path)
{
	const std::size_t slash = path.find_last_of('/');
	std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
	const std::size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && dot > 0)
		name.resize(dot);
	return name;
}

std::uint64_t Playlist::random()
{
	// splitmix64: a fresh order needs no more than this.
	std::uint64_t z = (m_state += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

void Playlist::shuffle(std::size_t avoid_first)
{
	const std::size_t n = m_paths.size();
	m_round.resize(n);
	for (std::size_t i = 0; i < n; i++)
		m_round[i] = i;
	for (std::size_t i = n; i > 1; i--)
		std::swap(m_round[i - 1], m_round[static_cast<std::size_t>(random() % i)]);
	// A new round does not begin with the track the last one ended with.
	if (n > 1 && m_round[0] == avoid_first)
		std::swap(m_round[0], m_round[1 + static_cast<std::size_t>(random() % (n - 1))]);
}

void Playlist::set(std::vector<std::string> paths, MusicOrder order, std::uint64_t seed, const std::string& keep)
{
	m_paths = std::move(paths);
	m_order = order;
	m_state = seed;
	m_at = 0;
	m_last = static_cast<std::size_t>(-1);
	for (std::size_t i = 0; i < m_paths.size() && !keep.empty(); i++)
	{
		if (m_paths[i] == keep)
		{
			m_last = i;
			break;
		}
	}
	if (order == MusicOrder::Shuffle)
	{
		shuffle(m_last);
	}
	else
	{
		m_round.resize(m_paths.size());
		for (std::size_t i = 0; i < m_round.size(); i++)
			m_round[i] = i;
		// In order: on from the track that plays.
		if (m_last != static_cast<std::size_t>(-1))
			m_at = m_last + 1;
	}
}

bool Playlist::contains(const std::string& path) const
{
	return !path.empty() && std::find(m_paths.begin(), m_paths.end(), path) != m_paths.end();
}

const std::string& Playlist::next()
{
	if (m_paths.empty())
		return s_none;
	if (m_at >= m_round.size())
	{
		if (m_order == MusicOrder::Shuffle)
			shuffle(m_last);
		m_at = 0;
	}
	m_last = m_round[m_at++];
	return m_paths[m_last];
}

// ------------------------------------------------------------------------------------ the mixer

void Mixer::setStartup(std::shared_ptr<const std::vector<s16>> stereo)
{
	m_startup = std::move(stereo);
}

void Mixer::setBackground(std::shared_ptr<const DSP::Sound> sound, bool crossfade)
{
	m_loop.reset();
	m_next.reset();
	m_fade_frames = 0;
	m_track_path.clear();
	m_heard_path.clear();
	m_track_seconds = 0.0;
	if (sound && sound->frames > 0 && sound->rate > 0)
		m_loop = std::make_unique<DSP::LoopPlayer>(std::move(sound), crossfade);
	m_track_serial++;
}

void Mixer::queueTrack(std::shared_ptr<const DSP::Sound> sound, bool loop_crossfade, std::string path, bool now, double start)
{
	if (!sound || sound->frames == 0 || sound->rate == 0)
		return;
	auto player = std::make_unique<DSP::LoopPlayer>(std::move(sound), loop_crossfade);
	if (start > 0.0)
		player->seek(start);
	if (m_loop == nullptr)
	{
		// Nothing plays: this one comes in as the background sound always has (its fade in).
		m_loop = std::move(player);
		m_track_path = std::move(path);
		m_track_seconds = m_loop->seconds();
		m_track_serial++;
		m_background_gain = 0.0f;
		return;
	}
	if (m_fade_frames > 0)
	{
		// (Two tracks are meeting already: this one waits for the end of that, and comes at once.)
		m_pending_path = std::move(path);
		m_pending = std::move(player);
		return;
	}
	m_next = std::move(player);
	m_next_path = std::move(path);
	m_next_now = now;
}

bool Mixer::advanceNow()
{
	if (m_next == nullptr || m_fade_frames > 0)
		return false;
	m_next_now = true;
	return true;
}

bool Mixer::wantsNextTrack() const
{
	if (m_playlist < 2 || m_loop == nullptr || m_next != nullptr || m_pending != nullptr || m_fade_frames > 0)
		return false;
	return m_loop->period() - m_loop->seconds() <= NEXT_TRACK_LEAD;
}

void Mixer::releaseMusic(std::unique_ptr<DSP::LoopPlayer>* current, std::unique_ptr<DSP::LoopPlayer>* next)
{
	*current = std::move(m_loop);
	*next = std::move(m_next);
	m_pending.reset();
	// (Let go in the middle of two tracks meeting: the one coming in is the one to come back.)
	if (m_fade_frames > 0 && !m_next_path.empty())
	{
		m_track_path = m_next_path;
		m_track_seconds = 0.0;
	}
	m_next_path.clear();
	m_heard_path.clear();
	m_fade_frames = 0;
	m_next_now = false;
}

std::unique_ptr<DSP::LoopPlayer> Mixer::takeRetired()
{
	return std::move(m_retired);
}

std::int64_t Mixer::crossfadeFrames() const
{
	// Three seconds, or a third of the shorter of the two tracks.
	double seconds = TRACK_CROSSFADE;
	if (m_loop)
		seconds = std::min(seconds, m_loop->period() / 3.0);
	if (m_next)
		seconds = std::min(seconds, m_next->period() / 3.0);
	return std::max<std::int64_t>(static_cast<std::int64_t>(seconds * DSP::OUTPUT_RATE), BLOCK_FRAMES);
}

void Mixer::setVolume(float startup, float background)
{
	m_startup_volume = std::clamp(startup, 0.0f, 1.0f);
	m_background_volume = std::clamp(background, 0.0f, 1.0f);
}

void Mixer::setEnabled(bool startup, bool background)
{
	m_startup_enabled = startup;
	m_background_enabled = background;
}

void Mixer::playStartup(double seconds)
{
	m_startup_wanted = true;
	m_startup_playing = true;
	m_startup_at = static_cast<std::int64_t>(std::max(seconds, 0.0) * DSP::OUTPUT_RATE);
	m_startup_gain = 1.0f;
	m_startup_gain_step = 0.0f;
}

void Mixer::skipStartup()
{
	if (m_startup_playing)
		m_startup_gain_step = -1.0f / (0.25f * DSP::OUTPUT_RATE);
	m_background_wanted = true;
}

void Mixer::startBackground()
{
	m_background_wanted = true;
}

void Mixer::fadeOut(double seconds)
{
	m_master_target = 0.0f;
	m_master_step = -1.0f / static_cast<float>(std::max(seconds, 0.001) * DSP::OUTPUT_RATE);
}

void Mixer::resume()
{
	// (Whatever was fading out comes back in, a little slower than it went.)
	m_master_target = 1.0f;
	m_master_step = 1.0f / (0.4f * DSP::OUTPUT_RATE);
	// A start-up sound a game's start cut off is not taken up again in the middle.
	if (m_startup_playing && m_master == 0.0f)
	{
		m_startup_playing = false;
		m_background_wanted = true;
	}
	if (m_background_on)
		m_background_gain = 0.0f; // its 1.5 s again
}

bool Mixer::idle() const
{
	const bool startup = m_startup_playing && m_startup_enabled && m_startup != nullptr;
	const bool background = m_background_enabled && m_loop != nullptr && (m_background_on || m_background_wanted);
	const bool cues = m_cues != nullptr && m_cues->busy();
	// (A start-up sound that is wanted and not made yet is waited for with silence going out: the
	// clock it joins is counted here.)
	return !startup && !background && !cues && !(m_startup_playing && m_startup_enabled);
}

void Mixer::render(s16* out)
{
	const std::int64_t startup_frames = (m_startup != nullptr) ? static_cast<std::int64_t>(m_startup->size() / 2) : 0;
	// When the background sound comes: at once when no start-up sound plays (or it was skipped
	// and has gone), else under the start-up sound's tail.
	if (m_background_wanted && !m_background_on && m_loop != nullptr && m_background_enabled)
	{
		const bool startup_heard = m_startup_playing && m_startup_enabled && m_startup != nullptr && m_startup_gain > 0.0f;
		const std::int64_t comes_at = static_cast<std::int64_t>(DSP::STARTUP_BACKGROUND_AT * DSP::OUTPUT_RATE);
		if (!startup_heard || m_startup_at >= std::min(comes_at, startup_frames) || m_startup_gain_step < 0.0f)
		{
			m_background_on = true;
			m_background_gain = 0.0f;
		}
	}

	if (m_scratch.size() != BLOCK_FRAMES * 2)
	{
		m_scratch.assign(BLOCK_FRAMES * 2, 0.0f);
		m_scratch_next.assign(BLOCK_FRAMES * 2, 0.0f);
		m_scratch_cues.assign(BLOCK_FRAMES * 2, 0.0f);
	}
	const bool background = m_background_on && m_loop != nullptr && m_background_enabled;
	if (background)
	{
		// The next track comes in over the current one's last seconds (or at once when asked):
		// the end goes down by the cosine as the next comes up by the sine.
		if (m_next != nullptr && m_fade_frames == 0)
		{
			const double remaining = (m_loop->period() - m_loop->seconds()) * DSP::OUTPUT_RATE;
			const std::int64_t length = crossfadeFrames();
			if (m_next_now || remaining <= static_cast<double>(length))
			{
				m_fade_frames = length;
				m_fade_at = 0;
				m_next_now = false;
				m_track_serial++;
			}
		}
		m_loop->read(m_scratch.data(), BLOCK_FRAMES);
		if (m_fade_frames > 0)
		{
			m_next->read(m_scratch_next.data(), BLOCK_FRAMES);
			for (unsigned i = 0; i < BLOCK_FRAMES; i++)
			{
				const double t = std::min(1.0, static_cast<double>(m_fade_at + i) / static_cast<double>(m_fade_frames));
				const float down = static_cast<float>(std::cos(t * PI / 2.0)), up = static_cast<float>(std::sin(t * PI / 2.0));
				m_scratch[i * 2] = m_scratch[i * 2] * down + m_scratch_next[i * 2] * up;
				m_scratch[i * 2 + 1] = m_scratch[i * 2 + 1] * down + m_scratch_next[i * 2 + 1] * up;
			}
			m_fade_at += BLOCK_FRAMES;
			if (m_fade_at >= m_fade_frames)
			{
				// The next one is the track now; the one before is freed by the caller, outside
				// the lock (it may be a hundred megabytes of blocks).
				m_retired = std::move(m_loop);
				m_loop = std::move(m_next);
				m_track_path = std::move(m_next_path);
				m_next_path.clear();
				m_fade_frames = 0;
				if (m_pending != nullptr)
				{
					m_next = std::move(m_pending);
					m_next_path = std::move(m_pending_path);
					m_pending_path.clear();
					m_next_now = true;
				}
			}
		}
		m_track_seconds = m_loop->seconds();
		// Another track begins to be heard: the interface may show its name.
		if (m_heard_track != m_track_serial)
		{
			m_heard_track = m_track_serial;
			m_heard_serial++;
			m_heard_path = (m_fade_frames > 0) ? m_next_path : m_track_path;
		}
	}

	const bool cues = (m_cues != nullptr) && [&] {
		std::fill(m_scratch_cues.begin(), m_scratch_cues.end(), 0.0f);
		return m_cues->mix(m_scratch_cues.data(), BLOCK_FRAMES, 2, DSP::OUTPUT_RATE);
	}();

	const float background_step = 1.0f / (1.5f * DSP::OUTPUT_RATE);
	const s16* const startup = (m_startup != nullptr && m_startup_enabled && m_startup_playing) ? m_startup->data() : nullptr;
	for (unsigned i = 0; i < BLOCK_FRAMES; i++)
	{
		float left = 0.0f, right = 0.0f;
		if (background)
		{
			// (An equal-power fade in: it is there early, and does not jump at the end.)
			m_background_gain = std::min(1.0f, m_background_gain + background_step);
			const float gain = m_background_volume * m_background_gain * (2.0f - m_background_gain);
			left = m_scratch[i * 2] * gain;
			right = m_scratch[i * 2 + 1] * gain;
		}
		if (m_startup_playing)
		{
			if (startup != nullptr && m_startup_at < startup_frames)
			{
				const float gain = m_startup_volume * m_startup_gain * (1.0f / 32768.0f);
				left += startup[m_startup_at * 2] * gain;
				right += startup[m_startup_at * 2 + 1] * gain;
			}
			m_startup_at++;
			m_startup_gain = std::clamp(m_startup_gain + m_startup_gain_step, 0.0f, 1.0f);
		}
		if (cues)
		{
			left += m_scratch_cues[i * 2];
			right += m_scratch_cues[i * 2 + 1];
		}
		if (m_master != m_master_target)
			m_master = std::clamp(m_master + m_master_step, 0.0f, 1.0f);
		left = DSP::SoftLimit(left) * m_master;
		right = DSP::SoftLimit(right) * m_master;
		// (NaN from a broken file must not reach the output: it compares false and becomes 0.)
		out[i * 2] = (left >= -1.0f && left <= 1.0f) ? static_cast<s16>(left * 32767.0f) : 0;
		out[i * 2 + 1] = (right >= -1.0f && right <= 1.0f) ? static_cast<s16>(right * 32767.0f) : 0;
	}
	// The start-up sound is over when its last frame is out, or when the skip has taken it away
	// (never before the sound was made, if it is still being made: its clock goes on meanwhile,
	// but not for ever).
	if (m_startup_playing)
	{
		const std::int64_t give_up = static_cast<std::int64_t>(12.0 * DSP::OUTPUT_RATE);
		if ((m_startup != nullptr && m_startup_at >= startup_frames) || m_startup_gain <= 0.0f || m_startup_at >= give_up || !m_startup_enabled)
		{
			m_startup_playing = false;
			m_background_wanted = m_background_wanted || m_startup_gain <= 0.0f;
		}
	}
}

// -------------------------------------------------------------------------------- the threads

namespace
{
#ifdef __PROSPERO__
	class ConsoleOutput final : public Output
	{
	public:
		bool open() override
		{
			static std::once_flag once;
			std::call_once(once, [] {
				// (The game's stream does this too; a second call says it is done already.)
				const int rc = sceAudioOutInit();
				UIAUDIO_LOG("sceAudioOutInit: 0x%x", static_cast<unsigned>(rc));
			});
			m_port = sceAudioOutOpen(0xff, 0, 0, BLOCK_FRAMES, DSP::OUTPUT_RATE, 1);
			if (m_port < 0)
			{
				UIAUDIO_LOG("the port did not open: 0x%x (the interface stays silent)", static_cast<unsigned>(m_port));
				m_port = -1;
				return false;
			}
			UIAUDIO_LOG("port %d open", m_port);
			return true;
		}
		bool write(const s16* frames) override { return m_port >= 0 && sceAudioOutOutput(m_port, frames) >= 0; }
		void close() override
		{
			if (m_port >= 0)
			{
				const int rc = sceAudioOutClose(m_port);
				UIAUDIO_LOG("port %d closed (0x%x)", m_port, static_cast<unsigned>(rc));
			}
			m_port = -1;
		}

	private:
		int m_port = -1;
	};
	ConsoleOutput s_console_output;
#endif

	// How long the game's stream goes on mixing the interface's sounds after the quick menu
	// closed: the Resume sound is a second long.
	constexpr double GAME_MIX_TAIL = 2.5;

	struct State
	{
		// Lock order: mutex, then cue_mutex. The game's audio thread takes cue_mutex alone, and
		// only by trying.
		std::mutex mutex;
		std::condition_variable wake;      // the mixing thread
		std::condition_variable load_wake; // the reading thread
		Mixer mixer;
		Output* output = nullptr;
		std::string folder;
		bool running = false;   // the mixing thread is there
		bool stopping = false;
		bool load_started = false, loaded = false;
		pthread_t mix_thread{};
		std::string described_startup = "Not looked for yet", described_music, described_cues;
		// The start-up animation's clock, for a sound that is made after the animation began.
		bool startup_began = false;
		std::chrono::steady_clock::time_point startup_at;
		bool startup_given = false;

		// The music: what the reading thread is asked to do.
		bool background_enabled = true;
		MusicOrder order = MusicOrder::Shuffle;
		bool kit_music = false;
		Playlist playlist;
		bool music_dirty = true;     // the playlist is to be made (again)
		bool music_released = false; // its memory went at a Stop: read the track again at a Start
		bool next_wanted = false, next_asked = false, skip_wanted = false;
		std::uint64_t seed = 0;

		std::mutex cue_mutex;
		CueVoices cues;
		std::atomic<bool> game_mix{false};
		std::atomic<std::int64_t> game_mix_until{0};
	};
	State& S()
	{
		static State* const state = [] {
			State* const s = new State();
			s->seed = static_cast<std::uint64_t>(NowNanoseconds()) | 1u;
			return s;
		}();
		return *state;
	}

	std::string Folder()
	{
		State& s = S();
		if (!s.folder.empty())
			return s.folder;
#ifdef __PROSPERO__
		return ps5px_title_dir();
#else
		return ".";
#endif
	}

	std::uint64_t NextSeed(State& s)
	{
		s.seed = s.seed * 6364136223846793005ull + 1442695040888963407ull;
		return s.seed;
	}

	// Frees players without the lock held.
	void ReleaseMusicLocked(State& s, std::unique_ptr<DSP::LoopPlayer>* a, std::unique_ptr<DSP::LoopPlayer>* b)
	{
		s.mixer.releaseMusic(a, b);
		s.music_released = true;
		s.next_asked = false;
	}

	void LoadStartup(State& s, const std::string& folder)
	{
		// The start-up sound: the one the title comes with, which its start-up animations are timed
		// to (resources/sounds/silicon-boot.wav), or startup.wav of the user's own in the title
		// folder in its place. Either is played as it is, its first eight seconds, the last two of
		// them fading under the library.
		const std::string own = folder + "/startup.wav";
		const std::string builtin = folder + "/resources/sounds/silicon-boot.wav";
		auto stereo = std::make_shared<std::vector<s16>>();
		DSP::StartupInfo made;
		const auto make = [&](const std::string& path, const char* name) {
			if (!DSP::FileExists(path.c_str()))
				return false;
			DSP::Sound sound;
			DSP::FileInfo info;
			std::string error;
			if (!DSP::LoadSound(path.c_str(), 30.0, &sound, &info, &error))
			{
				UIAUDIO_LOG("%s cannot be used: %s", name, error.c_str());
				return false;
			}
			UIAUDIO_LOG("%s: %s, %u Hz, %u channel(s), %.2f s", name, info.format.c_str(), info.rate, info.channels, info.seconds);
			return DSP::RenderPlain(sound, DSP::STARTUP_PLAIN_SECONDS, DSP::STARTUP_PLAIN_FADE, stereo.get(), &made);
		};
		const bool yours = make(own, "startup.wav");
		const bool have = yours || make(builtin, "the start-up sound");
		const std::lock_guard<std::mutex> lock(s.mutex);
		if (have)
		{
			UIAUDIO_LOG("start-up sound ready: %.2f s, peak %.2f", made.seconds, static_cast<double>(made.peak));
			s.mixer.setStartup(std::move(stereo));
			// The animation may have begun meanwhile: the sound joins it where it is.
			if (s.startup_began && !s.startup_given)
			{
				s.startup_given = true;
				s.mixer.playStartup(std::chrono::duration<double>(std::chrono::steady_clock::now() - s.startup_at).count());
			}
		}
		else
		{
			UIAUDIO_LOG("no start-up sound: %s is missing or cannot be read", builtin.c_str());
		}
		s.described_startup = yours ? "startup.wav (yours, in place of the title's own)" : have ? "the title's own start-up sound" : "no start-up sound";
	}

	void LoadCues(State& s, const std::string& folder)
	{
		std::shared_ptr<CueBank> bank = CueBank::Load(folder + "/resources/sounds");
		char line[96];
		std::snprintf(line, sizeof(line), "%d recordings of the interface's sounds%s", bank->files,
			bank->rejected > 0 ? " (some could not be read)" : "");
		UIAUDIO_LOG("%s", line);
		{
			const std::lock_guard<std::mutex> lock(s.cue_mutex);
			s.cues.setBank(std::move(bank));
		}
		const std::lock_guard<std::mutex> lock(s.mutex);
		s.described_cues = line;
	}

	// The playlist, made from the folders as they are now.
	void BuildPlaylist(State& s, const std::string& folder, MusicOrder order, bool kit_music, bool* need_track)
	{
		const std::vector<std::string> own = Playlist::ListFolder(folder + "/music");
		std::string background;
		for (const char* const name : {"background.wav", "background.mp3", "background.ogg", "background.flac"})
		{
			const std::string path = folder + "/" + name;
			if (DSP::FileExists(path.c_str()))
			{
				background = path;
				break;
			}
		}
		const std::vector<std::string> kit = Playlist::ListFolder(folder + "/resources/music");
		std::vector<std::string> sources = Playlist::Sources(own, background, kit, kit_music);

		char line[160];
		if (!own.empty() || !background.empty())
			std::snprintf(line, sizeof(line), "music: %zu track(s) of yours (music folder: %zu%s%s)", sources.size(), own.size(),
				background.empty() ? "" : ", ", background.empty() ? "" : Playlist::NameOf(background).c_str());
		else if (!sources.empty())
			std::snprintf(line, sizeof(line), "music: %zu track(s) of the kit's", sources.size());
		else
			std::snprintf(line, sizeof(line), "no music (none of yours%s)", kit.empty() || !kit_music ? "" : ", the kit's is off");
		UIAUDIO_LOG("%s, %s", line, order == MusicOrder::Shuffle ? "shuffled" : "in order");

		std::unique_ptr<DSP::LoopPlayer> a, b;
		{
			const std::lock_guard<std::mutex> lock(s.mutex);
			const std::string keep = s.mixer.trackPath();
			s.playlist.set(std::move(sources), order, NextSeed(s), keep);
			s.mixer.setPlaylistLength(s.playlist.size());
			s.described_music = line;
			s.next_asked = false;
			if (s.playlist.size() == 0)
			{
				ReleaseMusicLocked(s, &a, &b);
				*need_track = false;
			}
			else
			{
				// The track that plays goes on if it is still in the list; else the list's first
				// comes in over it.
				*need_track = !s.mixer.hasTrack() || !s.playlist.contains(keep);
				if (!s.mixer.hasTrack() && s.music_released && s.playlist.contains(keep))
					*need_track = false; // (read again from where it was, below)
			}
		}
	}

	// Reads a track and hands it to the mixer. `path` empty: the playlist's next ones are tried,
	// up to the whole list, until one can be read.
	void LoadTrack(State& s, std::string path, bool now, double start)
	{
		std::size_t tries;
		{
			const std::lock_guard<std::mutex> lock(s.mutex);
			tries = std::max<std::size_t>(s.playlist.size(), 1);
		}
		for (std::size_t attempt = 0; attempt < tries; attempt++)
		{
			if (path.empty())
			{
				const std::lock_guard<std::mutex> lock(s.mutex);
				path = s.playlist.next();
				start = 0.0;
			}
			if (path.empty())
				return;
			auto sound = std::make_shared<DSP::Sound>();
			DSP::FileInfo info;
			std::string error;
			if (DSP::LoadSound(path.c_str(), 12 * 60.0, sound.get(), &info, &error))
			{
				UIAUDIO_LOG("%s: %s, %u Hz, %u channel(s), %.1f s%s", Playlist::NameOf(path).c_str(), info.format.c_str(), info.rate, info.channels,
					info.seconds, info.cut ? " (cut there: it is longer)" : "");
				const std::lock_guard<std::mutex> lock(s.mutex);
				// (Not while the music is let go meanwhile: a Stop, or turned off.)
				if (s.running && s.background_enabled && !s.music_released)
				{
					s.mixer.queueTrack(std::move(sound), info.lossy || info.cut, path, now, start);
					s.next_asked = false;
				}
				s.wake.notify_all();
				return;
			}
			UIAUDIO_LOG("%s cannot be used: %s", path.c_str(), error.c_str());
			path.clear();
		}
		UIAUDIO_LOG("none of the playlist's tracks could be read");
	}

	// Reads the start-up sound and the interface's sounds once; then, for as long as the title
	// runs, the music's tracks as they are wanted. Nothing that plays waits for it.
	void* LoadMain(void*)
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
		State& s = S();
		const std::string folder = Folder();
		LoadStartup(s, folder);
		LoadCues(s, folder);
		{
			const std::lock_guard<std::mutex> lock(s.mutex);
			s.loaded = true;
		}
		s.wake.notify_all();

		for (;;)
		{
			bool rebuild, reload, next, skip, kit_music;
			MusicOrder order;
			std::string resume_path;
			double resume_at = 0.0;
			{
				std::unique_lock<std::mutex> lock(s.mutex);
				const auto wanted = [&] {
					const bool music_on = s.running && s.background_enabled;
					return music_on && (s.music_dirty || (s.music_released && !s.mixer.hasTrack()) || s.next_wanted || s.skip_wanted);
				};
				s.load_wake.wait(lock, wanted);
				rebuild = std::exchange(s.music_dirty, false);
				reload = s.music_released && !s.mixer.hasTrack();
				next = std::exchange(s.next_wanted, false);
				skip = std::exchange(s.skip_wanted, false);
				order = s.order;
				kit_music = s.kit_music;
				if (reload)
				{
					resume_path = s.mixer.trackPath();
					resume_at = s.mixer.trackSeconds();
				}
			}
			bool need_track = false;
			if (rebuild)
				BuildPlaylist(s, Folder(), order, kit_music, &need_track);
			{
				const std::lock_guard<std::mutex> lock(s.mutex);
				if (reload && !s.playlist.contains(resume_path))
					resume_path.clear(); // (it is not in the list any more: the list's next)
				s.music_released = false;
				if (s.playlist.size() == 0)
					continue;
			}
			if (reload)
				LoadTrack(s, resume_path, true, resume_at);
			else if (need_track || skip)
				LoadTrack(s, std::string(), true, 0.0);
			else if (next)
				LoadTrack(s, std::string(), false, 0.0);
		}
		return nullptr;
	}

	void* MixMain(void*)
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);
		State& s = S();
		Output* const output = s.output;
		bool open = output->open();
		s16 block[BLOCK_FRAMES * 2];
		for (;;)
		{
			std::unique_ptr<DSP::LoopPlayer> retired;
			{
				std::unique_lock<std::mutex> lock(s.mutex);
				if (s.stopping && (s.mixer.done() || !open))
					break;
				bool idle;
				{
					const std::lock_guard<std::mutex> cues(s.cue_mutex);
					idle = !s.stopping && s.mixer.idle();
					if (!idle)
						s.mixer.render(block);
				}
				if (idle)
				{
					// Nothing to hear: nothing is written, and the thread sleeps until there is.
					// (A cue's note may come between the look and the sleep without waking it:
					// with sounds on, the sleep is short.)
					const bool cues_on = s.cues.set() != SoundSet::Off;
					s.wake.wait_for(lock, std::chrono::milliseconds(cues_on ? 15 : 200));
					continue;
				}
				retired = s.mixer.takeRetired();
				if (!s.next_asked && s.mixer.wantsNextTrack())
				{
					s.next_asked = true;
					s.next_wanted = true;
					s.load_wake.notify_all();
				}
			}
			retired.reset();
			if (!open || !output->write(block))
			{
				// (No port: the mixer's clock still has to go on, at about the output's pace.)
				std::this_thread::sleep_for(std::chrono::microseconds(5333));
			}
		}
		if (open)
			output->close();
		return nullptr;
	}

	bool StartThread(pthread_t* thread, void* (*main)(void*))
	{
		// (No stack size is asked for: the platform layer then gives a thread 2 MiB of the large
		// memory, where a size that is asked for comes out of the title's small one.)
		const int rc = pthread_create(thread, nullptr, main, nullptr);
		if (rc != 0)
			UIAUDIO_LOG("a thread did not start: %d", rc);
		return rc == 0;
	}
} // namespace

void Start()
{
	State& s = S();
	std::unique_lock<std::mutex> lock(s.mutex);
	if (s.running)
		return;
#ifdef __PROSPERO__
	if (s.output == nullptr)
		s.output = &s_console_output;
#endif
	if (s.output == nullptr)
		return;
	s.mixer.setCues(&s.cues);
	if (!s.load_started)
	{
		s.load_started = true;
		// (The decoders and the start-up sound's making use the stack freely.)
		pthread_t thread;
		if (StartThread(&thread, &LoadMain))
			pthread_detach(thread);
		else
			s.loaded = true;
	}
	s.stopping = false;
	s.mixer.resume();
	if (StartThread(&s.mix_thread, &MixMain))
		s.running = true;
	// (The music, read again when a Stop let it go.)
	s.load_wake.notify_all();
}

void Stop()
{
	State& s = S();
	pthread_t thread;
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		if (!s.running)
			return;
		s.running = false;
		s.stopping = true;
		s.mixer.fadeOut(0.12);
		thread = s.mix_thread;
	}
	s.wake.notify_all();
	pthread_join(thread, nullptr);
	// The game needs the memory more: the music is read again, from where it was, after it. The
	// interface's sounds that were playing are not taken up again over the game.
	std::unique_ptr<DSP::LoopPlayer> a, b;
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		ReleaseMusicLocked(s, &a, &b);
		const std::lock_guard<std::mutex> cues(s.cue_mutex);
		s.cues.stopAll();
	}
}

void PlayStartup()
{
	State& s = S();
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		s.startup_began = true;
		s.startup_at = std::chrono::steady_clock::now();
		s.startup_given = true;
		s.mixer.playStartup(0.0);
	}
	s.wake.notify_all();
}

void SkipStartup()
{
	State& s = S();
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		s.mixer.skipStartup();
	}
	s.wake.notify_all();
}

void StartBackground()
{
	State& s = S();
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		s.mixer.startBackground();
	}
	s.wake.notify_all();
}

void SetVolume(float startup, float background)
{
	State& s = S();
	const std::lock_guard<std::mutex> lock(s.mutex);
	s.mixer.setVolume(startup, background);
}

void SetEnabled(bool startup, bool background)
{
	State& s = S();
	std::unique_ptr<DSP::LoopPlayer> a, b;
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		s.mixer.setEnabled(startup, background);
		if (s.background_enabled && !background && s.mixer.hasTrack())
			ReleaseMusicLocked(s, &a, &b); // (turned off: its memory goes; on again, it is read again)
		s.background_enabled = background;
	}
	s.wake.notify_all();
	s.load_wake.notify_all();
}

std::string Describe()
{
	State& s = S();
	const std::lock_guard<std::mutex> lock(s.mutex);
	if (!s.loaded)
		return "Being read...";
	std::string line = s.described_startup;
	if (!s.described_music.empty())
		line += ", " + s.described_music;
	if (!s.described_cues.empty())
		line += ", " + s.described_cues;
	return line;
}

void Cue(CueId cue)
{
	State& s = S();
	if (s.cues.post(cue))
		s.wake.notify_one();
}

void SetSoundSet(SoundSet set)
{
	S().cues.setSet(set);
}

void SetSoundVolume(float volume)
{
	S().cues.setVolume(volume);
}

void SetMusic(MusicOrder order, bool kit_music)
{
	State& s = S();
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		if (s.order == order && s.kit_music == kit_music)
			return;
		s.order = order;
		s.kit_music = kit_music;
		s.music_dirty = true;
	}
	s.load_wake.notify_all();
}

void NextTrack()
{
	State& s = S();
	{
		const std::lock_guard<std::mutex> lock(s.mutex);
		if (s.playlist.size() < 2 || !s.mixer.hasTrack() || s.mixer.crossfading())
			return;
		// (The next one is read already when the current one is near its end.)
		if (s.mixer.advanceNow())
			return;
		s.skip_wanted = true;
	}
	s.load_wake.notify_all();
}

TrackInfo CurrentTrack()
{
	State& s = S();
	const std::lock_guard<std::mutex> lock(s.mutex);
	TrackInfo info;
	if (s.mixer.hasTrack() && s.mixer.backgroundPlaying())
		info.name = Playlist::NameOf(s.mixer.heardPath());
	info.serial = s.mixer.heardSerial();
	return info;
}

void SetGameMixIn(bool open)
{
	State& s = S();
	if (open)
	{
		s.game_mix_until.store(0, std::memory_order_relaxed);
		s.game_mix.store(true, std::memory_order_release);
	}
	else
	{
		s.game_mix_until.store(NowNanoseconds() + static_cast<std::int64_t>(GAME_MIX_TAIL * 1e9), std::memory_order_relaxed);
		s.game_mix.store(false, std::memory_order_release);
	}
}

bool GameMixInWanted()
{
	State& s = S();
	if (s.game_mix.load(std::memory_order_acquire))
		return true;
	const std::int64_t until = s.game_mix_until.load(std::memory_order_relaxed);
	return until != 0 && NowNanoseconds() < until;
}

void MixIntoGame(float* frames, unsigned count, unsigned stride, unsigned rate)
{
	if (frames == nullptr || stride < 2 || !GameMixInWanted())
		return;
	State& s = S();
	// The game's audio thread never waits: when the lock is someone else's this moment (a settings
	// change, the interface's own thread at a hand-over), this write goes without the sounds.
	std::unique_lock<std::mutex> lock(s.cue_mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return;
	if (!s.cues.mix(frames, count, stride, rate))
		return;
	// The game's sound is at full scale: where the sum would go over, it is bent back, not
	// clipped (below the knee nothing changes).
	for (unsigned i = 0; i < count; i++)
	{
		frames[i * stride] = DSP::SoftLimit(frames[i * stride]);
		frames[i * stride + 1] = DSP::SoftLimit(frames[i * stride + 1]);
	}
}

namespace Testing
{
	void SetOutput(Output* output)
	{
		S().output = output;
	}
	void SetFolder(std::string folder)
	{
		State& s = S();
		const std::lock_guard<std::mutex> lock(s.mutex);
		s.folder = std::move(folder);
	}
	void SetSeed(std::uint64_t seed)
	{
		State& s = S();
		const std::lock_guard<std::mutex> lock(s.mutex);
		s.seed = seed;
	}
	void WaitUntilLoaded()
	{
		State& s = S();
		std::unique_lock<std::mutex> lock(s.mutex);
		s.wake.wait(lock, [&] { return s.loaded; });
	}
	bool WaitForTrack(double seconds)
	{
		State& s = S();
		const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
		std::unique_lock<std::mutex> lock(s.mutex);
		while (!s.mixer.hasTrack())
		{
			if (std::chrono::steady_clock::now() >= until)
				return false;
			s.wake.wait_for(lock, std::chrono::milliseconds(5));
		}
		return true;
	}
	std::vector<std::string> PlaylistPaths()
	{
		State& s = S();
		const std::lock_guard<std::mutex> lock(s.mutex);
		return s.playlist.paths();
	}
	void Rebuild()
	{
		State& s = S();
		{
			const std::lock_guard<std::mutex> lock(s.mutex);
			s.music_dirty = true;
		}
		s.load_wake.notify_all();
	}
} // namespace Testing
} // namespace PS5UIAudio
