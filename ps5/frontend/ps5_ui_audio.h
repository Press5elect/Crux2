// Crux2 - the interface's sound: the start-up sound, in time with the start-up animation; after it
// music, track after track, for as long as no game is loaded; and the short sounds of the interface
// itself (a move, a confirm, a dialog), over the music or, while the quick menu is open, over the
// game.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// What is played, all of it optional:
//
//   startup.wav                       the user's own start-up sound, in place of the title's
//                                     (resources/sounds/silicon-boot.wav)
//   music/*.wav .mp3 .ogg .flac       the user's music, in name order or shuffled ([PS5UI]
//                                     MusicOrder), crossfaded from one track into the next
//   background.wav / .mp3 / .ogg / .flac   the one background sound of the versions before: it
//                                     joins the playlist after the music folder's tracks
//   resources/music/*                 tracks a build may bring, played when the user has none of the
//                                     above ([PS5UI] KitMusic, off by default); Crux2's package brings none
//   resources/sounds/<set>/<cue>_NN.wav    the interface's sounds, the kit's two sets "glass" and
//                                     "paper" ([PS5UI] SoundSet); NN numbers a cue's variations
//
// The console's main output takes one port of a title at a time, and a running game has it
// (ps5_audio_stream.cpp). So the interface's port is open only while no game is loaded: Stop()
// before the emulator opens its own, Start() when the game is gone. While a game is loaded its
// stream mixes the interface's sounds into its own when the quick menu asks it to
// (SetGameMixIn, MixIntoGame); the music is not heard over a game.
//
// The mixing itself (Mixer, CueVoices, Playlist) is plain C++: ps5/tests/uiaudio runs it on the PC.

#pragma once

#include "ps5_ui_audio_dsp.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PS5UIAudio
{
/// Where the sound goes: the console's port, or a test's buffer.
struct Output
{
	virtual ~Output() = default;
	virtual bool open() = 0;
	/// 256 frames (left, right, ...). Blocks until the output has taken them: that is the pace.
	virtual bool write(const s16* frames) = 0;
	virtual void close() = 0;
};

constexpr unsigned BLOCK_FRAMES = 256;

// ------------------------------------------------------------------------- the interface's sounds

/// What happened in the interface, not which file plays: the sound set decides how it sounds. The
/// names are those of the kit's vocabulary (its src/audio/cues.hpp), whose recordings are used.
enum class CueId : std::uint8_t
{
	Focus,        // the highlight moved
	Select,       // confirm
	Back,         // cancel, or leave a screen
	Tab,          // another page, tab or filter
	Open,         // a screen or a drawer came in
	ModalOpen,    // a dialog or the quick menu appeared
	ModalClose,   // ... and went away
	Toggle,       // a switch was flipped
	Slider,       // a slider or a list of values moved one step
	Error,        // refused: the end of a list, something that cannot be done
	Notify,       // a notice came up
	Launch,       // a game starts
	FavouriteOn,  // starred
	FavouriteOff, // unstarred
	Saved,        // settings or a state were stored
	Welcome,      // the library appears for the first time
	Resume,       // back into the game
	Count
};
constexpr unsigned CUE_COUNT = static_cast<unsigned>(CueId::Count);

/// The kit's name of a cue, which is its recordings' file name ("modal_open" -> modal_open_01.wav).
const char* CueName(CueId cue);

/// Which recordings the cues are played from. Off: none are played.
enum class SoundSet : std::uint8_t
{
	Off,
	Glass, // soft, airy, tuned chimes (the kit's ProsperoEden set)
	Paper, // warm, wooden, tactile (the kit's ProsperoPuzzles set)
};
constexpr unsigned SOUND_SETS = 2; // the two that have recordings
/// "glass", "paper" ("" for Off): the folder of a set's recordings.
const char* SoundSetFolder(SoundSet set);

/// The recordings of every cue of both sets, made once and never changed after (voices point
/// into it while they play).
struct CueBank
{
	/// One recording: 16-bit stereo at its own rate.
	struct Clip
	{
		std::vector<s16> stereo;
		std::size_t frames = 0;
		unsigned rate = DSP::OUTPUT_RATE;
		bool tone = false; // made here: no set records this cue
	};

	/// [set - 1][cue]: the variations (_01, _02, ...) in order.
	std::array<std::array<std::vector<Clip>, CUE_COUNT>, SOUND_SETS> clips;
	/// The stand-ins for a cue neither set records: a short tone, as the kit has.
	std::array<std::vector<Clip>, CUE_COUNT> tones;
	int files = 0;    // recordings read
	int rejected = 0; // files that are there and could not be read

	/// Adds one recording. False when there is nothing in it.
	bool add(SoundSet set, CueId cue, const DSP::Sound& sound);
	/// Makes the stand-in tones (for every cue, used only where neither set records it).
	void makeTones();
	/// The variations to play a cue from: the set's own; else the other set's, as the kit falls
	/// back; else the tone. Null only for Off.
	const std::vector<Clip>* find(SoundSet set, CueId cue) const;
	/// Whether the set itself records the cue.
	bool records(SoundSet set, CueId cue) const;

	/// Reads root/glass/<cue>_NN.wav and root/paper/<cue>_NN.wav for the seventeen cues
	/// (NN from 01 up to the first that is not there).
	static std::shared_ptr<CueBank> Load(const std::string& root);
};

/// The voices the cues play in, and the queue they are asked for through.
///
/// post() is the only call for any thread: it never blocks and never allocates (a fixed ring of
/// requests; a full ring drops the request). Everything else is for the thread that renders, with
/// the owner's lock held (the interface's mixing thread, or the game's audio thread).
class CueVoices
{
public:
	/// Voices heard at once. A ninth cue takes the place of the oldest, which is faded out in 3 ms
	/// (in a slot of its own, so the new one does not wait).
	static constexpr unsigned MAX_VOICES = 8;
	/// At most this many voices of one cue at once (Focus while scrolling): the oldest of them
	/// gives way.
	static constexpr unsigned MAX_SAME = 2;
	static constexpr unsigned QUEUE = 64;
	/// A request older than this when the renderer gets to it is dropped (asked for while nothing
	/// was rendering: a game without the quick menu, a port being handed over).
	static constexpr double STALE_SECONDS = 0.25;

	CueVoices();

	// ---- any thread
	/// Asks for a cue. False when it was not queued (the set is Off, or the ring is full).
	bool post(CueId cue);
	void setSet(SoundSet set) { m_set.store(static_cast<int>(set), std::memory_order_relaxed); }
	SoundSet set() const { return static_cast<SoundSet>(m_set.load(std::memory_order_relaxed)); }
	/// 0..1.
	void setVolume(float volume);
	/// Whether there is anything to render (a voice playing, or a request waiting).
	bool busy() const { return m_busy.load(std::memory_order_relaxed) || pending(); }
	bool pending() const;

	// ---- the renderer, with the owner's lock held
	void setBank(std::shared_ptr<const CueBank> bank) { m_bank = std::move(bank); }
	const CueBank* bank() const { return m_bank.get(); }
	/// Takes the waiting requests and adds what the voices play to `frames`: `count` frames,
	/// `stride` floats apart, left and right the first two of each, at `rate` frames a second (the
	/// recordings are brought to it as they are played). Returns whether anything was added.
	bool mix(float* frames, std::size_t count, unsigned stride, unsigned rate);
	/// Silences every voice and forgets every request.
	void stopAll();

	// ---- for the tests and the log
	unsigned voices() const;      // playing, not counting those fading out after a steal
	unsigned long long started = 0, rateLimited = 0, stolen = 0, stale = 0, dropped = 0;
	int lastVariation(CueId cue) const { return m_last_variation[static_cast<unsigned>(cue)]; }
	/// The shortest time between two starts of one cue (Focus: 45 ms, ...).
	static double MinimumGap(CueId cue);

private:
	struct Request
	{
		std::atomic<unsigned> sequence{0};
		std::uint8_t cue = 0;
		std::int64_t when = 0; // steady clock, nanoseconds
	};
	struct Voice
	{
		const CueBank::Clip* clip = nullptr;
		double position = 0.0; // frames of the clip
		float pitch = 1.0f;    // the kit's small jitter: repeats do not sound mechanical
		float gain = 1.0f;
		std::uint8_t cue = 0;
		std::uint32_t age = 0;  // larger: started later
		int release = -1;       // frames of its fade-out left; -1: not fading
	};
	static constexpr unsigned SLOTS = MAX_VOICES + 4;
	static constexpr int RELEASE_FRAMES = 144; // 3 ms

	void start(CueId cue);
	std::uint32_t random();

	std::array<Request, QUEUE> m_queue;
	std::atomic<unsigned> m_head{0}; // the next request to write (any thread)
	unsigned m_tail = 0;             // the next to read (the renderer, with the lock)
	std::atomic<int> m_set{static_cast<int>(SoundSet::Glass)};
	std::atomic<float> m_volume{0.8f};
	std::atomic<bool> m_busy{false};

	std::shared_ptr<const CueBank> m_bank;
	std::array<Voice, SLOTS> m_voices{};
	std::uint32_t m_clock = 0;
	double m_time = 0.0; // seconds rendered: the clock the rate limits are counted by
	std::array<double, CUE_COUNT> m_last_start;
	std::array<int, CUE_COUNT> m_last_variation;
	std::uint32_t m_random = 0x2545f491u;
};

// ------------------------------------------------------------------------------------- the music

enum class MusicOrder : std::uint8_t
{
	InOrder, // by file name, round and round
	Shuffle, // a new order every round, never the same track twice in a row
};

/// Which tracks are played, and in what order.
class Playlist
{
public:
	/// The files the playlist is made of: the music folder's (in name order), then the old
	/// background file; the kit's tracks only when both are empty and they are allowed.
	static std::vector<std::string> Sources(const std::vector<std::string>& music_folder, const std::string& background,
		const std::vector<std::string>& kit, bool kit_music);
	/// The sound files in a folder (.wav, .mp3, .ogg, .flac; not the "._" files a Mac leaves),
	/// sorted by name, as paths. Empty when the folder is not there.
	static std::vector<std::string> ListFolder(const std::string& folder);
	/// The name a track is shown by: its file name without the folder and the extension.
	static std::string NameOf(const std::string& path);

	/// A new list. `keep`: the track playing now; when it is in the list, the order goes on after
	/// it (and, shuffled, it is not the next one).
	void set(std::vector<std::string> paths, MusicOrder order, std::uint64_t seed, const std::string& keep = std::string());
	std::size_t size() const { return m_paths.size(); }
	const std::vector<std::string>& paths() const { return m_paths; }
	MusicOrder order() const { return m_order; }
	bool contains(const std::string& path) const;
	/// The next track's path ("" for an empty list).
	const std::string& next();

private:
	void shuffle(std::size_t avoid_first);
	std::uint64_t random();

	std::vector<std::string> m_paths;
	std::vector<std::size_t> m_round; // the order of this round, as indices
	std::size_t m_at = 0;             // in m_round: the next one
	std::size_t m_last = static_cast<std::size_t>(-1); // index of the track handed out last
	MusicOrder m_order = MusicOrder::InOrder;
	std::uint64_t m_state = 1;
	static const std::string s_none;
};

/// What is heard, block by block. Everything here is called with the caller's lock held, or from
/// one thread.
class Mixer
{
public:
	/// Two tracks meet over this long (an equal-power fade: the end goes down by the cosine as the
	/// next comes up by the sine, which keeps the loudness).
	static constexpr double TRACK_CROSSFADE = 3.0;
	/// The next track is asked for this long before the current one ends (a long one takes a
	/// second or two to decode).
	static constexpr double NEXT_TRACK_LEAD = 25.0;

	/// The sounds, once they are read and made (either may be missing).
	void setStartup(std::shared_ptr<const std::vector<s16>> stereo);
	/// One background sound in place of whatever plays (no fade), round and round.
	void setBackground(std::shared_ptr<const DSP::Sound> sound, bool crossfade);
	/// A track of the playlist: plays at once when nothing does; else it is the next, and comes in
	/// over the end of the current one (or at once, crossfaded, when `now`). `start` seconds into it.
	void queueTrack(std::shared_ptr<const DSP::Sound> sound, bool loop_crossfade, std::string path, bool now, double start = 0.0);
	/// How many tracks the playlist has: with one, it goes round and round, and no next is asked for.
	void setPlaylistLength(std::size_t tracks) { m_playlist = tracks; }
	/// Whether the next track should be read now: the current one ends within NEXT_TRACK_LEAD and
	/// none is queued.
	bool wantsNextTrack() const;
	/// The music's memory goes (after a Stop, or when it is turned off): what was playing, and
	/// where, is kept for resumeTrack. The players are handed back, to be freed without the lock.
	void releaseMusic(std::unique_ptr<DSP::LoopPlayer>* current, std::unique_ptr<DSP::LoopPlayer>* next);
	/// The track that played (path) and how far into it, for after a releaseMusic.
	const std::string& trackPath() const { return m_track_path; }
	double trackSeconds() const { return m_track_seconds; }
	/// The track that is heard coming in or playing (while two meet: the one coming in), and a
	/// number that changes each time another one begins to be heard.
	const std::string& heardPath() const { return m_heard_path; }
	unsigned heardSerial() const { return m_heard_serial; }
	bool crossfading() const { return m_fade_frames > 0; }
	/// The queued next track comes in now (false: none is queued, or two tracks meet already).
	bool advanceNow();
	/// A track that has been crossfaded out, for the caller to free without the lock held.
	std::unique_ptr<DSP::LoopPlayer> takeRetired();

	void setVolume(float startup, float background);
	void setEnabled(bool startup, bool background);
	/// The interface's sounds are mixed in from here (the caller holds their lock too).
	void setCues(CueVoices* cues) { m_cues = cues; }

	/// The start-up animation is `seconds` into its clock (0 when it just began).
	void playStartup(double seconds);
	/// A button ended the animation: the start-up sound goes in a quarter of a second, and the
	/// background sound comes.
	void skipStartup();
	/// The library is there: the background sound may come (it waits for the start-up sound's
	/// tail if that is still playing).
	void startBackground();
	/// Everything fades out (Stop), in `seconds`; done() says when it has.
	void fadeOut(double seconds);
	/// The port is open again after a Stop: what was playing comes back in.
	void resume();

	/// Whether there is anything to hear now or soon: no, and the thread may wait.
	bool idle() const;
	bool done() const { return m_master_target == 0.0f && m_master == 0.0f; }
	/// The next block.
	void render(s16* out);

	/// For the log and the tests.
	double startupSeconds() const { return static_cast<double>(m_startup_at) / DSP::OUTPUT_RATE; }
	bool backgroundPlaying() const { return m_background_on; }
	bool hasTrack() const { return m_loop != nullptr; }
	bool hasNextTrack() const { return m_next != nullptr; }

private:
	std::int64_t crossfadeFrames() const;

	std::shared_ptr<const std::vector<s16>> m_startup;
	std::unique_ptr<DSP::LoopPlayer> m_loop, m_next;
	std::unique_ptr<DSP::LoopPlayer> m_pending; // asked for while two tracks met: it comes after
	std::unique_ptr<DSP::LoopPlayer> m_retired;
	std::string m_track_path, m_next_path, m_pending_path;
	double m_track_seconds = 0.0;
	bool m_next_now = false;
	std::int64_t m_fade_at = 0, m_fade_frames = 0; // 0 frames: no crossfade going on
	std::size_t m_playlist = 1;
	unsigned m_track_serial = 0; // another track came in
	unsigned m_heard_track = 0, m_heard_serial = 0;
	std::string m_heard_path;
	CueVoices* m_cues = nullptr;

	float m_startup_volume = 0.8f, m_background_volume = 0.4f;
	bool m_startup_enabled = true, m_background_enabled = true;

	bool m_startup_wanted = false;   // playStartup was called
	bool m_startup_playing = false;
	std::int64_t m_startup_at = 0;   // frames into the start-up sound (and its clock)
	float m_startup_gain = 1.0f, m_startup_gain_step = 0.0f; // 1 until it is skipped
	bool m_background_wanted = false;
	bool m_background_on = false;
	float m_background_gain = 0.0f;  // 0..1: its fade in
	float m_master = 1.0f, m_master_target = 1.0f, m_master_step = 0.0f;
	std::vector<float> m_scratch, m_scratch_next, m_scratch_cues;
};

// ------------------------------------------------------------------------------- the calls to make

/// Starts the thread that mixes and opens the port; the first time, also the thread that reads
/// the files (nothing here waits for a file). Cheap when already started.
void Start();
/// Fades out (120 ms), closes the port and ends the thread, and lets the music's memory go (it is
/// read again, from where it was, by the next Start). Returns when the port is closed.
void Stop();
/// The start-up animation's time 0.
void PlayStartup();
void SkipStartup();
void StartBackground();
/// 0..1 each. The background's is the music's.
void SetVolume(float startup, float background);
void SetEnabled(bool startup, bool background);
/// One line for a settings page: what was found ("the title's own start-up sound, music: 3
/// tracks of the kit's, sounds: glass").
std::string Describe();

/// Plays an interface sound. Any thread, every frame if need be: it never blocks (it leaves a
/// note for whichever thread renders). Too many of one cue too fast are thinned out there.
void Cue(CueId cue);
/// The sounds' set (Off: none) and their volume, 0..1.
void SetSoundSet(SoundSet set);
void SetSoundVolume(float volume);
/// The playlist's order, and whether the kit's music plays when the user has none. A change is
/// taken up at once (the track that plays goes on when it is still in the list).
void SetMusic(MusicOrder order, bool kit_music);
/// The next track now, crossfaded (a button on a settings page, say).
void NextTrack();

/// The track that plays: its name (file name without extension, "" when there is none) and a
/// number that changes with each new track, for the interface to show the name for a moment.
struct TrackInfo
{
	std::string name;
	unsigned serial = 0;
};
TrackInfo CurrentTrack();

/// While a game is loaded: the quick menu opened (true) or closed (false). While it is open, the
/// game's audio thread mixes the interface's sounds into the game's (and keeps writing while the
/// game is paused); after it closes, for 2.5 s more, so the last sound (Resume) is heard out.
void SetGameMixIn(bool open);
/// For ps5_audio_stream.cpp, once per write: whether MixIntoGame has anything to do. One atomic
/// read, and a clock read while the menu has just closed.
bool GameMixInWanted();
/// For ps5_audio_stream.cpp: adds the interface's sounds to the game's `count` frames (floats,
/// `stride` apart, left and right first) at `rate` frames a second. Never blocks: when another
/// thread holds the sounds this time, this write goes without them.
void MixIntoGame(float* frames, unsigned count, unsigned stride, unsigned rate);

namespace Testing
{
	/// Another output than the console's port (before Start), and another folder than the title's.
	void SetOutput(Output* output);
	void SetFolder(std::string folder);
	/// The shuffle's seed (else the clock's).
	void SetSeed(std::uint64_t seed);
	/// Waits until the files are read and the start-up sound is made.
	void WaitUntilLoaded();
	/// Waits until the playlist has a track playing (or `seconds` went by). False when none came.
	bool WaitForTrack(double seconds);
	/// The playlist as it is now (paths).
	std::vector<std::string> PlaylistPaths();
	/// Makes the playlist again from the folders (after the test changed them).
	void Rebuild();
} // namespace Testing
} // namespace PS5UIAudio
