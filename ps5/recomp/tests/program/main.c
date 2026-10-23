// A test program for the recompiler: deterministic work of several kinds, a checksum printed.
#include <stdio.h>
#include <string.h>

static unsigned int lcg(unsigned int *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static float dot(const float *a, const float *b, int n) { float r = 0; for (int i = 0; i < n; i++) r += a[i] * b[i]; return r; }

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void)
{
	unsigned int seed = 1234, sum = 0;
	static unsigned int table[4096];
	for (int i = 0; i < 4096; i++) table[i] = lcg(&seed);
	for (int round = 0; round < 64; round++)
		for (int i = 0; i < 4096; i++) sum ^= table[(i * 7 + round) & 4095] + (sum << 3) + (sum >> 5);
	float a[64], b[64];
	for (int i = 0; i < 64; i++) { a[i] = (float)(i % 13) * 0.5f; b[i] = (float)(i % 7) - 3.0f; }
	const float d = dot(a, b, 64);
	unsigned long long wide = 0;
	for (int i = 1; i < 1000; i++) wide = wide * 31 + (unsigned long long)i * i;
	char text[64];
	strcpy(text, "recompiled");
	printf("crux2-recomp-test: sum %08x dot %d fib %d wide %08x%08x len %d\n", sum, (int)(d * 100), fib(20),
		(unsigned int)(wide >> 32), (unsigned int)wide, (int)strlen(text));
	return 0;
}
