/* Crux2 - what the compiler may call in a game's native module that a C library would give: the
 * module links against nothing (crux2_module.cpp). Built with -fno-builtin, so that these loops
 * are not turned back into calls to themselves; the arithmetic is written as the processor's own
 * instructions for the same reason (the console's processor, Zen 2, has SSE4.1's rounding).
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE. */
#include <stddef.h>
#include <stdint.h>

#define HIDDEN __attribute__((visibility("hidden")))

/* ---- memory */

HIDDEN void* memcpy(void* dst, const void* src, size_t n)
{
	void* d = dst;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
	return dst;
}

HIDDEN void* memmove(void* dst, const void* src, size_t n)
{
	unsigned char* d = (unsigned char*)dst;
	const unsigned char* s = (const unsigned char*)src;
	if (d <= s || d >= s + n)
		return memcpy(dst, src, n);
	while (n--)
		d[n] = s[n];
	return dst;
}

HIDDEN void* memset(void* dst, int c, size_t n)
{
	void* d = dst;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return dst;
}

HIDDEN int memcmp(const void* a, const void* b, size_t n)
{
	const unsigned char* x = (const unsigned char*)a;
	const unsigned char* y = (const unsigned char*)b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

HIDDEN int bcmp(const void* a, const void* b, size_t n)
{
	return memcmp(a, b, n);
}

/* ---- arithmetic: PS2Recomp's FPU and VU0 macros call sqrtf and sqrt (the compiler keeps the call
 * for the C library's errno on a negative number; here a negative gives NaN, as the instruction). */

HIDDEN float sqrtf(float x)
{
	float r;
	__asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
	return r;
}

HIDDEN double sqrt(double x)
{
	double r;
	__asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
	return r;
}

HIDDEN float fabsf(float x)
{
	union { float f; uint32_t u; } v = { x };
	v.u &= 0x7FFFFFFFu;
	return v.f;
}

HIDDEN double fabs(double x)
{
	union { double d; uint64_t u; } v = { x };
	v.u &= 0x7FFFFFFFFFFFFFFFull;
	return v.d;
}

HIDDEN float copysignf(float x, float y)
{
	union { float f; uint32_t u; } a = { x }, b = { y };
	a.u = (a.u & 0x7FFFFFFFu) | (b.u & 0x80000000u);
	return a.f;
}

/* roundss/roundsd: 9 = toward -inf, 10 = toward +inf, 11 = toward zero (precision exception off) */
#define ROUND_F(name, mode) \
	HIDDEN float name(float x) { float r; __asm__("roundss $" #mode ", %1, %0" : "=x"(r) : "x"(x)); return r; }
#define ROUND_D(name, mode) \
	HIDDEN double name(double x) { double r; __asm__("roundsd $" #mode ", %1, %0" : "=x"(r) : "x"(x)); return r; }
ROUND_F(floorf, 9)
ROUND_F(ceilf, 10)
ROUND_F(truncf, 11)
ROUND_D(floor, 9)
ROUND_D(ceil, 10)
ROUND_D(trunc, 11)

HIDDEN float roundf(float x)
{
	const float t = truncf(x);
	const float f = fabsf(x - t);
	return (f >= 0.5f) ? t + copysignf(1.0f, x) : t;
}

HIDDEN float fminf(float a, float b)
{
	if (a != a)
		return b;
	if (b != b)
		return a;
	return a < b ? a : b;
}

HIDDEN float fmaxf(float a, float b)
{
	if (a != a)
		return b;
	if (b != b)
		return a;
	return a > b ? a : b;
}
