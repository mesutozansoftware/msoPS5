#include "package/exfatImage.h"

#include <cstring>
#include <set>
#include <string>
#include <utility>

namespace Package {

namespace {

constexpr size_t   BOOT_SECTOR_SIZE = 512;
constexpr char     SIGNATURE[]      = "EXFAT   ";
constexpr uint32_t FIRST_CLUSTER    = 2;
constexpr uint32_t END_OF_CHAIN     = 0xFFFFFFFF;

constexpr uint8_t  ENTRY_END_OF_DIRECTORY = 0x00;
constexpr uint8_t  ENTRY_FILE             = 0x85;
constexpr uint8_t  ENTRY_STREAM           = 0xC0;
constexpr uint8_t  ENTRY_FILE_NAME        = 0xC1;
constexpr uint16_t ATTRIBUTE_DIRECTORY    = 0x10;
constexpr uint8_t  FLAG_NO_FAT_CHAIN      = 0x02;
constexpr size_t   ENTRY_SIZE             = 32;
constexpr size_t   NAME_CHARS_PER_ENTRY   = 15;

struct Volume {
	ReaderPtr image;
	ReaderPtr heap;
	uint64_t  fat_offset    = 0;
	uint64_t  heap_offset   = 0;
	uint32_t  cluster_size  = 0;
	uint32_t  cluster_count = 0;
	uint32_t  root_cluster  = 0;

	[[nodiscard]] bool ValidCluster(uint32_t cluster) const {
		return cluster >= FIRST_CLUSTER && cluster - FIRST_CLUSTER < cluster_count;
	}

	// Clusters of an allocation, following the FAT unless the data is stored contiguously.
	[[nodiscard]] std::vector<uint64_t> Chain(uint32_t first, uint64_t length,
	                                          bool contiguous) const {
		std::vector<uint64_t> clusters;
		if (length == 0) {
			return clusters;
		}
		const uint64_t count = (length + cluster_size - 1) / cluster_size;
		if (count > cluster_count || !ValidCluster(first)) {
			throw PackageError("The exFAT image is corrupt (invalid cluster chain).");
		}
		clusters.reserve(count);
		if (contiguous) {
			if (first - FIRST_CLUSTER + count > cluster_count) {
				throw PackageError("The exFAT image is corrupt (file extends past the volume).");
			}
			for (uint64_t i = 0; i < count; i++) {
				clusters.push_back(first + i);
			}
			return clusters;
		}

		uint32_t cluster = first;
		while (clusters.size() < count) {
			if (!ValidCluster(cluster)) {
				throw PackageError("The exFAT image is corrupt (broken cluster chain).");
			}
			clusters.push_back(cluster);
			uint8_t next[4];
			image->Read(fat_offset + static_cast<uint64_t>(cluster) * 4, next, sizeof(next));
			cluster = Le32(next);
		}
		return clusters;
	}

	// The root directory has no recorded length, so its chain runs until the FAT ends it.
	[[nodiscard]] std::vector<uint64_t> RootChain() const {
		std::vector<uint64_t> clusters;
		uint32_t              cluster = root_cluster;
		while (cluster != END_OF_CHAIN) {
			if (!ValidCluster(cluster) || clusters.size() >= cluster_count) {
				throw PackageError("The exFAT image is corrupt (broken root directory chain).");
			}
			clusters.push_back(cluster);
			uint8_t next[4];
			image->Read(fat_offset + static_cast<uint64_t>(cluster) * 4, next, sizeof(next));
			cluster = Le32(next);
		}
		return clusters;
	}

	[[nodiscard]] ReaderPtr Open(std::vector<uint64_t> clusters, uint64_t size) const {
		// Clusters are numbered from 2 at the start of the cluster heap.
		for (auto& cluster: clusters) {
			cluster -= FIRST_CLUSTER;
		}
		return MakeBlockListReader(heap, std::move(clusters), cluster_size, size);
	}
};

void AppendUtf8(std::string& out, uint32_t cp) {
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

std::string Utf16ToUtf8(const std::u16string& text) {
	std::string out;
	for (size_t i = 0; i < text.size(); i++) {
		uint32_t cp = text[i];
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
		    text[i + 1] <= 0xDFFF) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
			i++;
		} else if (cp >= 0xD800 && cp <= 0xDFFF) {
			cp = 0xFFFD;
		}
		AppendUtf8(out, cp);
	}
	return out;
}

struct DirectoryItem {
	std::string name;
	bool        directory  = false;
	uint32_t    first      = 0;
	uint64_t    valid      = 0;
	uint64_t    length     = 0;
	bool        contiguous = false;
};

std::vector<DirectoryItem> ReadDirectory(const ReaderPtr& directory) {
	std::vector<DirectoryItem> items;
	const uint64_t             count = directory->Size() / ENTRY_SIZE;
	std::vector<uint8_t>       raw(count * ENTRY_SIZE);
	directory->Read(0, raw.data(), raw.size());

	for (uint64_t i = 0; i < count; i++) {
		const uint8_t* entry = raw.data() + i * ENTRY_SIZE;
		if (entry[0] == ENTRY_END_OF_DIRECTORY) {
			break;
		}
		if (entry[0] != ENTRY_FILE) {
			continue;
		}
		const uint8_t secondary = entry[1];
		if (secondary < 2 || i + secondary >= count || raw[(i + 1) * ENTRY_SIZE] != ENTRY_STREAM) {
			throw PackageError("The exFAT image is corrupt (invalid file entry set).");
		}
		const uint8_t* stream = raw.data() + (i + 1) * ENTRY_SIZE;

		DirectoryItem item;
		item.directory  = (Le16(entry + 4) & ATTRIBUTE_DIRECTORY) != 0;
		item.contiguous = (stream[1] & FLAG_NO_FAT_CHAIN) != 0;
		item.valid      = Le64(stream + 8);
		item.first      = Le32(stream + 20);
		item.length     = Le64(stream + 24);
		if (item.valid > item.length) {
			throw PackageError("The exFAT image is corrupt (invalid file length).");
		}

		const size_t   name_length = stream[3];
		std::u16string name;
		for (uint64_t n = i + 2; n <= i + secondary && name.size() < name_length; n++) {
			const uint8_t* part = raw.data() + n * ENTRY_SIZE;
			if (part[0] != ENTRY_FILE_NAME) {
				break;
			}
			for (size_t c = 0; c < NAME_CHARS_PER_ENTRY && name.size() < name_length; c++) {
				name += static_cast<char16_t>(Le16(part + 2 + c * 2));
			}
		}
		if (name.size() != name_length) {
			throw PackageError("The exFAT image is corrupt (truncated file name).");
		}
		item.name = Utf16ToUtf8(name);
		items.push_back(std::move(item));
		i += secondary;
	}
	return items;
}

} // namespace

bool ExfatImage::HasBootSector(Reader& image) {
	if (image.Size() < BOOT_SECTOR_SIZE) {
		return false;
	}
	uint8_t boot[11];
	image.Read(0, boot, sizeof(boot));
	return std::memcmp(boot + 3, SIGNATURE, 8) == 0;
}

ExfatImage::ExfatImage(const ReaderPtr& image) {
	if (!HasBootSector(*image)) {
		throw PackageError("The file is not a valid exFAT image.");
	}
	uint8_t boot[BOOT_SECTOR_SIZE];
	image->Read(0, boot, sizeof(boot));

	const uint8_t sector_shift  = boot[108];
	const uint8_t cluster_shift = boot[109];
	if (sector_shift < 9 || sector_shift > 12 || sector_shift + cluster_shift > 25) {
		throw PackageError("The exFAT image is corrupt (invalid geometry).");
	}

	Volume volume;
	volume.image                    = image;
	const uint64_t bytes_per_sector = 1ULL << sector_shift;
	volume.fat_offset               = Le32(boot + 80) * bytes_per_sector;
	volume.heap_offset              = Le32(boot + 88) * bytes_per_sector;
	volume.cluster_size             = static_cast<uint32_t>(1ULL << (sector_shift + cluster_shift));
	volume.cluster_count            = Le32(boot + 92);
	volume.root_cluster             = Le32(boot + 96);
	if (volume.cluster_count == 0 || !volume.ValidCluster(volume.root_cluster) ||
	    volume.heap_offset > image->Size()) {
		throw PackageError("The exFAT image is corrupt (invalid root directory).");
	}
	volume.heap = MakeSubReader(image, volume.heap_offset, image->Size() - volume.heap_offset);

	const auto root_clusters = volume.RootChain();
	const auto root_size     = static_cast<uint64_t>(root_clusters.size()) * volume.cluster_size;

	std::set<uint32_t>                             visited {volume.root_cluster};
	std::vector<std::pair<ReaderPtr, std::string>> pending {
	    {volume.Open(root_clusters, root_size), std::string()}};
	while (!pending.empty()) {
		auto [directory, prefix] = std::move(pending.back());
		pending.pop_back();

		for (auto& item: ReadDirectory(directory)) {
			if (!IsSafePathComponent(item.name)) {
				throw PackageError("The exFAT image contains an invalid file name.");
			}
			std::string path     = prefix.empty() ? item.name : prefix + "/" + item.name;
			auto        clusters = volume.Chain(item.first, item.length, item.contiguous);

			if (item.directory) {
				if (!visited.insert(item.first).second) {
					throw PackageError("The exFAT image is corrupt (directory loop).");
				}
				m_entries.push_back({path, true, 0, {}});
				pending.emplace_back(volume.Open(std::move(clusters), item.length),
				                     std::move(path));
				continue;
			}

			// Bytes past the valid data length read as zero, so only expose the valid part when
			// the tail is unwritten.
			const uint64_t valid = item.valid;
			const uint64_t size  = item.length;
			m_entries.push_back({std::move(path), false, size,
			                     [volume, clusters = std::move(clusters), valid, size]() {
				                     auto data = volume.Open(clusters, valid);
				                     if (valid == size) {
					                     return data;
				                     }
				                     return MakeZeroExtendedReader(data, size);
			                     }});
		}
	}
}

} // namespace Package
