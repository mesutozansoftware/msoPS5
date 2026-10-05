#ifndef PACKAGE_PACKAGE_READER_H
#define PACKAGE_PACKAGE_READER_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Package {

// Raised for malformed or unsupported package data. The message is shown to the user.
class PackageError: public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Random-access, read-only view of package bytes.
class Reader {
public:
	Reader()                         = default;
	virtual ~Reader()                = default;
	Reader(const Reader&)            = delete;
	Reader& operator=(const Reader&) = delete;

	[[nodiscard]] virtual uint64_t Size() const = 0;

	// Reads exactly `size` bytes at `offset`; throws PackageError when the range is out of bounds.
	virtual void Read(uint64_t offset, void* data, size_t size) = 0;

protected:
	void CheckRange(uint64_t offset, uint64_t size) const;
};

using ReaderPtr = std::shared_ptr<Reader>;

ReaderPtr OpenFileReader(const std::filesystem::path& path);

// A window of `size` bytes starting at `offset` in `base`.
ReaderPtr MakeSubReader(ReaderPtr base, uint64_t offset, uint64_t size);

// The concatenation of the listed `block_size`-byte blocks of `base`, truncated to `size` bytes.
ReaderPtr MakeBlockListReader(ReaderPtr base, std::vector<uint64_t> blocks, uint32_t block_size,
                              uint64_t size);

// `base` followed by zero bytes up to a total of `size` bytes.
ReaderPtr MakeZeroExtendedReader(ReaderPtr base, uint64_t size);

// One file or directory of an unpacked filesystem image. Paths use '/' and are relative to the
// image root.
struct TreeEntry {
	std::string                path;
	bool                       directory = false;
	uint64_t                   size      = 0;
	std::function<ReaderPtr()> open;
};

inline uint16_t Le16(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t Le32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t Le64(const uint8_t* p) {
	return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

// Validates a single path component read from an image: no separators, no "." / "..", no NUL.
bool IsSafePathComponent(const std::string& name);

} // namespace Package

#endif // PACKAGE_PACKAGE_READER_H
