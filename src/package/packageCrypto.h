#ifndef PACKAGE_PACKAGE_CRYPTO_H
#define PACKAGE_PACKAGE_CRYPTO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Package::Crypto {

using Digest256 = std::array<uint8_t, 32>;

Digest256 Sha256(const void* data, size_t size);
Digest256 Sha3_256(const void* data, size_t size);
Digest256 HmacSha256(const void* key, size_t key_size, const void* data, size_t size);

// AES-128-XTS decryption of whole sectors (IEEE P1619, 128-bit little-endian sector tweak).
class AesXtsDecryptor {
public:
	AesXtsDecryptor(const uint8_t* data_key, const uint8_t* tweak_key);
	~AesXtsDecryptor();
	AesXtsDecryptor(const AesXtsDecryptor&)            = delete;
	AesXtsDecryptor& operator=(const AesXtsDecryptor&) = delete;

	// Decrypts `size` bytes in place; `size` must be a multiple of 16.
	void DecryptSector(uint64_t sector, uint8_t* data, size_t size) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Root PFS key (EKPFS) of a debug package derived from its content id and passcode:
// H(H(BE32(index)) || H(content id padded to 48 bytes) || passcode), H = SHA-256 or SHA3-256.
Digest256 ComputeEkpfs(const std::string& content_id, const std::string& passcode, bool sha3);

struct PfsXtsKeys {
	std::array<uint8_t, 16> tweak_key {};
	std::array<uint8_t, 16> data_key {};
};

// PFS AES-XTS keys from the EKPFS and the superblock seed. `new_crypt` selects the PS5 schedule,
// which first replaces the EKPFS with HMAC-SHA256(EKPFS, seed).
PfsXtsKeys DerivePfsXtsKeys(const Digest256& ekpfs, const uint8_t* seed, bool new_crypt);

} // namespace Package::Crypto

#endif // PACKAGE_PACKAGE_CRYPTO_H
