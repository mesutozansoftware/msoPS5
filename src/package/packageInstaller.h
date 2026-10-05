#ifndef PACKAGE_PACKAGE_INSTALLER_H
#define PACKAGE_PACKAGE_INSTALLER_H

#include "package/packageReader.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Package {

struct InstallProgress {
	uint64_t    bytes_done  = 0;
	uint64_t    bytes_total = 0;
	std::string current_file;
};

struct InstallOptions {
	// Passcode of a fake package (32 characters); fPKGs normally use all zeros.
	std::string passcode = std::string(32, '0');
	// Called while files are copied; returning false cancels the installation.
	std::function<bool(const InstallProgress&)> progress;
};

struct InstallResult {
	std::filesystem::path game_dir;
	std::string           title_id;
	uint64_t              files = 0;
	uint64_t              bytes = 0;
};

class InstallCancelled: public PackageError {
public:
	using PackageError::PackageError;
};

// Installs a PS5 game package into a new folder below `games_dir`.
//
// Supported inputs: fake/debug PS5 packages (.pkg, finalized "FIH" images with a zlib or
// uncompressed inner image), PFS images (.ffpfs/.ffpfsc, optionally wrapping an exFAT image)
// and exFAT images (.exfat). Throws PackageError with a user-facing message on failure and
// InstallCancelled when the progress callback cancels; nothing is left behind in either case.
InstallResult InstallPackage(const std::filesystem::path& package,
                             const std::filesystem::path& games_dir,
                             const InstallOptions&        options = {});

} // namespace Package

#endif // PACKAGE_PACKAGE_INSTALLER_H
