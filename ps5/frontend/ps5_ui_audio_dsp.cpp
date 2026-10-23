// Crux2 - the interface's sound: reading sound files, bringing them to the output's rate, and
// making the start-up sound.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The start-up sound is the one of the approved mock-up (a Python script made that one offline):
// the user's own sound as a "bed" - opened by a low-pass in its first half second, widened, in
// a hall, drawn shut for a moment at the teleport and faded under the library - and over it
// effects made here, in time with the animation: a whoosh for each letter that flies in, a thud
// and a bell where it lands, the icon's drop, the teleport. The package cannot contain the
// user's sound, so the title makes the whole thing when it starts.
//
// Making the start-up sound runs on a worker thread, once, and is not written for speed; only
// LoopPlayer::read and SoftLimit run for every block that goes out. All of it uses only what the
// title already imports from the console's libc (malloc, fopen/fread/fseek/ftell/fclose,
// sin/cos/exp/log/pow/sqrt): no tanh, no round, no lrint. The title is compiled without
// exceptions, so the large allocations are asked for with nothrow and checked.

#include "ps5_ui_audio_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace PS5UIAudio::DSP
{
namespace
{
	constexpr double PI = 3.14159265358979323846;
	constexpr double SR = static_cast<double>(OUTPUT_RATE);

	// ---------------------------------------------------------------- files into memory

	// To 16 bits, from a value already in 16-bit units. (Not lrintf: the console's libc is not
	// asked for it, and a NaN must come out as silence, not as whatever the conversion gives.)
	s16 RoundS16(float value)
	{
		if (!(value == value))
			return 0;
		value = std::clamp(value, -32768.0f, 32767.0f);
		return static_cast<s16>(static_cast<int>(value + ((value < 0.0f) ? -0.5f : 0.5f)));
	}

	// Takes what a decoder hands out, mixes it down to stereo and stores it as 16 bits.
	class SoundWriter final : public FrameReceiver
	{
	public:
		SoundWriter(Sound* sound, double max_seconds)
			: m_sound(sound)
			, m_max_seconds(max_seconds)
		{
		}

		bool begin(unsigned rate, unsigned channels, Order order) override
		{
			if (rate < 1000 || rate > 768000)
			{
				m_error = "a sample rate of " + std::to_string(rate) + " Hz cannot be played";
				return false;
			}
			if (channels < 1 || channels > MAX_CHANNELS)
			{
				m_error = std::to_string(channels) + " channels cannot be played";
				return false;
			}

			m_sound->rate = rate;
			m_rate = rate;
			m_channels = channels;
			// No more memory than max_seconds at the output's rate take: a file at a higher rate
			// is cut earlier.
			m_max_frames = static_cast<std::uint64_t>(m_max_seconds * static_cast<double>(std::min(rate, OUTPUT_RATE)));
			setWeights(order);
			return true;
		}

		bool frames(const float* interleaved, std::size_t count) override
		{
			s16 block[CHUNK * 2];
			while (count > 0)
			{
				const std::uint64_t room = m_max_frames - std::min(m_max_frames, m_sound->stored);
				const std::size_t now = static_cast<std::size_t>(std::min<std::uint64_t>(std::min<std::size_t>(count, CHUNK), room));
				if (now == 0)
				{
					m_cut = true;
					return false;
				}

				for (std::size_t i = 0; i < now; i++)
				{
					float left = 0.0f, right = 0.0f;
					for (unsigned c = 0; c < m_channels; c++)
					{
						// A float file may hold anything: a NaN is silence, and an infinity must
						// not meet a weight of zero (which would make a NaN of it).
						float sample = interleaved[c];
						if (!(sample >= -8.0f))
							sample = (sample == sample) ? -8.0f : 0.0f;
						else if (sample > 8.0f)
							sample = 8.0f;
						left += sample * m_left[c];
						right += sample * m_right[c];
					}
					// (32768, so that a 16-bit file comes through as it is.)
					block[i * 2 + 0] = RoundS16(left * 32768.0f);
					block[i * 2 + 1] = RoundS16(right * 32768.0f);
					interleaved += m_channels;
				}

				if (!m_sound->append(block, now))
				{
					m_error = "out of memory after " + std::to_string(m_sound->stored / std::max(m_rate, 1u)) + " s";
					m_cut = true;
					return false;
				}
				count -= now;
			}
			return true;
		}

		bool cut() const { return m_cut; }
		unsigned channels() const { return m_channels; }
		const std::string& error() const { return m_error; }

	private:
		static constexpr unsigned MAX_CHANNELS = 32;
		static constexpr std::size_t CHUNK = 1024;

		// Which speaker each channel of a file with more than two is for: L and R the front
		// pair, C the centre, F the bass channel (left out, as a TV's own downmix does), l and r
		// the surrounds, c a centre surround.
		void setWeights(Order order)
		{
			static const char* const wav[] = {"C", "LR", "LRC", "LRlr", "LRClr", "LRCFlr", "LRCFclr", "LRCFlrlr"};
			static const char* const vorbis[] = {"C", "LR", "LCR", "LRlr", "LCRlr", "LCRlrF", "LCRlrcF", "LCRlrlrF"};
			// More than eight channels have no agreed layout: the first two are the stereo pair.
			const char* roles = (m_channels > 8) ? "LR" : ((order == Order::Vorbis) ? vorbis : wav)[m_channels - 1];
			const std::size_t known = std::strlen(roles);

			float sum_left = 0.0f, sum_right = 0.0f;
			for (unsigned c = 0; c < m_channels; c++)
			{
				float left = 0.0f, right = 0.0f;
				switch ((c < known) ? roles[c] : 'F')
				{
					case 'L': left = 1.0f; break;
					case 'R': right = 1.0f; break;
					case 'C': left = right = (m_channels == 1) ? 1.0f : 0.70710678f; break;
					case 'l': left = 0.70710678f; break;
					case 'r': right = 0.70710678f; break;
					case 'c': left = right = 0.5f; break;
					default: break;
				}
				m_left[c] = left;
				m_right[c] = right;
				sum_left += left;
				sum_right += right;
			}

			// So that every channel at full level together still fits.
			const float scale = 1.0f / std::max(1.0f, std::max(sum_left, sum_right));
			for (unsigned c = 0; c < m_channels; c++)
			{
				m_left[c] *= scale;
				m_right[c] *= scale;
			}
		}

		Sound* m_sound;
		double m_max_seconds;
		std::uint64_t m_max_frames = 0;
		unsigned m_rate = 0;
		unsigned m_channels = 0;
		float m_left[MAX_CHANNELS] = {};
		float m_right[MAX_CHANNELS] = {};
		bool m_cut = false;
		std::string m_error;
	};

	std::uint32_t LE16(const unsigned char* p)
	{
		return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
	}

	std::uint32_t LE32(const unsigned char* p)
	{
		return LE16(p) | (LE16(p + 2) << 16);
	}

	struct File
	{
		std::FILE* handle = nullptr;
		~File()
		{
			if (handle)
				std::fclose(handle);
		}
	};

	// An MP3 or Vorbis file does not begin and end where its sound does: the encoder's lead-in
	// and padding are near-silence, which would be a gap in the loop.
	void TrimSilence(Sound* sound)
	{
		constexpr int QUIET = 4; // of 32768
		const std::uint64_t most = std::min<std::uint64_t>(sound->rate * 150ull / 1000, sound->frames / 8);

		std::uint64_t lead = 0;
		while (lead < most)
		{
			const s16* frame = sound->frame(lead);
			if (std::abs(frame[0]) > QUIET || std::abs(frame[1]) > QUIET)
				break;
			lead++;
		}
		std::uint64_t tail = 0;
		while (tail < most)
		{
			const s16* frame = sound->frame(sound->frames - 1 - tail);
			if (std::abs(frame[0]) > QUIET || std::abs(frame[1]) > QUIET)
				break;
			tail++;
		}

		sound->first += lead;
		sound->frames -= lead + tail;
	}

	bool HasExtension(const char* path, const char* extension)
	{
		const std::size_t length = std::strlen(path), wanted = std::strlen(extension);
		if (length < wanted)
			return false;
		for (std::size_t i = 0; i < wanted; i++)
		{
			char c = path[length - wanted + i];
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			if (c != extension[i])
				return false;
		}
		return true;
	}
} // namespace

bool Sound::append(const s16* stereo, std::size_t count)
{
	while (count > 0)
	{
		const std::size_t offset = static_cast<std::size_t>(stored & (BLOCK_FRAMES - 1));
		if ((stored >> BLOCK_SHIFT) >= blocks.size())
		{
			s16* const block = new (std::nothrow) s16[BLOCK_FRAMES * 2];
			if (!block)
				return false;
			blocks.emplace_back(block);
		}

		const std::size_t now = std::min(count, BLOCK_FRAMES - offset);
		std::memcpy(&blocks[stored >> BLOCK_SHIFT][offset * 2], stereo, now * 2 * sizeof(s16));
		stereo += now * 2;
		stored += now;
		count -= now;
	}
	return true;
}

bool DecodeWavFile(const char* path, FrameReceiver& to, std::string* format, std::string* error)
{
	File file;
	file.handle = std::fopen(path, "rb");
	if (!file.handle)
	{
		*error = "cannot be opened";
		return false;
	}

	unsigned char header[12];
	if (std::fread(header, 1, sizeof(header), file.handle) != sizeof(header) || std::memcmp(header, "RIFF", 4) != 0 ||
		std::memcmp(header + 8, "WAVE", 4) != 0)
	{
		*error = "not a RIFF WAVE file";
		return false;
	}

	unsigned tag = 0, channels = 0, rate = 0, align = 0, bits = 0;
	for (;;)
	{
		unsigned char chunk[8];
		if (std::fread(chunk, 1, sizeof(chunk), file.handle) != sizeof(chunk))
		{
			*error = (tag == 0) ? "it has no format chunk" : "it has no data chunk";
			return false;
		}
		const std::uint32_t size = LE32(chunk + 4);

		if (std::memcmp(chunk, "fmt ", 4) == 0)
		{
			unsigned char fmt[40] = {};
			const std::size_t take = std::min<std::size_t>(size, sizeof(fmt));
			if (take < 16 || std::fread(fmt, 1, take, file.handle) != take)
			{
				*error = "its format chunk is cut short";
				return false;
			}
			tag = LE16(fmt);
			channels = LE16(fmt + 2);
			rate = LE32(fmt + 4);
			align = LE16(fmt + 12);
			bits = LE16(fmt + 14);
			// WAVE_FORMAT_EXTENSIBLE: what it really holds is the first field of its GUID.
			if (tag == 0xfffe && take >= 26)
				tag = LE16(fmt + 24);
			if (std::fseek(file.handle, static_cast<long>(size - take + (size & 1)), SEEK_CUR) != 0)
			{
				*error = "its format chunk is cut short";
				return false;
			}
			continue;
		}

		if (std::memcmp(chunk, "data", 4) != 0)
		{
			// (Chunks are padded to an even size.)
			if (std::fseek(file.handle, static_cast<long>(size) + static_cast<long>(size & 1), SEEK_CUR) != 0)
			{
				*error = "it has no data chunk";
				return false;
			}
			continue;
		}

		// The data chunk.
		const bool is_float = (tag == 3);
		if (tag == 0)
		{
			*error = "its data comes before its format";
			return false;
		}
		if (tag != 1 && tag != 3)
		{
			char text[64];
			std::snprintf(text, sizeof(text), "WAV format tag 0x%04x is not PCM", tag);
			*error = text;
			return false;
		}
		if ((!is_float && bits != 8 && bits != 16 && bits != 24 && bits != 32) || (is_float && bits != 32 && bits != 64))
		{
			*error = std::to_string(bits) + (is_float ? "-bit floats" : "-bit PCM") + " cannot be read";
			return false;
		}
		const unsigned bytes = bits / 8;
		if (channels == 0 || channels > 32)
		{
			*error = std::to_string(channels) + " channels cannot be played";
			return false;
		}
		// PCM's block size is its channels times its sample size; the header's own field is not
		// believed (a wrong one would otherwise decide how much memory is asked for below).
		align = channels * bytes;

		*format = std::string("WAV ") + (is_float ? "float " : "PCM ") + std::to_string(bits) + "-bit";
		if (!to.begin(rate, channels, FrameReceiver::Order::Wav))
			return false;

		// A size of 0 or of all ones is a file written as a stream: it goes to the file's end.
		std::uint64_t left = (size == 0 || size == 0xffffffffu) ? ~std::uint64_t(0) : size / align;
		constexpr std::size_t CHUNK = 2048;
		std::vector<unsigned char> raw(CHUNK * align);
		std::vector<float> samples(CHUNK * channels);
		std::uint64_t total = 0;
		while (left > 0)
		{
			const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, CHUNK));
			const std::size_t got = std::fread(raw.data(), align, want, file.handle);
			if (got == 0)
				break; // the file is shorter than its header says: what was there is the sound

			for (std::size_t i = 0; i < got; i++)
			{
				const unsigned char* in = &raw[i * align];
				float* out = &samples[i * channels];
				for (unsigned c = 0; c < channels; c++, in += bytes)
				{
					switch (bits + (is_float ? 1000 : 0))
					{
						case 8:
							out[c] = (static_cast<int>(in[0]) - 128) * (1.0f / 128.0f);
							break;
						case 16:
							out[c] = static_cast<s16>(LE16(in)) * (1.0f / 32768.0f);
							break;
						case 24:
							out[c] = static_cast<float>(static_cast<std::int32_t>((LE16(in) | (static_cast<std::uint32_t>(in[2]) << 16)) << 8) >> 8) *
									 (1.0f / 8388608.0f);
							break;
						case 32:
							out[c] = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(LE32(in))) * (1.0 / 2147483648.0));
							break;
						case 1032:
						{
							const std::uint32_t word = LE32(in);
							std::memcpy(&out[c], &word, sizeof(word));
							break;
						}
						default:
						{
							const std::uint64_t word = LE32(in) | (static_cast<std::uint64_t>(LE32(in + 4)) << 32);
							double value;
							std::memcpy(&value, &word, sizeof(word));
							// (Too large for a float is too loud anyway; it is clamped later.)
							out[c] = static_cast<float>(std::clamp(value, -8.0, 8.0));
							if (!(value == value))
								out[c] = 0.0f;
							break;
						}
					}
				}
			}

			total += got;
			left -= got;
			if (!to.frames(samples.data(), got))
				break;
			if (got < want)
				break;
		}

		if (total == 0)
		{
			*error = "there is no sound in it";
			return false;
		}
		return true;
	}
}

bool ReadFile(const char* path, std::size_t max_bytes, std::unique_ptr<unsigned char[]>* data, std::size_t* size, bool* whole,
	std::string* error)
{
	data->reset();
	*size = 0;
	*whole = true;

	File file;
	file.handle = std::fopen(path, "rb");
	if (!file.handle)
	{
		*error = "cannot be opened";
		return false;
	}
	long length = -1;
	if (std::fseek(file.handle, 0, SEEK_END) == 0)
		length = std::ftell(file.handle);
	if (length <= 0 || std::fseek(file.handle, 0, SEEK_SET) != 0)
	{
		*error = (length == 0) ? "it is empty" : "its size cannot be told";
		return false;
	}

	std::size_t take = static_cast<std::size_t>(length);
	if (take > max_bytes)
	{
		take = max_bytes;
		*whole = false;
	}
	data->reset(new (std::nothrow) unsigned char[take]);
	if (!*data)
	{
		*error = "no memory for its " + std::to_string(take >> 20) + " MiB";
		return false;
	}

	// (In pieces: one read of hundreds of megabytes is not something every file system likes.)
	std::size_t got = 0;
	while (got < take)
	{
		const std::size_t now = std::fread(data->get() + got, 1, std::min<std::size_t>(take - got, std::size_t(1) << 20), file.handle);
		if (now == 0)
			break;
		got += now;
	}
	if (got == 0)
	{
		data->reset();
		*error = "it cannot be read";
		return false;
	}
	if (got < take)
		*whole = false;
	*size = got;
	return true;
}

bool FileExists(const char* path)
{
	std::FILE* const file = std::fopen(path, "rb");
	if (!file)
		return false;
	std::fclose(file);
	return true;
}

bool LoadSound(const char* path, double max_seconds, Sound* sound, FileInfo* info, std::string* error)
{
	*sound = Sound();
	*info = FileInfo();
	error->clear();

	SoundWriter writer(sound, max_seconds);
	bool ok;
	if (HasExtension(path, ".wav"))
		ok = DecodeWavFile(path, writer, &info->format, error);
	else if (HasExtension(path, ".mp3"))
		ok = DecodeMp3File(path, writer, &info->format, error), info->lossy = true;
	else if (HasExtension(path, ".ogg"))
		ok = DecodeVorbisFile(path, writer, &info->format, error), info->lossy = true;
	else if (HasExtension(path, ".flac"))
		ok = DecodeFlacFile(path, writer, &info->format, error);
	else
		ok = false, *error = "not a kind of sound file this reads";

	if (!writer.error().empty())
		*error = writer.error();
	if (ok && sound->stored == 0)
	{
		ok = false;
		if (error->empty())
			*error = "there is no sound in it";
	}
	if (!ok)
	{
		if (error->empty())
			*error = "it could not be decoded";
		*sound = Sound();
		return false;
	}

	sound->first = 0;
	sound->frames = sound->stored;
	if (info->lossy)
		TrimSilence(sound);

	info->rate = sound->rate;
	info->channels = writer.channels();
	info->seconds = static_cast<double>(sound->frames) / static_cast<double>(sound->rate);
	info->cut = writer.cut();
	return true;
}

// ---------------------------------------------------------------- from one rate to another

namespace
{
	// The modified Bessel function the Kaiser window is made of.
	double BesselI0(double x)
	{
		double sum = 1.0, term = 1.0;
		for (int k = 1; k < 64; k++)
		{
			term *= (x / (2.0 * k)) * (x / (2.0 * k));
			sum += term;
			if (term < sum * 1e-17)
				break;
		}
		return sum;
	}
} // namespace

SincKernel::SincKernel(double from_rate, double to_rate)
{
	m_step = from_rate / to_rate;
	// Going down, the low-pass has to end at the output's half rate, not the input's: it is
	// that much longer, measured in input frames.
	const double down = std::max(1.0, m_step);
	unsigned taps = static_cast<unsigned>(std::ceil(64.0 * down));
	taps = std::min(taps, 512u);
	taps += taps & 1;
	m_taps = taps;

	// 64 frames and this window: about 80 dB down outside the pass band, which ends some 4 %
	// short of the half rate.
	const double cutoff = 0.96 / down;
	const double half = static_cast<double>(taps) / 2.0;
	const double beta = 7.857;
	const double i0_beta = BesselI0(beta);

	m_table.resize(static_cast<std::size_t>(PHASES + 1) * taps);
	for (unsigned phase = 0; phase <= PHASES; phase++)
	{
		const double fraction = static_cast<double>(phase) / PHASES;
		float* const row = &m_table[static_cast<std::size_t>(phase) * taps];
		double sum = 0.0;
		for (unsigned k = 0; k < taps; k++)
		{
			// How far input frame k of the window is from the position.
			const double x = static_cast<double>(static_cast<int>(k) - static_cast<int>(taps / 2) + 1) - fraction;
			const double u = x / half;
			const double window = (std::fabs(u) < 1.0) ? BesselI0(beta * std::sqrt(1.0 - u * u)) / i0_beta : 0.0;
			const double y = PI * cutoff * x;
			const double sinc = (std::fabs(y) < 1e-9) ? 1.0 : std::sin(y) / y;
			const double value = cutoff * sinc * window;
			row[k] = static_cast<float>(value);
			sum += value;
		}
		// A steady level stays exactly that level at every position.
		for (unsigned k = 0; k < taps; k++)
			row[k] = static_cast<float>(row[k] / sum);
	}
}

void SincKernel::weights(double fraction, float* out) const
{
	double scaled = fraction * PHASES;
	unsigned phase = static_cast<unsigned>(scaled);
	if (phase >= PHASES)
		phase = PHASES - 1;
	const float blend = static_cast<float>(std::clamp(scaled - phase, 0.0, 1.0));
	const float* const a = &m_table[static_cast<std::size_t>(phase) * m_taps];
	const float* const b = a + m_taps;
	for (unsigned k = 0; k < m_taps; k++)
		out[k] = a[k] + (b[k] - a[k]) * blend;
}

std::vector<float> ResampleMono(const std::vector<float>& in, double from_rate, double to_rate)
{
	const SincKernel kernel(from_rate, to_rate);
	const unsigned taps = kernel.taps();
	const std::int64_t size = static_cast<std::int64_t>(in.size());
	const std::size_t count = static_cast<std::size_t>(std::floor(static_cast<double>(in.size()) * to_rate / from_rate));

	std::vector<float> out(count);
	std::vector<float> weights(taps);
	for (std::size_t j = 0; j < count; j++)
	{
		const double position = static_cast<double>(j) * kernel.step();
		const std::int64_t base = static_cast<std::int64_t>(std::floor(position));
		kernel.weights(position - static_cast<double>(base), weights.data());

		const std::int64_t first = base - taps / 2 + 1;
		const std::int64_t from = std::max<std::int64_t>(first, 0), to = std::min<std::int64_t>(first + taps, size);
		float sum = 0.0f;
		for (std::int64_t i = from; i < to; i++)
			sum += in[static_cast<std::size_t>(i)] * weights[static_cast<std::size_t>(i - first)];
		out[j] = sum;
	}
	return out;
}

// ---------------------------------------------------------------- a sound, round and round

LoopPlayer::LoopPlayer(std::shared_ptr<const Sound> sound, bool crossfade)
	: m_sound(std::move(sound))
{
	const std::uint64_t frames = m_sound->frames;
	// 80 ms, in which the end goes down and the beginning comes up: the two are different
	// sounds, so by the sine and the cosine, which keeps the loudness level.
	m_fade = crossfade ? std::min<std::uint64_t>(m_sound->rate * 80ull / 1000, frames / 4) : 0;
	m_period = std::max<std::uint64_t>(frames - m_fade, 1);

	if (m_sound->rate != OUTPUT_RATE)
	{
		m_kernel = std::make_unique<SincKernel>(static_cast<double>(m_sound->rate), SR);
		m_weights.resize(m_kernel->taps());
		m_window.reserve((static_cast<std::size_t>(std::ceil(m_kernel->step() * 256.0)) + m_kernel->taps() + 4) * 2);
	}
}

void LoopPlayer::seek(double seconds)
{
	const double period = static_cast<double>(m_period);
	double position = std::max(seconds, 0.0) * static_cast<double>(m_sound->rate);
	position = std::fmod(position, period);
	// (A sound at the output's rate is read from whole frames.)
	m_position = m_kernel ? position : std::floor(position);
}

// Frames of the endless sound: `first` may be before its beginning or many rounds in.
void LoopPlayer::fetch(std::int64_t first, std::size_t count, float* stereo) const
{
	const std::int64_t period = static_cast<std::int64_t>(m_period);
	std::uint64_t at = static_cast<std::uint64_t>(((first % period) + period) % period);
	constexpr float SCALE = 1.0f / 32768.0f;

	for (std::size_t i = 0; i < count; i++)
	{
		const s16* const frame = m_sound->frame(at);
		if (at < m_fade)
		{
			// The beginning of a round: the sound's last frames are still going out under it.
			const double phase = (static_cast<double>(at) + 0.5) / static_cast<double>(m_fade) * (PI / 2.0);
			const float in = static_cast<float>(std::sin(phase)) * SCALE, out = static_cast<float>(std::cos(phase)) * SCALE;
			const s16* const tail = m_sound->frame(m_period + at);
			stereo[0] = frame[0] * in + tail[0] * out;
			stereo[1] = frame[1] * in + tail[1] * out;
		}
		else
		{
			stereo[0] = frame[0] * SCALE;
			stereo[1] = frame[1] * SCALE;
		}
		stereo += 2;
		if (++at == m_period)
			at = 0;
	}
}

void LoopPlayer::read(float* stereo, std::size_t count)
{
	if (!m_kernel)
	{
		// At the output's rate already: its frames are the output's.
		const std::uint64_t at = static_cast<std::uint64_t>(m_position);
		fetch(static_cast<std::int64_t>(at), count, stereo);
		m_position = static_cast<double>((at + count) % m_period);
		return;
	}

	const unsigned taps = m_kernel->taps();
	const std::int64_t half = taps / 2;
	const double step = m_kernel->step();
	const double period = static_cast<double>(m_period);
	while (count > 0)
	{
		// The input frames all of this stretch is made of, once; then each output frame from
		// its part of them. The position's fraction is carried over the seam, so a sound at
		// any rate goes round without a step in time.
		const std::size_t now = std::min<std::size_t>(count, 256);
		const std::int64_t first = static_cast<std::int64_t>(std::floor(m_position)) - half + 1;
		const std::int64_t last = static_cast<std::int64_t>(std::floor(m_position + static_cast<double>(now - 1) * step)) + half;
		m_window.resize(static_cast<std::size_t>(last - first + 1) * 2);
		fetch(first, static_cast<std::size_t>(last - first + 1), m_window.data());

		for (std::size_t i = 0; i < now; i++)
		{
			const double position = m_position + static_cast<double>(i) * step;
			const std::int64_t base = static_cast<std::int64_t>(std::floor(position));
			m_kernel->weights(position - static_cast<double>(base), m_weights.data());

			const float* const in = &m_window[static_cast<std::size_t>(base - half + 1 - first) * 2];
			float left = 0.0f, right = 0.0f;
			for (unsigned k = 0; k < taps; k++)
			{
				left += in[k * 2 + 0] * m_weights[k];
				right += in[k * 2 + 1] * m_weights[k];
			}
			stereo[i * 2 + 0] = left;
			stereo[i * 2 + 1] = right;
		}

		m_position += static_cast<double>(now) * step;
		while (m_position >= period)
			m_position -= period;
		stereo += now * 2;
		count -= now;
	}
}

// ---------------------------------------------------------------- the start-up sound

namespace
{
	// The user's sound is used up to here; a longer one is faded out by then.
	constexpr double BED_MAX_SECONDS = 7.0;
	constexpr double BED_FADE_FROM = 5.0;
	constexpr double HALL_SECONDS = 2.4;
	// The last effect (the teleport's second bell) has ended by here.
	constexpr double EFFECTS_SECONDS = 5.75;
	// The level the result is brought to (its highest peak), as the mock-up's was.
	constexpr float STARTUP_PEAK = 0.72f;
	// Without a bed there is no loud passage to measure by: the effects get the gain they had
	// in the mock-up, where the bed was the user's sound.
	constexpr float EFFECTS_ALONE_GAIN = 0.83f;

	// Noise for the effects: the same on every run, and each effect has its own, so that one
	// can be left out without changing the others.
	class Random
	{
	public:
		explicit Random(std::uint64_t seed)
			: m_state(seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e019ull)
		{
			if (m_state == 0)
				m_state = 1;
			next();
			next();
		}

		// The bell curve, by Box and Muller.
		double normal()
		{
			if (m_have_spare)
			{
				m_have_spare = false;
				return m_spare;
			}
			const double u1 = 1.0 - uniform(); // never 0
			const double u2 = uniform();
			const double radius = std::sqrt(-2.0 * std::log(u1));
			m_spare = radius * std::sin(2.0 * PI * u2);
			m_have_spare = true;
			return radius * std::cos(2.0 * PI * u2);
		}

	private:
		std::uint64_t next()
		{
			m_state ^= m_state >> 12;
			m_state ^= m_state << 25;
			m_state ^= m_state >> 27;
			return m_state * 0x2545f4914f6cdd1dull;
		}

		double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

		std::uint64_t m_state;
		double m_spare = 0.0;
		bool m_have_spare = false;
	};

	// The simplest low-pass: each output a step from the last one towards the input.
	struct OnePole
	{
		double state = 0.0;
		double run(double input, double cutoff)
		{
			const double keep = std::exp(-2.0 * PI * cutoff / SR);
			state = (1.0 - keep) * input + keep * state;
			return state;
		}
	};

	// Up in `attack` seconds, then dying away.
	double Envelope(double t, double attack, double decay)
	{
		return std::min(t / std::max(attack, 1e-4), 1.0) * std::exp(-std::max(t - attack, 0.0) / decay);
	}

	std::size_t Samples(double seconds)
	{
		return static_cast<std::size_t>(seconds * SR);
	}

	// Rushing air that rises: noise between two low-passes that open together.
	std::vector<float> Whoosh(double length, std::uint64_t seed)
	{
		Random random(seed);
		OnePole upper, lower;
		std::vector<float> out(Samples(length));
		for (std::size_t k = 0; k < out.size(); k++)
		{
			const double p = static_cast<double>(k) / SR / length;
			const double noise = random.normal();
			const double cutoff = 300.0 + 5200.0 * p * p;
			const double band = upper.run(noise, cutoff) - lower.run(noise, cutoff * 0.25);
			const double swell = std::sin(PI * std::pow(p, 1.6));
			out[k] = static_cast<float>(band * swell * swell);
		}
		return out;
	}

	// Something heavy landing: a low tone that drops, and a short burst of noise on top.
	std::vector<float> Thud(std::uint64_t seed)
	{
		Random random(seed);
		std::vector<float> out(Samples(0.30));
		double phase = 0.0;
		for (std::size_t k = 0; k < out.size(); k++)
		{
			const double t = static_cast<double>(k) / SR;
			phase += (110.0 * std::exp(-t * 14.0) + 48.0) / SR;
			out[k] = static_cast<float>(std::sin(2.0 * PI * phase) * Envelope(t, 0.002, 0.07) + random.normal() * std::exp(-t * 220.0) * 0.2);
		}
		return out;
	}

	// A struck bell: a tone and two overtones a little off the octave and the twelfth, which
	// die sooner.
	std::vector<float> Bell(double frequency, double length = 1.3)
	{
		std::vector<float> out(Samples(length));
		const std::size_t release = Samples(0.010);
		for (std::size_t k = 0; k < out.size(); k++)
		{
			const double t = static_cast<double>(k) / SR;
			double value = (std::sin(2.0 * PI * frequency * t) + 0.4 * std::sin(2.0 * PI * frequency * 2.005 * t) * std::exp(-t * 6.0) +
							   0.18 * std::sin(2.0 * PI * frequency * 3.01 * t) * std::exp(-t * 11.0)) *
						   Envelope(t, 0.003, 0.36);
			// (Its last hundredth of a second goes to nothing, where the mock-up's just stopped.)
			if (out.size() - k <= release)
				value *= static_cast<double>(out.size() - k) / static_cast<double>(release);
			out[k] = static_cast<float>(value);
		}
		return out;
	}

	// The picture being made: two channels of floats, and sounds put on it at a time and a side.
	struct Canvas
	{
		std::vector<float> left, right;

		void put(const std::vector<float>& sound, double at, double pan, double gain, double scale = 1.0)
		{
			const std::size_t start = static_cast<std::size_t>((at + STARTUP_OFFSET) * SR);
			if (start >= left.size())
				return;
			const std::size_t count = std::min(sound.size(), left.size() - start);
			const float to_left = static_cast<float>(gain * scale * std::sqrt((1.0 - pan) / 2.0));
			const float to_right = static_cast<float>(gain * scale * std::sqrt((1.0 + pan) / 2.0));
			for (std::size_t k = 0; k < count; k++)
			{
				left[start + k] += sound[k] * to_left;
				right[start + k] += sound[k] * to_right;
			}
		}
	};

	// The Fourier transform, in place, of a power-of-two length: `sign` -1 forward, +1 back
	// (the caller divides by the length).
	void Transform(std::vector<float>& re, std::vector<float>& im, const std::vector<float>& cosines, const std::vector<float>& sines, float sign)
	{
		const std::size_t n = re.size();
		for (std::size_t i = 1, j = 0; i < n; i++)
		{
			std::size_t bit = n >> 1;
			for (; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;
			if (i < j)
			{
				std::swap(re[i], re[j]);
				std::swap(im[i], im[j]);
			}
		}

		for (std::size_t length = 2; length <= n; length <<= 1)
		{
			const std::size_t half = length / 2, stride = n / length;
			for (std::size_t i = 0; i < n; i += length)
			{
				for (std::size_t k = 0; k < half; k++)
				{
					const float wr = cosines[k * stride], wi = sign * sines[k * stride];
					const float xr = re[i + k + half], xi = im[i + k + half];
					const float vr = xr * wr - xi * wi, vi = xr * wi + xi * wr;
					re[i + k + half] = re[i + k] - vr;
					im[i + k + half] = im[i + k] - vi;
					re[i + k] += vr;
					im[i + k] += vi;
				}
			}
		}
	}

} // namespace

// A hall around the two channels: every sample echoed by noise that dies away (a different
// noise on each side). `left` and `right` come back HALL_SECONDS longer, holding the echo alone.
//
// The two channels go through one transform as the real and the imaginary part of one signal;
// their spectra are told apart again by the symmetry a real signal's spectrum has.
void Hall(std::vector<float>& left, std::vector<float>& right)
{
	const std::size_t dry = left.size(), tail = Samples(HALL_SECONDS);
	std::size_t n = 1;
	while (n < dry + tail)
		n <<= 1;

	std::vector<float> cosines(n / 2), sines(n / 2);
	for (std::size_t k = 0; k < n / 2; k++)
	{
		const double angle = 2.0 * PI * static_cast<double>(k) / static_cast<double>(n);
		cosines[k] = static_cast<float>(std::cos(angle));
		sines[k] = static_cast<float>(std::sin(angle));
	}

	// The echoes: noise dying to a hundredth in the 2.4 seconds, nothing in the first 12 ms
	// (the distance to the walls), the highest tones taken off (walls do that).
	std::vector<float> hr(n, 0.0f), hi(n, 0.0f);
	for (int side = 0; side < 2; side++)
	{
		std::vector<float>& h = side ? hi : hr;
		Random random(side ? 0x4a11'0002 : 0x4a11'0001);
		OnePole lowpass;
		const std::size_t gap = Samples(0.012);
		double energy = 0.0;
		for (std::size_t k = 0; k < tail; k++)
		{
			const double t = static_cast<double>(k) / SR;
			const double noise = random.normal() * std::exp(-t / (HALL_SECONDS / 5.0));
			const double value = lowpass.run((k < gap) ? 0.0 : noise, 5000.0);
			h[k] = static_cast<float>(value);
			energy += value * value;
		}
		const float scale = static_cast<float>(1.0 / std::sqrt(std::max(energy, 1e-12)));
		for (std::size_t k = 0; k < tail; k++)
			h[k] *= scale;
	}

	std::vector<float> xr(n, 0.0f), xi(n, 0.0f);
	std::copy(left.begin(), left.end(), xr.begin());
	std::copy(right.begin(), right.end(), xi.begin());

	Transform(xr, xi, cosines, sines, -1.0f);
	Transform(hr, hi, cosines, sines, -1.0f);

	for (std::size_t k = 0; k <= n / 2; k++)
	{
		const std::size_t m = (n - k) & (n - 1);
		// The left channel's spectrum is the part with a real signal's symmetry, the right
		// one's the part with the opposite symmetry.
		const float lr = (xr[k] + xr[m]) * 0.5f, li = (xi[k] - xi[m]) * 0.5f;
		const float rr = (xi[k] + xi[m]) * 0.5f, ri = (xr[m] - xr[k]) * 0.5f;
		const float alr = (hr[k] + hr[m]) * 0.5f, ali = (hi[k] - hi[m]) * 0.5f;
		const float arr = (hi[k] + hi[m]) * 0.5f, ari = (hr[m] - hr[k]) * 0.5f;
		// Each channel times its own echo, and the two put together again the same way.
		const float pr = lr * alr - li * ali, pi = lr * ali + li * alr;
		const float qr = rr * arr - ri * ari, qi = rr * ari + ri * arr;
		xr[k] = pr - qi;
		xi[k] = pi + qr;
		xr[m] = pr + qi;
		xi[m] = qr - pi;
	}

	Transform(xr, xi, cosines, sines, 1.0f);

	const float scale = 1.0f / static_cast<float>(n);
	left.resize(dry + tail);
	right.resize(dry + tail);
	for (std::size_t k = 0; k < dry + tail; k++)
	{
		left[k] = xr[k] * scale;
		right[k] = xi[k] * scale;
	}
}

namespace
{
	// The user's sound, one channel at the output's rate and at full level; empty when there is
	// nothing to hear in it.
	std::vector<float> BedSource(const Sound& bed)
	{
		const std::size_t take =
			static_cast<std::size_t>(std::min<std::uint64_t>(bed.frames, static_cast<std::uint64_t>(BED_MAX_SECONDS * bed.rate)));
		std::vector<float> mono(take);
		for (std::size_t i = 0; i < take; i++)
		{
			const s16* const frame = bed.frame(i);
			mono[i] = static_cast<float>(frame[0] + frame[1]) * (0.5f / 32768.0f);
		}
		if (bed.rate != OUTPUT_RATE)
			mono = ResampleMono(mono, static_cast<double>(bed.rate), SR);

		float peak = 0.0f;
		for (const float sample : mono)
			peak = std::max(peak, std::fabs(sample));
		if (peak < 1e-4f)
			return {};
		for (float& sample : mono)
			sample /= peak;
		return mono;
	}

	// The bed's treatment, onto the canvas.
	void PutBed(Canvas& canvas, const std::vector<float>& source)
	{
		const std::size_t count = source.size();
		const double length = static_cast<double>(count) / SR;
		const double fade_from = std::min(BED_FADE_FROM, std::max(length - 0.25, 0.0));
		const double fade_length = std::max(length - fade_from, 1e-3);

		std::vector<float> bed(count);
		OnePole lowpass;
		for (std::size_t k = 0; k < count; k++)
		{
			const double t = static_cast<double>(k) / SR;
			// It opens: dark at first, bright within the first half second.
			const double open = std::clamp(t / 0.5, 0.0, 1.0);
			double cutoff = 1400.0 + 15000.0 * open * open;
			// At the teleport it is drawn shut for a moment and let go again.
			const double d = (t - (STARTUP_TELEPORT + 0.26)) / 0.10;
			const double dip = (std::fabs(d) < 6.0) ? std::exp(-d * d) : 0.0;
			cutoff = cutoff * (1.0 - 0.93 * dip) + 300.0 * dip;

			double gain = std::clamp(t / 0.08, 0.0, 1.0) * (1.0 - 0.55 * dip);
			// It ends under the library: a long fade.
			const double fade = std::clamp(1.0 - (t - fade_from) / fade_length, 0.0, 1.0);
			gain *= fade * std::sqrt(fade);
			bed[k] = static_cast<float>(lowpass.run(source[k], cutoff) * gain);
		}

		// Width: each side also hears the sound a few milliseconds late, and the two delays
		// drift slowly and differently.
		const auto delayed = [&bed, count](std::size_t k, double ms, double depth_ms, double rate) {
			const double t = static_cast<double>(k) / SR;
			const double index = static_cast<double>(k) - (ms + depth_ms * std::sin(2.0 * PI * rate * t)) * SR / 1000.0;
			const double floor_index = std::floor(index);
			const std::int64_t last = static_cast<std::int64_t>(count) - 1;
			const std::int64_t i0 = std::clamp<std::int64_t>(static_cast<std::int64_t>(floor_index), 0, last);
			const std::int64_t i1 = std::min<std::int64_t>(i0 + 1, last);
			const float f = static_cast<float>(index - floor_index);
			return bed[static_cast<std::size_t>(i0)] * (1.0f - f) + bed[static_cast<std::size_t>(i1)] * f;
		};
		std::vector<float> left(count), right(count);
		for (std::size_t k = 0; k < count; k++)
		{
			left[k] = 0.7f * bed[k] + 0.3f * delayed(k, 11.0, 2.0, 0.23);
			right[k] = 0.7f * bed[k] + 0.3f * delayed(k, 17.0, 2.5, 0.31);
		}

		const std::size_t start = Samples(STARTUP_OFFSET);
		const auto add = [start](std::vector<float>& to, const std::vector<float>& from, float gain) {
			const std::size_t n = (start < to.size()) ? std::min(from.size(), to.size() - start) : 0;
			for (std::size_t k = 0; k < n; k++)
				to[start + k] += from[k] * gain;
		};
		add(canvas.left, left, 1.25f);
		add(canvas.right, right, 1.25f);
		Hall(left, right);
		add(canvas.left, left, 0.32f);
		add(canvas.right, right, 0.32f);
	}

	// tanh, which the console's libc is not asked for.
	float Squash(float x)
	{
		const float e = std::exp(-2.0f * std::fabs(x));
		const float y = (1.0f - e) / (1.0f + e);
		return (x < 0.0f) ? -y : y;
	}
} // namespace

float SoftLimit(float sample)
{
	constexpr float KNEE = 0.85f;
	const float level = std::fabs(sample);
	if (level <= KNEE)
		return sample;
	const float over = KNEE + (1.0f - KNEE) * Squash((level - KNEE) / (1.0f - KNEE));
	return (sample < 0.0f) ? -over : over;
}

bool RenderStartup(const Sound* bed, std::vector<s16>* stereo, StartupInfo* info, unsigned parts)
{
	*info = StartupInfo();
	stereo->clear();

	std::vector<float> source;
	if (bed && (parts & PART_BED) && bed->frames > 0 && bed->rate > 0)
		source = BedSource(*bed);
	info->bed_seconds = static_cast<double>(source.size()) / SR;

	const double seconds =
		std::max(EFFECTS_SECONDS, source.empty() ? 0.0 : STARTUP_OFFSET + info->bed_seconds + HALL_SECONDS + 0.01);
	const std::size_t count = Samples(seconds);

	// What follows takes some 25 MiB for a six-second bed, most of it for the hall's transform,
	// in vectors - which do not come back from a failed allocation in a title built without
	// exceptions. So it is asked first whether that much is there at all.
	{
		std::size_t transform = 1;
		while (transform < source.size() + Samples(HALL_SECONDS))
			transform <<= 1;
		const std::size_t needed = count * (2 * sizeof(float) + 2 * sizeof(s16)) +
								   (source.empty() ? 0 : (transform * 5 + (source.size() + Samples(HALL_SECONDS)) * 4) * sizeof(float)) +
								   (std::size_t(4) << 20);
		void* const probe = std::malloc(needed);
		if (!probe)
			return false;
		std::free(probe);
	}

	Canvas canvas;
	canvas.left.assign(count, 0.0f);
	canvas.right.assign(count, 0.0f);

	if (!source.empty())
		PutBed(canvas, source);

	// ---- the effects, in time with the picture
	// Each letter is heard on the side it comes from, and its bell is in the key of the
	// mock-up's bed (whose strongest tones were near 404 and 606 Hz), going up.
	static constexpr double PAN[STARTUP_LETTERS] = {-0.7, -0.7, 0.7, 0.7, -0.7, 0.7, 0.7};
	static constexpr double NOTES[STARTUP_LETTERS] = {404.0, 505.0, 606.0, 808.0, 1010.0, 1212.0, 1616.0};
	for (int i = 0; i < STARTUP_LETTERS; i++)
	{
		const double start = STARTUP_LETTER_START[i], lands = start + STARTUP_LETTER_FLIGHT;
		if (parts & PART_WHOOSHES)
			canvas.put(Whoosh(STARTUP_LETTER_FLIGHT + 0.05, 100 + i), start, PAN[i], 0.21);
		if (parts & PART_IMPACTS)
		{
			canvas.put(Thud(200 + i), lands, PAN[i] * 0.3, 0.29);
			canvas.put(Bell(NOTES[i]), lands, PAN[i] * 0.25, 0.14);
		}
	}

	if (parts & PART_ICON)
	{
		// The icon comes down: a deeper thud, two bells, and a shimmer after it.
		std::vector<float> drop(Samples(0.5));
		double phase = 0.0;
		for (std::size_t k = 0; k < drop.size(); k++)
		{
			const double t = static_cast<double>(k) / SR;
			phase += (70.0 * std::exp(-t * 9.0) + 40.0) / SR;
			drop[k] = static_cast<float>(std::sin(2.0 * PI * phase) * Envelope(t, 0.004, 0.12));
		}
		canvas.put(drop, STARTUP_ICON_LANDS, 0.0, 0.42);
		canvas.put(Bell(808.0, 1.8), STARTUP_ICON_LANDS + 0.02, 0.0, 0.16);
		canvas.put(Bell(1212.0, 1.8), STARTUP_ICON_LANDS + 0.02, 0.0, 0.16, 0.6);

		Random random(300);
		OnePole lowpass;
		std::vector<float> shimmer(Samples(0.9));
		for (std::size_t k = 0; k < shimmer.size(); k++)
		{
			const double noise = random.normal();
			shimmer[k] = static_cast<float>((noise - lowpass.run(noise, 6000.0)) * Envelope(static_cast<double>(k) / SR, 0.3, 0.25));
		}
		canvas.put(shimmer, STARTUP_ICON_LANDS + 0.35, 0.0, 0.07);
	}

	if (parts & PART_TELEPORT)
	{
		// Out: a tone falling with noise closing under it, cut by a click. In: a tone rising on
		// the left, and two bells.
		{
			Random random(400);
			OnePole lowpass;
			std::vector<float> down(Samples(0.26));
			double phase = 0.0;
			for (std::size_t k = 0; k < down.size(); k++)
			{
				const double p = static_cast<double>(k) / SR / 0.26;
				phase += (1500.0 * (1.0 - p) * (1.0 - p) + 110.0) / SR;
				down[k] = static_cast<float>(std::sin(2.0 * PI * phase) * p * std::sqrt(p) +
											 lowpass.run(random.normal(), 2500.0 * (1.0 - p) + 200.0) * 0.6 * p);
			}
			canvas.put(down, STARTUP_TELEPORT, 0.0, 0.30);
		}
		{
			Random random(401);
			std::vector<float> click(600);
			for (std::size_t k = 0; k < click.size(); k++)
				click[k] = static_cast<float>(random.normal() * std::exp(-static_cast<double>(k) / 60.0));
			canvas.put(click, STARTUP_TELEPORT + 0.25, 0.0, 0.3);
		}
		{
			std::vector<float> up(Samples(0.16));
			double phase = 0.0;
			for (std::size_t k = 0; k < up.size(); k++)
			{
				const double p = static_cast<double>(k) / SR / 0.16;
				phase += (220.0 + 1500.0 * p * p) / SR;
				up[k] = static_cast<float>(std::sin(2.0 * PI * phase) * std::pow(std::max(std::sin(PI * p), 0.0), 0.7));
			}
			canvas.put(up, STARTUP_TELEPORT + 0.35, -0.6, 0.4);
		}
		canvas.put(Bell(1616.0, 1.4), STARTUP_TELEPORT + 0.38, -0.5, 0.22);
		canvas.put(Bell(1212.0, 1.4), STARTUP_TELEPORT + 0.46, -0.3, 0.14);
	}

	// ---- the loud moments rounded off, and the whole brought to its level
	float peak = 0.0f;
	for (std::size_t k = 0; k < count; k++)
	{
		canvas.left[k] = Squash(canvas.left[k] * 0.95f);
		canvas.right[k] = Squash(canvas.right[k] * 0.95f);
		peak = std::max(peak, std::max(std::fabs(canvas.left[k]), std::fabs(canvas.right[k])));
	}
	float gain = (peak > 1e-6f) ? STARTUP_PEAK / peak : 0.0f;
	if (source.empty())
		gain = std::min(gain, EFFECTS_ALONE_GAIN);

	stereo->resize(count * 2);
	for (std::size_t k = 0; k < count; k++)
	{
		(*stereo)[k * 2 + 0] = RoundS16(canvas.left[k] * gain * 32767.0f);
		(*stereo)[k * 2 + 1] = RoundS16(canvas.right[k] * gain * 32767.0f);
	}

	info->seconds = seconds;
	info->gain = gain;
	info->peak = peak * gain;
	return true;
}

bool RenderPlain(const Sound& sound, double seconds, double fade, std::vector<s16>* stereo, StartupInfo* info)
{
	*info = StartupInfo();
	stereo->clear();
	if (sound.frames == 0 || sound.rate == 0 || seconds <= 0.0)
		return false;
	const std::uint64_t most = static_cast<std::uint64_t>(seconds * sound.rate);
	const bool cut = sound.frames > most;
	const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(sound.frames, most));
	std::vector<float> left(take), right(take);
	for (std::size_t i = 0; i < take; i++)
	{
		const s16* const frame = sound.frame(i);
		left[i] = static_cast<float>(frame[0]) * (1.0f / 32768.0f);
		right[i] = static_cast<float>(frame[1]) * (1.0f / 32768.0f);
	}
	if (sound.rate != OUTPUT_RATE)
	{
		left = ResampleMono(left, static_cast<double>(sound.rate), SR);
		right = ResampleMono(right, static_cast<double>(sound.rate), SR);
	}
	const std::size_t count = std::min(left.size(), right.size());
	float peak = 0.0f;
	for (std::size_t k = 0; k < count; k++)
		peak = std::max(peak, std::max(std::fabs(left[k]), std::fabs(right[k])));
	if (count == 0 || peak < 1e-4f)
		return false;
	// (A quiet file is brought up, but not by more than four times: its noise comes up with it.)
	const float gain = std::min(STARTUP_PEAK / peak, 4.0f);
	// Its end: faded when the sound goes on beyond what is used of it, and over a hundredth of a
	// second otherwise (a file that ends away from zero would click).
	const std::size_t tail = std::min(count, Samples(cut ? std::max(fade, 0.01) : 0.01));
	stereo->resize(count * 2);
	for (std::size_t k = 0; k < count; k++)
	{
		float g = gain;
		if (k + tail >= count)
		{
			const float u = static_cast<float>(count - k) / static_cast<float>(tail);
			g *= u * u * (3.0f - 2.0f * u);
		}
		(*stereo)[k * 2 + 0] = RoundS16(left[k] * g * 32767.0f);
		(*stereo)[k * 2 + 1] = RoundS16(right[k] * g * 32767.0f);
	}
	info->seconds = static_cast<double>(count) / SR;
	info->bed_seconds = info->seconds;
	info->gain = gain;
	info->peak = peak * gain;
	return true;
}

} // namespace PS5UIAudio::DSP
