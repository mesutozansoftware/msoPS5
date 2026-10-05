#include "package/pfsImage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <zlib.h>

namespace Package {

namespace {

constexpr uint64_t PFS_MAGIC         = 20130315;
constexpr size_t   SUPERBLOCK_SIZE   = 0x400;
constexpr uint16_t MODE_SIGNED       = 0x1;
constexpr uint16_t MODE_64BIT_INODES = 0x2;
constexpr uint16_t MODE_ENCRYPTED    = 0x4;

constexpr uint16_t INODE_MODE_DIR        = 0x4000;
constexpr uint16_t INODE_MODE_FILE       = 0x8000;
constexpr uint32_t INODE_FLAG_COMPRESSED = 0x1;
constexpr int      DIRECT_BLOCKS         = 12;
constexpr int      INDIRECT_BLOCKS       = 5;
constexpr uint32_t SIGNATURE_SIZE        = 32;

constexpr uint32_t DIRENT_FILE      = 2;
constexpr uint32_t DIRENT_DIRECTORY = 3;
constexpr uint32_t DIRENT_HEADER    = 16;

constexpr uint32_t XTS_SECTOR_SIZE = 0x1000;

constexpr uint32_t PFSC_MAGIC       = 0x43534650; // "PFSC"
constexpr size_t   PFSC_HEADER_SIZE = 0x30;

// The superblock embeds a 64-bit signed inode describing the inode table.
constexpr size_t SUPERBLOCK_INODE_OFFSET = 0x50;
constexpr size_t S64_POINTER_OFFSET      = 0x68;
constexpr size_t S64_ENTRY_SIZE          = SIGNATURE_SIZE + 8;
constexpr size_t SEED_OFFSET             = 0x370;

struct Superblock {
	uint16_t                mode               = 0;
	uint32_t                block_size         = 0;
	uint64_t                total_blocks       = 0;
	uint64_t                dinode_count       = 0;
	uint64_t                dinode_block_count = 0;
	uint64_t                inode_table_block  = 0;
	uint32_t                inode_size         = 0;
	std::array<uint8_t, 16> seed {};

	[[nodiscard]] bool Signed() const { return (mode & MODE_SIGNED) != 0; }
	[[nodiscard]] bool WidePointers() const { return Signed() && (mode & MODE_64BIT_INODES) != 0; }
	// Size of one (signature, block pointer) record in signed inodes and indirect blocks.
	[[nodiscard]] uint32_t EntrySize() const { return SIGNATURE_SIZE + (WidePointers() ? 8 : 4); }
};

struct Inode {
	uint16_t                             mode            = 0;
	uint32_t                             flags           = 0;
	uint64_t                             size            = 0;
	uint64_t                             size_compressed = 0;
	uint32_t                             blocks          = 0;
	std::array<int64_t, DIRECT_BLOCKS>   db {};
	std::array<int64_t, INDIRECT_BLOCKS> ib {};

	[[nodiscard]] bool Compressed() const { return (flags & INODE_FLAG_COMPRESSED) != 0; }
	// Compressed inodes store the on-disk length in `size` and the logical length in
	// `size_compressed`.
	[[nodiscard]] uint64_t StoredSize() const { return size; }
	[[nodiscard]] uint64_t LogicalSize() const { return Compressed() ? size_compressed : size; }
};

int64_t ReadPointer(const uint8_t* p, bool wide) {
	return wide ? static_cast<int64_t>(Le64(p)) : static_cast<int32_t>(Le32(p));
}

std::optional<Superblock> ParseSuperblock(Reader& image) {
	if (image.Size() < SUPERBLOCK_SIZE) {
		return std::nullopt;
	}
	uint8_t raw[SUPERBLOCK_SIZE];
	image.Read(0, raw, sizeof(raw));

	const uint64_t version = Le64(raw);
	if ((version != 1 && version != 2) || Le64(raw + 0x08) != PFS_MAGIC) {
		return std::nullopt;
	}

	Superblock sb;
	sb.mode       = Le16(raw + 0x1C);
	sb.block_size = Le32(raw + 0x20);
	if (sb.block_size < XTS_SECTOR_SIZE || sb.block_size > 0x100000 ||
	    (sb.block_size & (sb.block_size - 1)) != 0) {
		return std::nullopt;
	}
	sb.total_blocks       = image.Size() / sb.block_size;
	sb.dinode_count       = Le64(raw + 0x30);
	sb.dinode_block_count = Le64(raw + 0x40);
	sb.inode_size         = sb.Signed() ? (sb.WidePointers() ? 0x310 : 0x2C8) : 0xA8;
	std::memcpy(sb.seed.data(), raw + SEED_OFFSET, sb.seed.size());

	const uint64_t per_block = sb.block_size / sb.inode_size;
	if (sb.dinode_count == 0 || sb.dinode_block_count == 0 ||
	    sb.dinode_block_count >= sb.total_blocks ||
	    sb.dinode_count > sb.dinode_block_count * per_block) {
		return std::nullopt;
	}

	// The inode table starts at the superblock inode's first direct block; older layouts place
	// the inode-table signature blocks first and leave that pointer empty.
	const uint8_t* table_inode = raw + SUPERBLOCK_INODE_OFFSET;
	const int64_t  first =
	    static_cast<int64_t>(Le64(table_inode + S64_POINTER_OFFSET + SIGNATURE_SIZE));
	if (first > 0 && static_cast<uint64_t>(first) < sb.total_blocks) {
		sb.inode_table_block = static_cast<uint64_t>(first);
	} else {
		sb.inode_table_block = 1;
		for (int i = 0; i < INDIRECT_BLOCKS; i++) {
			const size_t offset =
			    S64_POINTER_OFFSET + (DIRECT_BLOCKS + i) * S64_ENTRY_SIZE + SIGNATURE_SIZE;
			if (static_cast<int64_t>(Le64(table_inode + offset)) > 0) {
				sb.inode_table_block++;
			}
		}
	}
	if (sb.inode_table_block + sb.dinode_block_count > sb.total_blocks) {
		return std::nullopt;
	}
	return sb;
}

Inode ParseInode(const uint8_t* p, const Superblock& sb) {
	Inode inode;
	inode.mode            = Le16(p);
	inode.flags           = Le32(p + 0x04);
	inode.size            = Le64(p + 0x08);
	inode.size_compressed = Le64(p + 0x10);
	inode.blocks          = Le32(p + 0x60);

	if (sb.Signed()) {
		const bool     wide  = sb.WidePointers();
		const uint32_t entry = sb.EntrySize();
		const uint8_t* table = p + (wide ? 0x68 : 0x64);
		for (int i = 0; i < DIRECT_BLOCKS; i++) {
			inode.db[i] = ReadPointer(table + i * entry + SIGNATURE_SIZE, wide);
		}
		for (int i = 0; i < INDIRECT_BLOCKS; i++) {
			inode.ib[i] = ReadPointer(table + (DIRECT_BLOCKS + i) * entry + SIGNATURE_SIZE, wide);
		}
	} else {
		for (int i = 0; i < DIRECT_BLOCKS; i++) {
			inode.db[i] = static_cast<int32_t>(Le32(p + 0x64 + i * 4));
		}
		for (int i = 0; i < INDIRECT_BLOCKS; i++) {
			inode.ib[i] = static_cast<int32_t>(Le32(p + 0x94 + i * 4));
		}
	}
	return inode;
}

uint64_t CheckedBlock(int64_t block, const Superblock& sb) {
	if (block <= 0 || static_cast<uint64_t>(block) >= sb.total_blocks) {
		throw PackageError("The PFS image is corrupt (block pointer out of range).");
	}
	return static_cast<uint64_t>(block);
}

// Block numbers holding the first `count` blocks of an inode's stored data.
std::vector<uint64_t> InodeBlocks(const Superblock& sb, Reader& data, const Inode& inode,
                                  uint64_t count) {
	if (count > sb.total_blocks) {
		throw PackageError("The PFS image is corrupt (file larger than the image).");
	}
	std::vector<uint64_t> blocks;
	blocks.reserve(count);

	if (!sb.Signed()) {
		// Unsigned images store every file contiguously.
		if (count == 0) {
			return blocks;
		}
		const uint64_t start = CheckedBlock(inode.db[0], sb);
		if (start + count > sb.total_blocks) {
			throw PackageError("The PFS image is corrupt (file extends past the image).");
		}
		for (uint64_t i = 0; i < count; i++) {
			blocks.push_back(start + i);
		}
		return blocks;
	}

	for (int i = 0; i < DIRECT_BLOCKS && blocks.size() < count; i++) {
		blocks.push_back(CheckedBlock(inode.db[i], sb));
	}

	const uint32_t       entry     = sb.EntrySize();
	const uint32_t       per_block = sb.block_size / entry;
	std::vector<uint8_t> record_block(sb.block_size);
	const auto           read_records = [&](uint64_t block, uint64_t limit, auto&& emit) {
        data.Read(block * sb.block_size, record_block.data(), record_block.size());
        for (uint32_t i = 0; i < per_block && i < limit; i++) {
            emit(ReadPointer(record_block.data() + i * entry + SIGNATURE_SIZE, sb.WidePointers()));
        }
	};

	if (blocks.size() < count) {
		read_records(CheckedBlock(inode.ib[0], sb), count - blocks.size(),
		             [&](int64_t block) { blocks.push_back(CheckedBlock(block, sb)); });
	}
	if (blocks.size() < count) {
		std::vector<uint64_t> children;
		const uint64_t        needed = (count - blocks.size() + per_block - 1) / per_block;
		read_records(CheckedBlock(inode.ib[1], sb), needed,
		             [&](int64_t block) { children.push_back(CheckedBlock(block, sb)); });
		for (const uint64_t child: children) {
			read_records(child, count - blocks.size(),
			             [&](int64_t block) { blocks.push_back(CheckedBlock(block, sb)); });
		}
	}
	if (blocks.size() < count) {
		throw PackageError("The PFS image uses an unsupported block indirection depth.");
	}
	return blocks;
}

// AES-XTS view of an encrypted PFS image; the first `plain_bytes` (the superblock) are stored
// in the clear.
class XtsReader final: public Reader {
public:
	XtsReader(ReaderPtr base, const Crypto::PfsXtsKeys& keys, uint64_t plain_bytes)
	    : m_base(std::move(base)), m_cipher(keys.data_key.data(), keys.tweak_key.data()),
	      m_plain_bytes(plain_bytes) {}

	[[nodiscard]] uint64_t Size() const override { return m_base->Size(); }

	void Read(uint64_t offset, void* data, size_t size) override {
		CheckRange(offset, size);
		auto* out = static_cast<uint8_t*>(data);
		while (size > 0) {
			const uint64_t sector       = offset / XTS_SECTOR_SIZE;
			const uint64_t sector_start = sector * XTS_SECTOR_SIZE;
			const uint64_t within       = offset - sector_start;
			const size_t   chunk =
			    static_cast<size_t>(std::min<uint64_t>(size, XTS_SECTOR_SIZE - within));

			if (sector_start < m_plain_bytes) {
				m_base->Read(offset, out, chunk);
			} else if (within == 0 && chunk == XTS_SECTOR_SIZE) {
				m_base->Read(sector_start, out, XTS_SECTOR_SIZE);
				m_cipher.DecryptSector(sector, out, XTS_SECTOR_SIZE);
			} else {
				uint8_t buffer[XTS_SECTOR_SIZE];
				m_base->Read(sector_start, buffer, XTS_SECTOR_SIZE);
				m_cipher.DecryptSector(sector, buffer, XTS_SECTOR_SIZE);
				std::memcpy(out, buffer + within, chunk);
			}
			out += chunk;
			offset += chunk;
			size -= chunk;
		}
	}

private:
	ReaderPtr               m_base;
	Crypto::AesXtsDecryptor m_cipher;
	uint64_t                m_plain_bytes;
};

class PfscReader final: public Reader {
public:
	PfscReader(ReaderPtr stored, uint64_t logical_size)
	    : m_stored(std::move(stored)), m_size(logical_size) {
		uint8_t header[PFSC_HEADER_SIZE];
		m_stored->Read(0, header, sizeof(header));
		if (Le32(header) != PFSC_MAGIC) {
			throw PackageError("The package is corrupt (compressed file without a PFSC header).");
		}
		if (Le32(header + 0x04) != 0) {
			throw PackageError(
			    "This package uses Kraken (Oodle) compression, which msoPS5 cannot unpack yet.");
		}
		m_block_size                 = Le32(header + 0x0C);
		const uint64_t offsets       = Le64(header + 0x18);
		const uint64_t logical_bytes = Le64(header + 0x28);
		if (m_block_size < 0x1000 || m_block_size > 0x1000000 ||
		    (m_block_size & (m_block_size - 1)) != 0 || logical_bytes < logical_size) {
			throw PackageError("The package is corrupt (invalid PFSC header).");
		}

		const uint64_t count       = (logical_bytes + m_block_size - 1) / m_block_size;
		const uint64_t stored_size = m_stored->Size();
		if (offsets > stored_size || count + 1 > (stored_size - offsets) / 8) {
			throw PackageError("The package is corrupt (PFSC block table out of range).");
		}
		std::vector<uint8_t> table((count + 1) * 8);
		m_stored->Read(offsets, table.data(), table.size());
		m_offsets.resize(count + 1);
		for (uint64_t i = 0; i <= count; i++) {
			m_offsets[i] = Le64(table.data() + i * 8);
			if (m_offsets[i] > stored_size || (i > 0 && m_offsets[i] < m_offsets[i - 1])) {
				throw PackageError("The package is corrupt (invalid PFSC block table).");
			}
		}
		m_block.resize(m_block_size);
	}

	[[nodiscard]] uint64_t Size() const override { return m_size; }

	void Read(uint64_t offset, void* data, size_t size) override {
		CheckRange(offset, size);
		auto* out = static_cast<uint8_t*>(data);
		while (size > 0) {
			const uint64_t index  = offset / m_block_size;
			const uint64_t within = offset % m_block_size;
			const size_t   chunk =
			    static_cast<size_t>(std::min<uint64_t>(size, m_block_size - within));
			LoadBlock(index);
			std::memcpy(out, m_block.data() + within, chunk);
			out += chunk;
			offset += chunk;
			size -= chunk;
		}
	}

private:
	void LoadBlock(uint64_t index) {
		if (index == m_loaded) {
			return;
		}
		m_loaded              = UINT64_MAX;
		const uint64_t start  = m_offsets[index];
		const uint64_t length = m_offsets[index + 1] - start;
		if (length == m_block_size) {
			m_stored->Read(start, m_block.data(), m_block_size);
		} else if (length == 0) {
			std::fill(m_block.begin(), m_block.end(), 0);
		} else if (length < m_block_size) {
			m_compressed.resize(length);
			m_stored->Read(start, m_compressed.data(), length);
			Inflate();
		} else {
			throw PackageError("The package is corrupt (oversized PFSC block).");
		}
		m_loaded = index;
	}

	void Inflate() {
		// Blocks are zlib streams; accept raw deflate data as well.
		const bool zlib_header = m_compressed.size() >= 2 && (m_compressed[0] & 0x0F) == 8 &&
		                         (m_compressed[0] >> 4) <= 7 &&
		                         ((m_compressed[0] << 8) | m_compressed[1]) % 31 == 0;
		z_stream stream {};
		if (inflateInit2(&stream, zlib_header ? MAX_WBITS : -MAX_WBITS) != Z_OK) {
			throw PackageError("Could not initialise zlib.");
		}
		stream.next_in      = m_compressed.data();
		stream.avail_in     = static_cast<uInt>(m_compressed.size());
		stream.next_out     = m_block.data();
		stream.avail_out    = m_block_size;
		const int  result   = inflate(&stream, Z_FINISH);
		const auto produced = static_cast<size_t>(stream.total_out);
		inflateEnd(&stream);
		if (result != Z_STREAM_END) {
			throw PackageError("The package is corrupt (a compressed block failed to decompress).");
		}
		std::fill(m_block.begin() + static_cast<std::ptrdiff_t>(produced), m_block.end(), 0);
	}

	ReaderPtr             m_stored;
	uint64_t              m_size;
	uint32_t              m_block_size = 0;
	std::vector<uint64_t> m_offsets;
	std::vector<uint8_t>  m_block;
	std::vector<uint8_t>  m_compressed;
	uint64_t              m_loaded = UINT64_MAX;
};

ReaderPtr OpenInode(const Superblock& sb, const ReaderPtr& data, const Inode& inode) {
	const uint64_t stored = inode.StoredSize();
	if (stored == 0) {
		return MakeSubReader(data, 0, 0);
	}
	const uint64_t count  = (stored + sb.block_size - 1) / sb.block_size;
	auto           blocks = InodeBlocks(sb, *data, inode, count);

	ReaderPtr reader;
	if (std::adjacent_find(blocks.begin(), blocks.end(),
	                       [](uint64_t a, uint64_t b) { return b != a + 1; }) == blocks.end()) {
		reader = MakeSubReader(data, blocks.front() * sb.block_size, stored);
	} else {
		reader = MakeBlockListReader(data, std::move(blocks), sb.block_size, stored);
	}
	return inode.Compressed() ? MakePfscReader(reader, inode.LogicalSize()) : reader;
}

struct Dirent {
	uint32_t    inode = 0;
	uint32_t    type  = 0;
	std::string name;
};

std::vector<Dirent> ReadDirectory(const Superblock& sb, Reader& data, const Inode& inode) {
	if ((inode.mode & INODE_MODE_DIR) == 0) {
		throw PackageError("The PFS image is corrupt (directory inode is not a directory).");
	}
	std::vector<Dirent>  entries;
	std::vector<uint8_t> block(sb.block_size);
	for (const uint64_t number: InodeBlocks(sb, data, inode, inode.blocks)) {
		data.Read(number * sb.block_size, block.data(), block.size());
		for (uint32_t offset = 0; offset + DIRENT_HEADER <= sb.block_size;) {
			const uint8_t* p        = block.data() + offset;
			const uint32_t ent_size = Le32(p + 12);
			if (ent_size == 0) {
				break;
			}
			const uint32_t name_len = Le32(p + 8);
			if (ent_size < DIRENT_HEADER || ent_size > sb.block_size - offset ||
			    name_len > ent_size - DIRENT_HEADER) {
				throw PackageError("The PFS image is corrupt (invalid directory entry).");
			}
			entries.push_back(
			    {Le32(p), Le32(p + 4),
			     std::string(reinterpret_cast<const char*>(p + DIRENT_HEADER), name_len)});
			offset += ent_size;
		}
	}
	return entries;
}

} // namespace

ReaderPtr MakePfscReader(ReaderPtr stored, uint64_t logical_size) {
	return std::make_shared<PfscReader>(std::move(stored), logical_size);
}

bool PfsImage::HasSuperblock(Reader& image) {
	return ParseSuperblock(image).has_value();
}

namespace {

std::vector<TreeEntry> LoadTree(const Superblock& sb, const ReaderPtr& data) {
	std::vector<Inode>   inodes;
	std::vector<uint8_t> block(sb.block_size);
	const uint64_t       per_block = sb.block_size / sb.inode_size;
	for (uint64_t b = 0; b < sb.dinode_block_count && inodes.size() < sb.dinode_count; b++) {
		data->Read((sb.inode_table_block + b) * sb.block_size, block.data(), block.size());
		for (uint64_t i = 0; i < per_block && inodes.size() < sb.dinode_count; i++) {
			inodes.push_back(ParseInode(block.data() + i * sb.inode_size, sb));
		}
	}

	const auto inode_at = [&inodes](uint32_t number) -> const Inode& {
		if (number >= inodes.size()) {
			throw PackageError("The PFS image is corrupt (inode number out of range).");
		}
		return inodes[number];
	};

	// Inode 0 is the super root; the game files live below its "uroot" directory.
	uint32_t uroot = UINT32_MAX;
	for (const auto& entry: ReadDirectory(sb, *data, inode_at(0))) {
		if (entry.type == DIRENT_DIRECTORY && entry.name == "uroot") {
			uroot = entry.inode;
		}
	}
	if (uroot == UINT32_MAX) {
		throw PackageError("The PFS image is corrupt (no user root directory).");
	}

	std::vector<TreeEntry>                        entries;
	std::set<uint32_t>                            visited {uroot};
	std::vector<std::pair<uint32_t, std::string>> pending {{uroot, std::string()}};
	while (!pending.empty()) {
		const auto [dir, prefix] = pending.back();
		pending.pop_back();

		for (const auto& entry: ReadDirectory(sb, *data, inode_at(dir))) {
			if (entry.type != DIRENT_FILE && entry.type != DIRENT_DIRECTORY) {
				continue; // "." and ".."
			}
			if (!IsSafePathComponent(entry.name)) {
				throw PackageError("The PFS image contains an invalid file name.");
			}
			const Inode& inode = inode_at(entry.inode);
			std::string  path  = prefix.empty() ? entry.name : prefix + "/" + entry.name;

			if (entry.type == DIRENT_DIRECTORY) {
				if ((inode.mode & INODE_MODE_DIR) == 0 || !visited.insert(entry.inode).second) {
					throw PackageError("The PFS image is corrupt (invalid directory tree).");
				}
				entries.push_back({path, true, 0, {}});
				pending.emplace_back(entry.inode, std::move(path));
			} else {
				if ((inode.mode & INODE_MODE_FILE) == 0) {
					throw PackageError("The PFS image is corrupt (file inode is not a file).");
				}
				entries.push_back({std::move(path), false, inode.LogicalSize(),
				                   [sb, data, inode]() { return OpenInode(sb, data, inode); }});
			}
		}
	}
	return entries;
}

} // namespace

PfsImage::PfsImage(const ReaderPtr& image, const std::vector<Crypto::Digest256>& ekpfs_candidates) {
	const auto sb = ParseSuperblock(*image);
	if (!sb) {
		throw PackageError("The file is not a valid PFS image.");
	}
	if ((sb->mode & MODE_ENCRYPTED) == 0) {
		m_entries = LoadTree(*sb, image);
		return;
	}

	m_encrypted = true;
	for (const auto& ekpfs: ekpfs_candidates) {
		for (const bool new_crypt: {true, false}) {
			try {
				const auto keys = Crypto::DerivePfsXtsKeys(ekpfs, sb->seed.data(), new_crypt);
				m_entries = LoadTree(*sb, std::make_shared<XtsReader>(image, keys, sb->block_size));
				m_key     = ekpfs;
				return;
			} catch (const std::exception&) {
				// A wrong key decrypts to garbage that fails validation; try the next one.
			}
		}
	}
	throw PackageError(
	    "The package is encrypted with a key msoPS5 cannot derive. Only fake/debug packages "
	    "(fPKG) with a known passcode can be installed.");
}

} // namespace Package
