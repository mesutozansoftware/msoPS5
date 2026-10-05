// Tests for the PS5 package installer. Run with the directory written by
// tests/data/package/generate_fixtures.py as the only argument.

#include "package/packageCrypto.h"
#include "package/packageInstaller.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void Check(bool value, const std::string& text) {
	if (!value) {
		std::fprintf(stderr, "PackageInstallerTests: failed: %s\n", text.c_str());
		std::exit(1);
	}
}

std::string Hex(const uint8_t* data, size_t size) {
	static constexpr char DIGITS[] = "0123456789abcdef";
	std::string           out;
	for (size_t i = 0; i < size; i++) {
		out += DIGITS[data[i] >> 4];
		out += DIGITS[data[i] & 0xF];
	}
	return out;
}

std::vector<char> ReadAll(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::map<std::string, std::vector<char>> ReadTree(const fs::path& root) {
	std::map<std::string, std::vector<char>> files;
	for (const auto& entry: fs::recursive_directory_iterator(root)) {
		if (entry.is_regular_file()) {
			files[fs::relative(entry.path(), root).generic_string()] = ReadAll(entry.path());
		}
	}
	return files;
}

class TempDirectory {
public:
	TempDirectory() {
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path = fs::temp_directory_path() / ("msops5_package_test_" + std::to_string(unique));
		fs::create_directories(m_path);
	}
	~TempDirectory() {
		std::error_code error;
		fs::remove_all(m_path, error);
	}
	TempDirectory(const TempDirectory&)            = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;

	[[nodiscard]] const fs::path& Path() const { return m_path; }

private:
	fs::path m_path;
};

void TestKnownAnswers() {
	using namespace Package::Crypto;

	const auto abc = Sha3_256("abc", 3);
	Check(Hex(abc.data(), abc.size()) ==
	          "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
	      "SHA3-256(abc)");
	const auto empty = Sha3_256("", 0);
	Check(Hex(empty.data(), empty.size()) ==
	          "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
	      "SHA3-256(empty)");
	const std::vector<uint8_t> a3(200, 0xA3); // longer than one SHA3-256 block
	const auto                 long_hash = Sha3_256(a3.data(), a3.size());
	Check(Hex(long_hash.data(), long_hash.size()) ==
	          "79f38adec5c20307a98ef76e8324afbfd46cfd81b22e3973c65fa1bd9de31787",
	      "SHA3-256(200 x 0xA3)");

	const std::vector<uint8_t> key(20, 0x0B);
	const auto                 mac = HmacSha256(key.data(), key.size(), "Hi There", 8);
	Check(Hex(mac.data(), mac.size()) ==
	          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
	      "HMAC-SHA256 (RFC 4231 case 1)");

	// IEEE 1619 XTS-AES-128 vector 2.
	const std::vector<uint8_t> key1(16, 0x11);
	const std::vector<uint8_t> key2(16, 0x22);
	const uint8_t              ciphertext[32] = {0xc4, 0x54, 0x18, 0x5e, 0x6a, 0x16, 0x93, 0x6e,
	                                             0x39, 0x33, 0x40, 0x38, 0xac, 0xef, 0x83, 0x8b,
	                                             0xfb, 0x18, 0x6f, 0xff, 0x74, 0x80, 0xad, 0xc4,
	                                             0x28, 0x93, 0x82, 0xec, 0xd6, 0xd3, 0x94, 0xf0};
	uint8_t                    buffer[32];
	std::memcpy(buffer, ciphertext, sizeof(buffer));
	AesXtsDecryptor(key1.data(), key2.data())
	    .DecryptSector(0x3333333333ULL, buffer, sizeof(buffer));
	for (const uint8_t byte: buffer) {
		Check(byte == 0x44, "XTS-AES-128 (IEEE 1619 vector 2)");
	}
}

void TestInstall(const fs::path& fixtures, const char* package, const char* expected) {
	TempDirectory games;
	const auto    result = Package::InstallPackage(fixtures / package, games.Path());
	const auto    name   = std::string(package);

	Check(result.title_id == "PPSA01234", name + ": title id");
	Check(result.game_dir == games.Path() / "PPSA01234", name + ": game folder");
	const auto installed = ReadTree(result.game_dir);
	const auto reference = ReadTree(fixtures / "expected" / expected);
	Check(installed.size() == reference.size(), name + ": file count");
	for (const auto& [path, data]: reference) {
		const auto found = installed.find(path);
		Check(found != installed.end(), name + ": missing " + path);
		Check(found->second == data, name + ": contents of " + path);
	}
	Check(result.files == reference.size(), name + ": reported file count");

	// Nothing but the game folder is left behind, and a second install is refused.
	Check(std::distance(fs::directory_iterator(games.Path()), fs::directory_iterator()) == 1,
	      name + ": stray files in the game folder");
	bool refused = false;
	try {
		Package::InstallPackage(fixtures / package, games.Path());
	} catch (const Package::PackageError& e) {
		refused = std::string(e.what()).find("already installed") != std::string::npos;
	}
	Check(refused, name + ": reinstall is refused");
}

void ExpectError(const fs::path& package, const char* fragment) {
	TempDirectory games;
	std::string   message;
	try {
		Package::InstallPackage(package, games.Path());
	} catch (const Package::PackageError& e) {
		message = e.what();
	}
	Check(message.find(fragment) != std::string::npos,
	      package.filename().string() + ": expected an error mentioning \"" + fragment +
	          "\", got \"" + message + "\"");
	Check(fs::is_empty(games.Path()), package.filename().string() + ": left files behind");
}

void TestCancel(const fs::path& fixtures) {
	TempDirectory           games;
	Package::InstallOptions options;
	options.progress = [](const Package::InstallProgress& progress) {
		return progress.bytes_done < progress.bytes_total / 2;
	};
	bool cancelled = false;
	try {
		Package::InstallPackage(fixtures / "plain.ffpfs", games.Path(), options);
	} catch (const Package::InstallCancelled&) {
		cancelled = true;
	}
	Check(cancelled, "cancel: InstallCancelled thrown");
	Check(fs::is_empty(games.Path()), "cancel: partial install removed");
}

void TestRejectedInputs(const fs::path& fixtures) {
	ExpectError(fixtures / "retail.pkg", "retail");

	TempDirectory scratch;
	const auto    write = [&scratch](const char* name, const std::vector<uint8_t>& data) {
        const auto path = scratch.Path() / name;
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()),
		              static_cast<std::streamsize>(data.size()));
        return path;
	};

	std::vector<uint8_t> ufs(70000, 0);
	const uint8_t        ufs_magic[4] = {0x19, 0x01, 0x54, 0x19};
	std::memcpy(ufs.data() + 65536 + 1372, ufs_magic, sizeof(ufs_magic));
	ExpectError(write("game.ffpkg", ufs), "UFS");

	std::vector<uint8_t> ps4(64, 0);
	std::memcpy(ps4.data(),
	            "\x7F"
	            "CNT",
	            4);
	ExpectError(write("ps4.pkg", ps4), "PS4");

	ExpectError(write("random.bin", std::vector<uint8_t>(8192, 0x5A)), "not a supported");
}

} // namespace

int main(int argc, char* argv[]) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]);
		return 2;
	}
	const fs::path fixtures = argv[1];

	TestKnownAnswers();
	for (const char* package:
	     {"plain.ffpfs", "compressed.ffpfsc", "signed32.ffpfs", "signed64.ffpfs", "fake.pkg"}) {
		TestInstall(fixtures, package, "game");
	}
	for (const char* package: {"game.exfat", "exfat-wrapped.ffpfsc"}) {
		TestInstall(fixtures, package, "exfat-game");
	}
	TestCancel(fixtures);
	TestRejectedInputs(fixtures);

	std::printf("PackageInstallerTests: all tests passed\n");
	return 0;
}
