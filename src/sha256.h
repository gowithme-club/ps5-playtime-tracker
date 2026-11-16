/* SHA-256 (FIPS 180-4), for the console identifier (console.h).
 *
 * Pure, covered by a host test.
 */

#ifndef GWM_SHA256_H
#define GWM_SHA256_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define GWM_SHA256_LEN 32

static uint32_t gwm_rotr(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void gwm_sha256_block(uint32_t h[8], const uint8_t *p)
{
	static const uint32_t k[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
	};
	uint32_t w[64], v[8];

	for(int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for(int i = 16; i < 64; i++)
	{
		uint32_t s0 = gwm_rotr(w[i - 15], 7) ^ gwm_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = gwm_rotr(w[i - 2], 17) ^ gwm_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	memcpy(v, h, sizeof(v));
	for(int i = 0; i < 64; i++)
	{
		uint32_t s1 = gwm_rotr(v[4], 6) ^ gwm_rotr(v[4], 11) ^ gwm_rotr(v[4], 25);
		uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
		uint32_t s0 = gwm_rotr(v[0], 2) ^ gwm_rotr(v[0], 13) ^ gwm_rotr(v[0], 22);
		uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));

		v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
		v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
	}
	for(int i = 0; i < 8; i++) h[i] += v[i];
}

static void gwm_sha256(const uint8_t *data, size_t len, uint8_t out[GWM_SHA256_LEN])
{
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	uint8_t tail[128];
	size_t full = len / 64 * 64, rest = len - full;
	uint64_t bits = (uint64_t)len * 8;

	for(size_t i = 0; i < full; i += 64) gwm_sha256_block(h, data + i);

	/* Padding: 0x80, zeros, then the length in bits, big-endian. */
	size_t padded = rest < 56 ? 64 : 128;
	memset(tail, 0, sizeof(tail));
	if(rest) memcpy(tail, data + full, rest);
	tail[rest] = 0x80;
	for(int i = 0; i < 8; i++) tail[padded - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));

	gwm_sha256_block(h, tail);
	if(padded == 128) gwm_sha256_block(h, tail + 64);

	for(int i = 0; i < 8; i++)
	{
		out[i * 4] = (uint8_t)(h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)h[i];
	}
}

#endif /* GWM_SHA256_H */
