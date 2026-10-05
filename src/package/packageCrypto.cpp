#include "package/packageCrypto.h"

#include "package/packageReader.h"

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <cstring>
#include <vector>

namespace Package::Crypto {

Digest256 Sha256(const void* data, size_t size) {
	Digest256 out {};
	CC_SHA256(data, static_cast<CC_LONG>(size), out.data());
	return out;
}

Digest256 HmacSha256(const void* key, size_t key_size, const void* data, size_t size) {
	Digest256 out {};
	CCHmac(kCCHmacAlgSHA256, key, key_size, data, size, out.data());
	return out;
}

namespace {

// Keccak-f[1600] as specified in FIPS 202; CommonCrypto does not provide SHA-3.
void KeccakF1600(uint64_t* a) {
	static constexpr uint64_t ROUND_CONSTANTS[24] = {
	    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL, 0x8000000080008000ULL,
	    0x000000000000808BULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
	    0x000000000000008AULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
	    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
	    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800AULL, 0x800000008000000AULL,
	    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
	static constexpr int ROTATIONS[25] = {0,  1,  62, 28, 27, 36, 44, 6,  55, 20, 3,  10, 43,
	                                      25, 39, 41, 45, 15, 21, 8,  18, 2,  61, 56, 14};
	const auto rotl = [](uint64_t v, int n) { return n == 0 ? v : (v << n) | (v >> (64 - n)); };

	for (const uint64_t rc: ROUND_CONSTANTS) {
		uint64_t c[5];
		for (int x = 0; x < 5; x++) {
			c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
		}
		for (int x = 0; x < 5; x++) {
			const uint64_t d = c[(x + 4) % 5] ^ rotl(c[(x + 1) % 5], 1);
			for (int y = 0; y < 25; y += 5) {
				a[y + x] ^= d;
			}
		}
		uint64_t b[25];
		for (int x = 0; x < 5; x++) {
			for (int y = 0; y < 5; y++) {
				b[y + 5 * ((2 * x + 3 * y) % 5)] = rotl(a[x + 5 * y], ROTATIONS[x + 5 * y]);
			}
		}
		for (int y = 0; y < 25; y += 5) {
			for (int x = 0; x < 5; x++) {
				a[y + x] = b[y + x] ^ (~b[y + (x + 1) % 5] & b[y + (x + 2) % 5]);
			}
		}
		a[0] ^= rc;
	}
}

} // namespace

Digest256 Sha3_256(const void* data, size_t size) {
	constexpr size_t RATE = 136;
	uint64_t         state[25] {};
	const auto*      in = static_cast<const uint8_t*>(data);

	const auto absorb = [&state](const uint8_t* block) {
		for (size_t i = 0; i < RATE / 8; i++) {
			state[i] ^= Le64(block + i * 8);
		}
		KeccakF1600(state);
	};

	for (; size >= RATE; in += RATE, size -= RATE) {
		absorb(in);
	}
	uint8_t last[RATE] {};
	std::memcpy(last, in, size);
	last[size] ^= 0x06;
	last[RATE - 1] ^= 0x80;
	absorb(last);

	Digest256 out {};
	for (size_t i = 0; i < out.size(); i++) {
		out[i] = static_cast<uint8_t>(state[i / 8] >> (8 * (i % 8)));
	}
	return out;
}

struct AesXtsDecryptor::Impl {
	CCCryptorRef data_decryptor  = nullptr;
	CCCryptorRef tweak_encryptor = nullptr;
};

AesXtsDecryptor::AesXtsDecryptor(const uint8_t* data_key, const uint8_t* tweak_key)
    : m_impl(std::make_unique<Impl>()) {
	if (CCCryptorCreate(kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode, data_key, kCCKeySizeAES128,
	                    nullptr, &m_impl->data_decryptor) != kCCSuccess ||
	    CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, tweak_key, kCCKeySizeAES128,
	                    nullptr, &m_impl->tweak_encryptor) != kCCSuccess) {
		if (m_impl->data_decryptor != nullptr) {
			CCCryptorRelease(m_impl->data_decryptor);
		}
		throw PackageError("Could not initialise AES decryption.");
	}
}

AesXtsDecryptor::~AesXtsDecryptor() {
	CCCryptorRelease(m_impl->data_decryptor);
	CCCryptorRelease(m_impl->tweak_encryptor);
}

void AesXtsDecryptor::DecryptSector(uint64_t sector, uint8_t* data, size_t size) const {
	if (size % 16 != 0) {
		throw PackageError("Encrypted data is not aligned to the AES block size.");
	}

	uint8_t tweak_input[16] {};
	for (int i = 0; i < 8; i++) {
		tweak_input[i] = static_cast<uint8_t>(sector >> (8 * i));
	}
	uint8_t tweak[16];
	size_t  moved = 0;
	if (CCCryptorUpdate(m_impl->tweak_encryptor, tweak_input, 16, tweak, 16, &moved) !=
	        kCCSuccess ||
	    moved != 16) {
		throw PackageError("AES decryption failed.");
	}

	// Precompute every block's tweak so the whole sector is decrypted in one ECB call.
	std::vector<uint8_t> tweaks(size);
	for (size_t offset = 0; offset < size; offset += 16) {
		std::memcpy(tweaks.data() + offset, tweak, 16);
		// Multiply the tweak by x in GF(2^128) (little-endian byte order).
		uint8_t carry = 0;
		for (unsigned char& byte: tweak) {
			const uint8_t next = static_cast<uint8_t>(byte >> 7);
			byte               = static_cast<uint8_t>((byte << 1) | carry);
			carry              = next;
		}
		if (carry != 0) {
			tweak[0] ^= 0x87;
		}
	}

	for (size_t i = 0; i < size; i++) {
		data[i] ^= tweaks[i];
	}
	if (CCCryptorUpdate(m_impl->data_decryptor, data, size, data, size, &moved) != kCCSuccess ||
	    moved != size) {
		throw PackageError("AES decryption failed.");
	}
	for (size_t i = 0; i < size; i++) {
		data[i] ^= tweaks[i];
	}
}

Digest256 ComputeEkpfs(const std::string& content_id, const std::string& passcode, bool sha3) {
	const auto hash = [sha3](const void* data, size_t size) {
		return sha3 ? Sha3_256(data, size) : Sha256(data, size);
	};

	constexpr uint8_t INDEX[4] = {0, 0, 0, 1};
	uint8_t           padded_id[48] {};
	std::memcpy(padded_id, content_id.data(), std::min<size_t>(content_id.size(), 48));

	uint8_t    material[96] {};
	const auto index_hash = hash(INDEX, sizeof(INDEX));
	const auto id_hash    = hash(padded_id, sizeof(padded_id));
	std::memcpy(material, index_hash.data(), 32);
	std::memcpy(material + 32, id_hash.data(), 32);
	std::memcpy(material + 64, passcode.data(), std::min<size_t>(passcode.size(), 32));
	return hash(material, sizeof(material));
}

PfsXtsKeys DerivePfsXtsKeys(const Digest256& ekpfs, const uint8_t* seed, bool new_crypt) {
	Digest256 root = ekpfs;
	if (new_crypt) {
		root = HmacSha256(ekpfs.data(), ekpfs.size(), seed, 16);
	}

	uint8_t message[20] = {1, 0, 0, 0};
	std::memcpy(message + 4, seed, 16);
	const auto key = HmacSha256(root.data(), root.size(), message, sizeof(message));

	PfsXtsKeys keys;
	std::memcpy(keys.tweak_key.data(), key.data(), 16);
	std::memcpy(keys.data_key.data(), key.data() + 16, 16);
	return keys;
}

} // namespace Package::Crypto
