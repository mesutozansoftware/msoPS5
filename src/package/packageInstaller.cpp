#include "package/packageInstaller.h"

#include "package/exfatImage.h"
#include "package/packageCrypto.h"
#include "package/pfsImage.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace Package {

namespace {

namespace fs = std::filesystem;

constexpr uint8_t  FIH_MAGIC[4]          = {0x7F, 'F', 'I', 'H'};
constexpr uint8_t  CNT_MAGIC[4]          = {0x7F, 'C', 'N', 'T'};
constexpr uint8_t  FIH_RETAIL            = 0x80;
constexpr uint64_t FIH_HEADER_SIZE       = 0x10000;
constexpr uint64_t CNT_CONTENT_ID_OFFSET = 0x40;
constexpr size_t   CONTENT_ID_LENGTH     = 36;
constexpr uint64_t UFS2_SUPERBLOCK       = 65536;
constexpr uint64_t UFS2_MAGIC_OFFSET     = 1372;
constexpr uint32_t UFS2_MAGIC            = 0x19540119;
constexpr size_t   COPY_CHUNK            = 4 * 1024 * 1024;
constexpr uint64_t MAX_PARAM_JSON        = 1024 * 1024;

struct OpenedPackage {
	std::vector<TreeEntry> entries;
	std::string            content_id;
};

bool HasUfsSuperblock(Reader& image) {
	if (image.Size() < UFS2_SUPERBLOCK + UFS2_MAGIC_OFFSET + 4) {
		return false;
	}
	uint8_t magic[4];
	image.Read(UFS2_SUPERBLOCK + UFS2_MAGIC_OFFSET, magic, sizeof(magic));
	return Le32(magic) == UFS2_MAGIC;
}

[[noreturn]] void ThrowUfsUnsupported() {
	throw PackageError("This is a UFS game image (.ffpkg). msoPS5 cannot install UFS images yet; "
	                   "use an exFAT or PFS image of the game, or the game folder itself.");
}

bool ContainsGame(const std::vector<TreeEntry>& entries) {
	return std::any_of(entries.begin(), entries.end(), [](const TreeEntry& entry) {
		return !entry.directory &&
		       (entry.path == "eboot.bin" ||
		        (entry.path.size() > 10 &&
		         entry.path.compare(entry.path.size() - 10, 10, "/eboot.bin") == 0));
	});
}

// Opens a filesystem image (PFS or exFAT). When a PFS image only wraps a single image file,
// as .ffpfs/.ffpfsc files usually do, that inner image is opened instead.
std::vector<TreeEntry> OpenImage(const ReaderPtr& image, const std::vector<Crypto::Digest256>& keys,
                                 int depth = 0) {
	if (ExfatImage::HasBootSector(*image)) {
		return ExfatImage(image).Entries();
	}
	if (HasUfsSuperblock(*image)) {
		ThrowUfsUnsupported();
	}
	if (!PfsImage::HasSuperblock(*image)) {
		throw PackageError("The file is not a supported PS5 package or game image.");
	}

	PfsImage pfs(image, keys);
	auto     entries = pfs.Entries();
	if (depth == 0 && !ContainsGame(entries) && entries.size() == 1 && !entries[0].directory) {
		auto inner_keys = keys;
		if (pfs.Key()) {
			inner_keys.insert(inner_keys.begin(), *pfs.Key());
		}
		return OpenImage(entries[0].open(), inner_keys, depth + 1);
	}
	return entries;
}

std::string ReadContentId(Reader& file, uint64_t cnt_offset) {
	if (cnt_offset == 0 || cnt_offset > file.Size() ||
	    file.Size() - cnt_offset < CNT_CONTENT_ID_OFFSET + CONTENT_ID_LENGTH) {
		return {};
	}
	uint8_t magic[4];
	file.Read(cnt_offset, magic, sizeof(magic));
	if (std::memcmp(magic, CNT_MAGIC, sizeof(magic)) != 0) {
		return {};
	}
	std::string id(CONTENT_ID_LENGTH, '\0');
	file.Read(cnt_offset + CNT_CONTENT_ID_OFFSET, id.data(), id.size());
	const bool printable = std::all_of(id.begin(), id.end(), [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
	});
	return printable ? id : std::string();
}

// A finalized PS5 package: a plaintext header, an outer PFS holding pfs_image.dat (the game's
// inner PFS, usually PFSC compressed) and the metadata container.
OpenedPackage OpenFinalizedPackage(const ReaderPtr& file, const InstallOptions& options) {
	uint8_t header[0x100];
	file->Read(0, header, sizeof(header));

	if (header[0x05] == FIH_RETAIL) {
		throw PackageError(
		    "This is a retail PS5 package. Retail packages are encrypted with console keys and "
		    "cannot be installed; use a fake package (fPKG) or a dump of the game folder.");
	}

	uint64_t       pfs_offset = Le64(header + 0x10);
	uint64_t       pfs_size   = Le64(header + 0x18);
	const uint64_t superblock = Le64(header + 0x20);
	if (pfs_offset == 0 || pfs_offset >= file->Size()) {
		pfs_offset = FIH_HEADER_SIZE;
	}
	if (pfs_offset >= file->Size()) {
		throw PackageError("The package is truncated.");
	}
	if (pfs_size == 0 || pfs_size > file->Size() - pfs_offset) {
		pfs_size = file->Size() - pfs_offset;
	}

	OpenedPackage package;
	package.content_id = ReadContentId(*file, Le64(header + 0x58));

	std::vector<Crypto::Digest256> keys;
	if (package.content_id.size() == CONTENT_ID_LENGTH) {
		keys.push_back(Crypto::ComputeEkpfs(package.content_id, options.passcode, false));
		keys.push_back(Crypto::ComputeEkpfs(package.content_id, options.passcode, true));
	}

	const auto outer_image = MakeSubReader(file, pfs_offset, pfs_size);
	if (!PfsImage::HasSuperblock(*outer_image)) {
		if (superblock != 0 && superblock != pfs_offset) {
			throw PackageError("This package uses the network-install (\"data-first\") layout, "
			                   "which msoPS5 cannot unpack yet.");
		}
		throw PackageError("The package is corrupt (no PFS image found).");
	}

	const PfsImage outer(outer_image, keys);
	const auto&    outer_entries = outer.Entries();
	const auto     find          = [&outer_entries](const char* name) {
        return std::find_if(outer_entries.begin(), outer_entries.end(),
		                                 [name](const TreeEntry& e) { return e.path == name; });
	};

	const auto nested = find("pfs_image.dat");
	if (nested == outer_entries.end()) {
		package.entries = outer_entries;
		return package;
	}

	auto inner_keys = keys;
	if (outer.Key()) {
		inner_keys.insert(inner_keys.begin(), *outer.Key());
	}
	const auto inner_image = nested->open();
	if (!PfsImage::HasSuperblock(*inner_image) &&
	    find("naps_pkg_layout.dat") != outer_entries.end()) {
		throw PackageError("This package uses the network-install (\"data-first\") layout, "
		                   "which msoPS5 cannot unpack yet.");
	}
	package.entries = OpenImage(inner_image, inner_keys);
	return package;
}

OpenedPackage OpenPackage(const fs::path& path, const InstallOptions& options) {
	const auto file = OpenFileReader(path);
	if (file->Size() < 4) {
		throw PackageError("The file is too small to be a PS5 package.");
	}
	uint8_t magic[4];
	file->Read(0, magic, sizeof(magic));

	if (std::memcmp(magic, FIH_MAGIC, sizeof(magic)) == 0) {
		return OpenFinalizedPackage(file, options);
	}
	if (std::memcmp(magic, CNT_MAGIC, sizeof(magic)) == 0) {
		throw PackageError("This is a PS4 package (or a bare PS5 metadata container). msoPS5 "
		                   "installs PS5 games only.");
	}
	// Encrypted standalone PFS images are made with the all-zero EKPFS.
	return {OpenImage(file, {Crypto::Digest256 {}}), {}};
}

// The game is the shallowest directory that contains eboot.bin.
std::string FindGameRoot(const std::vector<TreeEntry>& entries) {
	std::optional<std::string> best;
	for (const auto& entry: entries) {
		if (entry.directory) {
			continue;
		}
		std::string dir;
		if (entry.path == "eboot.bin") {
			dir = "";
		} else if (entry.path.size() > 10 &&
		           entry.path.compare(entry.path.size() - 10, 10, "/eboot.bin") == 0) {
			dir = entry.path.substr(0, entry.path.size() - 10);
		} else {
			continue;
		}
		const auto depth = [](const std::string& p) {
			return p.empty() ? 0 : 1 + std::count(p.begin(), p.end(), '/');
		};
		if (!best || depth(dir) < depth(*best)) {
			best = dir;
		}
	}
	if (!best) {
		throw PackageError("The package does not contain a PS5 game (no eboot.bin found).");
	}
	return *best;
}

std::string RelativeTo(const std::string& path, const std::string& root) {
	if (root.empty()) {
		return path;
	}
	if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
	    path[root.size()] == '/') {
		return path.substr(root.size() + 1);
	}
	return {};
}

bool IsTitleId(const std::string& id) {
	return id.size() == 9 &&
	       std::all_of(id.begin(), id.begin() + 4,
	                   [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; }) &&
	       std::all_of(id.begin() + 4, id.end(),
	                   [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string ReadTitleId(const std::vector<TreeEntry>& entries, const std::string& root) {
	const std::string param = root.empty() ? "sce_sys/param.json" : root + "/sce_sys/param.json";
	for (const auto& entry: entries) {
		if (entry.directory || entry.path != param || entry.size > MAX_PARAM_JSON) {
			continue;
		}
		try {
			const auto  reader = entry.open();
			std::string text(reader->Size(), '\0');
			reader->Read(0, text.data(), text.size());
			const auto json = nlohmann::json::parse(text, nullptr, false);
			if (json.is_object() && json.contains("titleId") && json["titleId"].is_string()) {
				const auto id = json["titleId"].get<std::string>();
				if (IsTitleId(id)) {
					return id;
				}
			}
		} catch (const std::exception&) {
			// An unreadable param.json only costs the folder name; fall back below.
		}
	}
	return {};
}

std::string SanitizeFolderName(std::string name) {
	for (auto& c: name) {
		if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) {
			c = '_';
		}
	}
	while (!name.empty() && (name.front() == '.' || name.front() == ' ')) {
		name.erase(name.begin());
	}
	return name.empty() ? std::string("game") : name;
}

std::string ChooseFolderName(const std::string& title_id, const std::string& content_id,
                             const fs::path& package) {
	if (!title_id.empty()) {
		return title_id;
	}
	// Content ids look like "UP9000-PPSA01234_00-GAMENAME00000000".
	if (content_id.size() == CONTENT_ID_LENGTH && IsTitleId(content_id.substr(7, 9))) {
		return content_id.substr(7, 9);
	}
	return SanitizeFolderName(package.stem().string());
}

void CopyFile(const TreeEntry& entry, const fs::path& destination, InstallProgress& progress,
              const InstallOptions& options, std::vector<char>& buffer) {
	const auto    reader = entry.open();
	std::ofstream out(destination, std::ios::binary | std::ios::trunc);
	if (!out) {
		throw PackageError("Could not create \"" + destination.string() + "\".");
	}
	const uint64_t size = reader->Size();
	for (uint64_t offset = 0; offset < size;) {
		const auto chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - offset));
		reader->Read(offset, buffer.data(), chunk);
		out.write(buffer.data(), static_cast<std::streamsize>(chunk));
		if (!out) {
			throw PackageError("Could not write \"" + destination.string() +
			                   "\". Is the disk full?");
		}
		offset += chunk;
		progress.bytes_done += chunk;
		if (options.progress && !options.progress(progress)) {
			throw InstallCancelled("The installation was cancelled.");
		}
	}
}

} // namespace

InstallResult InstallPackage(const fs::path& package, const fs::path& games_dir,
                             const InstallOptions& options) {
	if (options.passcode.size() != 32) {
		throw PackageError("The package passcode must be 32 characters long.");
	}
	std::error_code error;
	if (!fs::is_directory(games_dir, error)) {
		throw PackageError("The game folder \"" + games_dir.string() + "\" does not exist.");
	}

	const auto        opened = OpenPackage(package, options);
	const std::string root   = FindGameRoot(opened.entries);

	InstallResult result;
	result.title_id = ReadTitleId(opened.entries, root);
	result.game_dir = games_dir / ChooseFolderName(result.title_id, opened.content_id, package);
	if (fs::exists(result.game_dir, error)) {
		throw PackageError("\"" + result.game_dir.filename().string() +
		                   "\" is already installed in this game folder. Remove it first to "
		                   "reinstall.");
	}

	std::vector<const TreeEntry*> selected;
	InstallProgress               progress;
	for (const auto& entry: opened.entries) {
		if (!RelativeTo(entry.path, root).empty()) {
			selected.push_back(&entry);
			progress.bytes_total += entry.directory ? 0 : entry.size;
		}
	}
	const auto space = fs::space(games_dir, error);
	if (!error && space.available < progress.bytes_total) {
		throw PackageError("Not enough free disk space to install this game.");
	}

	const fs::path staging =
	    games_dir / ("." + result.game_dir.filename().string() + ".msops5-install");
	fs::remove_all(staging, error);
	try {
		fs::create_directories(staging);
		std::vector<char> buffer(COPY_CHUNK);
		for (const auto* entry: selected) {
			const fs::path target = staging / fs::path(RelativeTo(entry->path, root));
			if (entry->directory) {
				fs::create_directories(target);
				continue;
			}
			fs::create_directories(target.parent_path());
			progress.current_file = entry->path;
			CopyFile(*entry, target, progress, options, buffer);
			result.files++;
		}
		fs::rename(staging, result.game_dir);
	} catch (...) {
		fs::remove_all(staging, error);
		throw;
	}

	result.bytes = progress.bytes_done;
	return result;
}

} // namespace Package
