#include "package/packageReader.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace Package {

void Reader::CheckRange(uint64_t offset, uint64_t size) const {
	const uint64_t total = Size();
	if (offset > total || size > total - offset) {
		throw PackageError("The package is truncated or corrupt (read past the end of the data).");
	}
}

bool IsSafePathComponent(const std::string& name) {
	if (name.empty() || name == "." || name == "..") {
		return false;
	}
	return name.find_first_of(std::string("/\\\0", 3)) == std::string::npos;
}

namespace {

class FileReader final: public Reader {
public:
	explicit FileReader(const std::filesystem::path& path) {
		m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (m_fd < 0) {
			throw PackageError("Could not open \"" + path.string() + "\": " + std::strerror(errno));
		}
		struct stat st {};
		if (::fstat(m_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
			::close(m_fd);
			throw PackageError("\"" + path.string() + "\" is not a regular file.");
		}
		m_size = static_cast<uint64_t>(st.st_size);
	}

	~FileReader() override { ::close(m_fd); }

	FileReader(const FileReader&)            = delete;
	FileReader& operator=(const FileReader&) = delete;

	[[nodiscard]] uint64_t Size() const override { return m_size; }

	void Read(uint64_t offset, void* data, size_t size) override {
		CheckRange(offset, size);
		auto* out = static_cast<uint8_t*>(data);
		while (size > 0) {
			const ssize_t n = ::pread(m_fd, out, size, static_cast<off_t>(offset));
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				throw PackageError(std::string("Could not read the package: ") +
				                   (n < 0 ? std::strerror(errno) : "unexpected end of file"));
			}
			out += n;
			offset += static_cast<uint64_t>(n);
			size -= static_cast<size_t>(n);
		}
	}

private:
	int      m_fd   = -1;
	uint64_t m_size = 0;
};

class SubReader final: public Reader {
public:
	SubReader(ReaderPtr base, uint64_t offset, uint64_t size)
	    : m_base(std::move(base)), m_offset(offset), m_size(size) {
		if (offset > m_base->Size() || size > m_base->Size() - offset) {
			throw PackageError(
			    "The package is truncated or corrupt (region past the end of the data).");
		}
	}

	[[nodiscard]] uint64_t Size() const override { return m_size; }

	void Read(uint64_t offset, void* data, size_t size) override {
		CheckRange(offset, size);
		m_base->Read(m_offset + offset, data, size);
	}

private:
	ReaderPtr m_base;
	uint64_t  m_offset;
	uint64_t  m_size;
};

class BlockListReader final: public Reader {
public:
	BlockListReader(ReaderPtr base, std::vector<uint64_t> blocks, uint32_t block_size,
	                uint64_t size)
	    : m_base(std::move(base)), m_blocks(std::move(blocks)), m_block_size(block_size),
	      m_size(size) {
		if (block_size == 0 || size > static_cast<uint64_t>(m_blocks.size()) * block_size) {
			throw PackageError("The package is corrupt (file larger than its block list).");
		}
	}

	[[nodiscard]] uint64_t Size() const override { return m_size; }

	void Read(uint64_t offset, void* data, size_t size) override {
		CheckRange(offset, size);
		auto* out = static_cast<uint8_t*>(data);
		while (size > 0) {
			const uint64_t index  = offset / m_block_size;
			const uint64_t within = offset % m_block_size;
			const size_t   chunk  = static_cast<size_t>(
                std::min<uint64_t>(size, static_cast<uint64_t>(m_block_size) - within));
			m_base->Read(m_blocks[index] * m_block_size + within, out, chunk);
			out += chunk;
			offset += chunk;
			size -= chunk;
		}
	}

private:
	ReaderPtr             m_base;
	std::vector<uint64_t> m_blocks;
	uint32_t              m_block_size;
	uint64_t              m_size;
};

class ZeroExtendedReader final: public Reader {
public:
	ZeroExtendedReader(ReaderPtr base, uint64_t size): m_base(std::move(base)), m_size(size) {
		if (m_base->Size() > size) {
			throw PackageError("The package is corrupt (file data longer than the file).");
		}
	}

	[[nodiscard]] uint64_t Size() const override { return m_size; }

	void Read(uint64_t offset, void* data, size_t size) override {
		CheckRange(offset, size);
		auto*          out  = static_cast<uint8_t*>(data);
		const uint64_t real = m_base->Size();
		if (offset < real) {
			const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size, real - offset));
			m_base->Read(offset, out, chunk);
			out += chunk;
			size -= chunk;
		}
		std::fill(out, out + size, 0);
	}

private:
	ReaderPtr m_base;
	uint64_t  m_size;
};

} // namespace

ReaderPtr OpenFileReader(const std::filesystem::path& path) {
	return std::make_shared<FileReader>(path);
}

ReaderPtr MakeSubReader(ReaderPtr base, uint64_t offset, uint64_t size) {
	return std::make_shared<SubReader>(std::move(base), offset, size);
}

ReaderPtr MakeBlockListReader(ReaderPtr base, std::vector<uint64_t> blocks, uint32_t block_size,
                              uint64_t size) {
	return std::make_shared<BlockListReader>(std::move(base), std::move(blocks), block_size, size);
}

ReaderPtr MakeZeroExtendedReader(ReaderPtr base, uint64_t size) {
	return std::make_shared<ZeroExtendedReader>(std::move(base), size);
}

} // namespace Package
