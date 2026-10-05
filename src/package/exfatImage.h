#ifndef PACKAGE_EXFAT_IMAGE_H
#define PACKAGE_EXFAT_IMAGE_H

#include "package/packageReader.h"

#include <vector>

namespace Package {

// Read-only exFAT volume image, as used by .exfat game images and inside .ffpfs wrappers.
class ExfatImage {
public:
	explicit ExfatImage(const ReaderPtr& image);

	// Every file and directory of the volume, parents before children.
	[[nodiscard]] const std::vector<TreeEntry>& Entries() const { return m_entries; }

	// Returns true when `image` starts with an exFAT boot sector.
	static bool HasBootSector(Reader& image);

private:
	std::vector<TreeEntry> m_entries;
};

} // namespace Package

#endif // PACKAGE_EXFAT_IMAGE_H
