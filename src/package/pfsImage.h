#ifndef PACKAGE_PFS_IMAGE_H
#define PACKAGE_PFS_IMAGE_H

#include "package/packageCrypto.h"
#include "package/packageReader.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace Package {

// Read-only PlayStation File System (PFS) image as found in PS5 packages and .ffpfs/.ffpfsc
// images: unsigned or signed inodes, optional AES-XTS encryption and zlib PFSC compression.
class PfsImage {
public:
	// Opens `image`. An encrypted image is decrypted with the first EKPFS candidate (under either
	// key schedule) that yields a valid directory tree; PackageError is thrown when none does.
	PfsImage(const ReaderPtr& image, const std::vector<Crypto::Digest256>& ekpfs_candidates);

	// Every file and directory below the user root, parents before children.
	[[nodiscard]] const std::vector<TreeEntry>& Entries() const { return m_entries; }

	[[nodiscard]] bool Encrypted() const { return m_encrypted; }

	// The EKPFS that decrypted the image, when it is encrypted.
	[[nodiscard]] const std::optional<Crypto::Digest256>& Key() const { return m_key; }

	// Returns true when `image` starts with a PFS superblock.
	static bool HasSuperblock(Reader& image);

private:
	std::vector<TreeEntry>           m_entries;
	bool                             m_encrypted = false;
	std::optional<Crypto::Digest256> m_key;
};

// Logical view of a PFSC container (zlib block compression) of `logical_size` bytes.
ReaderPtr MakePfscReader(ReaderPtr stored, uint64_t logical_size);

} // namespace Package

#endif // PACKAGE_PFS_IMAGE_H
