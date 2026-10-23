/* SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 *
 * Crux2 - the recomp kit's disc reader (ps2disc.py), its one slow part in native code: a CHD hunk
 * stored with MAME's "huff" codec (a canonical Huffman tree of the 256 byte values, then the bytes).
 * Loaded by ps2disc.py through ctypes when it is there; ps2disc.py does the same in Python when it
 * is not. No C library: built freestanding, for Windows (a DLL) and Linux (a shared object).
 *
 *   int crux2_huff(const unsigned char *src, unsigned srclen, unsigned char *dst, unsigned dstlen)
 *       0, or -1 for data that is not a valid hunk.
 */
#include <stdint.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef struct
{
	const uint8_t *data;
	uint32_t length;
	uint32_t offset; /* the next byte to load */
	uint32_t buffer; /* bits waiting, at the top */
	int bits;        /* how many */
} Bits;

static uint32_t peek(Bits *b, int n)
{
	if (n == 0)
		return 0;
	while (b->bits <= 24)
	{
		if (b->offset < b->length)
			b->buffer |= (uint32_t)b->data[b->offset] << (24 - b->bits);
		b->offset++;
		b->bits += 8;
	}
	return b->buffer >> (32 - n);
}

static void skip(Bits *b, int n)
{
	b->buffer = n >= 32 ? 0 : b->buffer << n;
	b->bits -= n;
}

static uint32_t read_bits(Bits *b, int n)
{
	uint32_t v = peek(b, n);
	skip(b, n);
	return v;
}

/* bits read past the end of the data */
static int overflow(const Bits *b)
{
	return (int64_t)b->offset - b->bits / 8 > (int64_t)b->length;
}

typedef struct
{
	int numcodes, maxbits;
	uint8_t lengths[256];
	uint16_t *lookup; /* (symbol << 5) | length, 1 << maxbits of them */
} Tree;

static int build(Tree *t)
{
	uint32_t histo[33];
	for (int i = 0; i < 33; i++)
		histo[i] = 0;
	for (int i = 0; i < t->numcodes; i++)
	{
		if (t->lengths[i] > t->maxbits)
			return -1;
		histo[t->lengths[i]]++;
	}
	uint32_t start = 0;
	for (int len = 32; len > 0; len--)
	{
		uint32_t next = (start + histo[len]) >> 1;
		if (len != 1 && next * 2 != start + histo[len])
			return -1;
		histo[len] = start;
		start = next;
	}
	const uint32_t size = 1u << t->maxbits;
	for (uint32_t i = 0; i < size; i++)
		t->lookup[i] = 1; /* symbol 0, one bit: what a malformed tree leaves (as libchdr does) */
	for (int i = 0; i < t->numcodes; i++)
	{
		const int len = t->lengths[i];
		if (len == 0)
			continue;
		const uint32_t code = histo[len]++;
		const int shift = t->maxbits - len;
		const uint32_t lo = code << shift, hi = (code + 1) << shift;
		for (uint32_t j = lo; j < hi && j < size; j++)
			t->lookup[j] = (uint16_t)((i << 5) | len);
	}
	return 0;
}

static int decode(const Tree *t, Bits *b)
{
	const uint16_t v = t->lookup[peek(b, t->maxbits)];
	skip(b, v & 31);
	return v >> 5;
}

static uint16_t s_small_lookup[1 << 6];
static uint16_t s_lookup[1 << 16];

EXPORT int crux2_huff(const unsigned char *src, unsigned srclen, unsigned char *dst, unsigned dstlen)
{
	Bits b = {src, srclen, 0, 0, 0};
	Tree small = {24, 6, {0}, s_small_lookup};
	Tree tree = {256, 16, {0}, s_lookup};

	small.lengths[0] = (uint8_t)read_bits(&b, 3);
	const int start = (int)read_bits(&b, 3) + 1;
	int count = 0;
	for (int i = 1; i < 24; i++)
	{
		if (i < start || count == 7)
			small.lengths[i] = 0;
		else
		{
			count = (int)read_bits(&b, 3);
			small.lengths[i] = (uint8_t)(count == 7 ? 0 : count);
		}
	}
	if (build(&small) != 0)
		return -1;

	int rlefullbits = 0;
	for (uint32_t temp = 256 - 9; temp != 0; temp >>= 1)
		rlefullbits++;
	int cur = 0, last = 0;
	while (cur < 256)
	{
		const int value = decode(&small, &b);
		if (value != 0)
		{
			last = value - 1;
			tree.lengths[cur++] = (uint8_t)last;
		}
		else
		{
			int n = (int)read_bits(&b, 3) + 2;
			if (n == 9)
				n += (int)read_bits(&b, rlefullbits);
			for (; n != 0 && cur < 256; n--)
				tree.lengths[cur++] = (uint8_t)last;
		}
	}
	if (build(&tree) != 0)
		return -1;
	for (unsigned i = 0; i < dstlen; i++)
		dst[i] = (unsigned char)decode(&tree, &b);
	return overflow(&b) ? -1 : 0;
}

/* What the compiler may call for the loops above, with no C library to give them. */
void *memset(void *d, int c, __SIZE_TYPE__ n)
{
	unsigned char *p = (unsigned char *)d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
	unsigned char *p = (unsigned char *)d;
	const unsigned char *q = (const unsigned char *)s;
	while (n--)
		*p++ = *q++;
	return d;
}

#if defined(_WIN32)
/* The DLL's entry: nothing to set up. */
int __stdcall DllMainCRTStartup(void *module, unsigned reason, void *reserved)
{
	(void)module;
	(void)reason;
	(void)reserved;
	return 1;
}
#endif
