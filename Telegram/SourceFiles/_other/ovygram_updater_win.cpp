#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace {

namespace fs = std::filesystem;

constexpr DWORD kParentExitTimeoutMs = 15000;
constexpr DWORD kHostExitTimeoutMs = 5000;
constexpr size_t kMaxArchiveEntries = 4096;
constexpr uint64_t kMaxUncompressedTotal = 1024ULL * 1024 * 1024;

std::wofstream gLog;

void Log(const std::wstring &line) {
	if (gLog.is_open()) {
		gLog << line << L"\n";
		gLog.flush();
	}
}

std::wstring ToLower(std::wstring value) {
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
		return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
	});
	return value;
}

std::wstring Utf8ToWide(const std::string &utf8) {
	if (utf8.empty()) {
		return {};
	}
	const auto needed = MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		utf8.data(),
		static_cast<int>(utf8.size()),
		nullptr,
		0);
	if (needed <= 0) {
		return {};
	}
	auto result = std::wstring(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		utf8.data(),
		static_cast<int>(utf8.size()),
		result.data(),
		needed);
	return result;
}

std::wstring QuoteArg(const std::wstring &arg) {
	if (arg.empty()) {
		return L"\"\"";
	}
	const auto needsQuotes = arg.find_first_of(L" \t\"") != std::wstring::npos;
	if (!needsQuotes) {
		return arg;
	}
	auto out = std::wstring(L"\"");
	size_t backslashes = 0;
	for (const auto ch : arg) {
		if (ch == L'\\') {
			++backslashes;
		} else if (ch == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
			backslashes = 0;
		} else {
			if (backslashes > 0) {
				out.append(backslashes, L'\\');
				backslashes = 0;
			}
			out.push_back(ch);
		}
	}
	if (backslashes > 0) {
		out.append(backslashes * 2, L'\\');
	}
	out.push_back(L'"');
	return out;
}

bool ComputeFileSha256Hex(const fs::path &path, std::wstring &hexOut) {
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
		return false;
	}
	DWORD objLen = 0;
	DWORD cbData = 0;
	DWORD hashLen = 0;
	const auto cleanup = [&] {
		if (hash) BCryptDestroyHash(hash);
		if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	};
	if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbData, 0) != 0
		|| BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cbData, 0) != 0
		|| hashLen != 32) {
		cleanup();
		return false;
	}
	auto objBuf = std::vector<UCHAR>(objLen);
	if (BCryptCreateHash(alg, &hash, objBuf.data(), objLen, nullptr, 0, 0) != 0) {
		cleanup();
		return false;
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open()) {
		cleanup();
		return false;
	}
	auto buffer = std::vector<char>(65536);
	while (stream) {
		stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const auto got = stream.gcount();
		if (got > 0) {
			if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0) != 0) {
				cleanup();
				return false;
			}
		}
	}
	UCHAR digest[32] = {};
	if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
		cleanup();
		return false;
	}
	cleanup();

	static constexpr wchar_t kHex[] = L"0123456789abcdef";
	hexOut.clear();
	hexOut.reserve(64);
	for (const auto byte : digest) {
		hexOut.push_back(kHex[(byte >> 4) & 0xF]);
		hexOut.push_back(kHex[byte & 0xF]);
	}
	return true;
}

void WaitForParentProcess(DWORD pid, ULONGLONG expectedCreated) {
	if (!pid || pid == GetCurrentProcessId()) {
		return;
	}
	HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc) {
		return;
	}
	FILETIME created = {}, exited = {}, kernel = {}, user = {};
	if (expectedCreated != 0 && GetProcessTimes(proc, &created, &exited, &kernel, &user)) {
		const auto actual = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
		if (actual != expectedCreated) {
			CloseHandle(proc);
			return;
		}
	}
	Log(L"Waiting for parent process " + std::to_wstring(pid));
	if (WaitForSingleObject(proc, kParentExitTimeoutMs) == WAIT_TIMEOUT) {
		Log(L"Parent process timed out; terminating.");
		HANDLE term = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (term) {
			if (expectedCreated == 0
				|| (GetProcessTimes(term, &created, &exited, &kernel, &user)
					&& (((ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime) == expectedCreated))) {
				TerminateProcess(term, 1);
				WaitForSingleObject(proc, 5000);
			}
			CloseHandle(term);
		}
	}
	CloseHandle(proc);
}

void TerminateLingeringPythonHosts(const fs::path &targetDir) {
	const auto pythonPrefix = ToLower((targetDir / L"python").lexically_normal().wstring()) + L"\\";
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return;
	}
	std::vector<DWORD> pids;
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snap, &entry)) {
		do {
			if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId()) {
				continue;
			}
			HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (!proc) {
				continue;
			}
			wchar_t imagePath[MAX_PATH * 4] = {};
			DWORD len = static_cast<DWORD>(std::size(imagePath));
			if (QueryFullProcessImageNameW(proc, 0, imagePath, &len)) {
				const auto lowered = ToLower(fs::path(imagePath).lexically_normal().wstring());
				if (lowered.rfind(pythonPrefix, 0) == 0) {
					pids.push_back(entry.th32ProcessID);
				}
			}
			CloseHandle(proc);
		} while (Process32NextW(snap, &entry));
	}
	CloseHandle(snap);

	for (const auto pid : pids) {
		HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
		if (!proc) {
			continue;
		}
		Log(L"Waiting for lingering python host process " + std::to_wstring(pid));
		if (WaitForSingleObject(proc, kHostExitTimeoutMs) == WAIT_TIMEOUT) {
			Log(L"Terminating lingering python host process " + std::to_wstring(pid));
			TerminateProcess(proc, 1);
			WaitForSingleObject(proc, 3000);
		}
		CloseHandle(proc);
	}
}

bool IsDirectoryWritable(const fs::path &dir) {
	const auto probe = dir / (L".ovygram_write_probe_" + std::to_wstring(GetCurrentProcessId()));
	HANDLE h = CreateFileW(
		probe.c_str(),
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
		nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	CloseHandle(h);
	return true;
}

bool IsSafeArchiveRelPath(const std::wstring &rel, fs::path &outPath) {
	if (rel.empty() || rel.front() == L'/' || rel.front() == L'\\') {
		return false;
	}
	if (rel.find(L':') != std::wstring::npos) {
		return false;
	}
	auto normalized = rel;
	std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
	fs::path built;
	std::wstringstream ss(normalized);
	std::wstring part;
	while (std::getline(ss, part, L'/')) {
		if (part.empty()) {
			continue;
		}
		if (part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') {
			return false;
		}
		const auto upper = [&] {
			auto stem = part.substr(0, part.find(L'.'));
			std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) {
				return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
			});
			return stem;
		}();
		if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL"
			|| (upper.size() == 4
				&& (upper.rfind(L"COM", 0) == 0 || upper.rfind(L"LPT", 0) == 0)
				&& upper[3] >= L'1' && upper[3] <= L'9')) {
			return false;
		}
		built /= part;
	}
	if (built.empty()) {
		return false;
	}
	outPath = built;
	return true;
}

uint16_t ReadLe16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0])
		| (static_cast<uint32_t>(p[1]) << 8)
		| (static_cast<uint32_t>(p[2]) << 16)
		| (static_cast<uint32_t>(p[3]) << 24);
}

bool ExtractZipSafe(const fs::path &zipPath, const fs::path &destDir) {
	std::ifstream file(zipPath, std::ios::binary | std::ios::ate);
	if (!file.is_open()) {
		Log(L"Cannot open zip: " + zipPath.wstring());
		return false;
	}
	const auto fileSize = static_cast<uint64_t>(file.tellg());
	if (fileSize < 22) {
		Log(L"Zip file too small.");
		return false;
	}

	// Locate End of Central Directory (EOCD) in the last 66 KiB.
	const auto tailSize = static_cast<size_t>(std::min<uint64_t>(fileSize, 65557));
	auto tail = std::vector<uint8_t>(tailSize);
	file.seekg(static_cast<std::streamoff>(fileSize - tailSize));
	file.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tailSize));

	int64_t eocdOffset = -1;
	for (int64_t i = static_cast<int64_t>(tailSize) - 22; i >= 0; --i) {
		if (ReadLe32(tail.data() + i) == 0x06054b50U) {
			const auto commentLen = ReadLe16(tail.data() + i + 20);
			if (static_cast<size_t>(i) + 22 + commentLen == tailSize) {
				eocdOffset = i;
				break;
			}
		}
	}
	if (eocdOffset < 0) {
		Log(L"ZIP EOCD not found.");
		return false;
	}
	const auto totalEntries = ReadLe16(tail.data() + eocdOffset + 10);
	const auto cdSize = ReadLe32(tail.data() + eocdOffset + 12);
	const auto cdOffset = ReadLe32(tail.data() + eocdOffset + 16);
	if (totalEntries == 0 || totalEntries > kMaxArchiveEntries
		|| static_cast<uint64_t>(cdOffset) + cdSize > fileSize) {
		Log(L"Invalid ZIP central directory bounds.");
		return false;
	}

	auto cd = std::vector<uint8_t>(cdSize);
	file.seekg(static_cast<std::streamoff>(cdOffset));
	file.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cdSize));
	if (!file) {
		return false;
	}

	uint64_t totalExtracted = 0;
	size_t pos = 0;
	for (uint16_t idx = 0; idx < totalEntries; ++idx) {
		if (pos + 46 > cd.size() || ReadLe32(cd.data() + pos) != 0x02014b50U) {
			Log(L"Malformed central directory header.");
			return false;
		}
		const auto method = ReadLe16(cd.data() + pos + 10);
		const auto expectedCrc = ReadLe32(cd.data() + pos + 16);
		const auto compSize = ReadLe32(cd.data() + pos + 20);
		const auto uncompSize = ReadLe32(cd.data() + pos + 24);
		const auto nameLen = ReadLe16(cd.data() + pos + 28);
		const auto extraLen = ReadLe16(cd.data() + pos + 30);
		const auto commentLen = ReadLe16(cd.data() + pos + 32);
		const auto localOffset = ReadLe32(cd.data() + pos + 42);
		if (pos + 46 + nameLen + extraLen + commentLen > cd.size()) {
			return false;
		}
		const auto rawName = std::string(
			reinterpret_cast<const char*>(cd.data() + pos + 46),
			nameLen);
		pos += 46 + nameLen + extraLen + commentLen;

		if (rawName.empty() || rawName.back() == '/' || rawName.back() == '\\') {
			continue;
		}
		const auto wideName = Utf8ToWide(rawName);
		fs::path relPath;
		if (!IsSafeArchiveRelPath(wideName, relPath)) {
			Log(L"Rejected unsafe archive entry: " + wideName);
			return false;
		}
		// Never overwrite or introduce TelegramForcePortable so installed copies stay installed.
		if (ToLower(relPath.wstring()) == L"telegramforceportable") {
			continue;
		}

		totalExtracted += uncompSize;
		if (totalExtracted > kMaxUncompressedTotal) {
			Log(L"Archive exceeds uncompressed limit.");
			return false;
		}

		// Read local file header.
		uint8_t localHdr[30] = {};
		file.seekg(static_cast<std::streamoff>(localOffset));
		file.read(reinterpret_cast<char*>(localHdr), sizeof(localHdr));
		if (!file || ReadLe32(localHdr) != 0x04034b50U) {
			Log(L"Malformed local file header.");
			return false;
		}
		const auto lNameLen = ReadLe16(localHdr + 26);
		const auto lExtraLen = ReadLe16(localHdr + 28);
		const auto dataOffset = static_cast<uint64_t>(localOffset) + 30 + lNameLen + lExtraLen;
		if (dataOffset + compSize > fileSize) {
			return false;
		}

		auto compBuf = std::vector<uint8_t>(compSize);
		if (compSize > 0) {
			file.seekg(static_cast<std::streamoff>(dataOffset));
			file.read(reinterpret_cast<char*>(compBuf.data()), static_cast<std::streamsize>(compSize));
			if (!file) {
				return false;
			}
		}

		auto outBuf = std::vector<uint8_t>(uncompSize);
		if (method == 0) {
			if (compSize != uncompSize) {
				return false;
			}
			outBuf = std::move(compBuf);
		} else if (method == 8) {
			z_stream zs = {};
			if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
				return false;
			}
			zs.next_in = compBuf.data();
			zs.avail_in = compSize;
			zs.next_out = outBuf.data();
			zs.avail_out = uncompSize;
			const auto status = inflate(&zs, Z_FINISH);
			inflateEnd(&zs);
			if (status != Z_STREAM_END || zs.total_out != uncompSize) {
				Log(L"Inflate failed for entry: " + wideName);
				return false;
			}
		} else {
			Log(L"Unsupported ZIP compression method.");
			return false;
		}

		const auto actualCrc = crc32(0L, outBuf.empty() ? nullptr : outBuf.data(), uncompSize);
		if (actualCrc != expectedCrc) {
			Log(L"CRC mismatch for entry: " + wideName);
			return false;
		}

		const auto targetPath = destDir / relPath;
		std::error_code ec;
		fs::create_directories(targetPath.parent_path(), ec);
		std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) {
			Log(L"Failed to create extracted file: " + targetPath.wstring());
			return false;
		}
		if (!outBuf.empty()) {
			out.write(reinterpret_cast<const char*>(outBuf.data()), static_cast<std::streamsize>(outBuf.size()));
		}
		if (!out) {
			return false;
		}
	}
	return true;
}

bool ReplaceFileAtomic(const fs::path &src, const fs::path &dst) {
	std::error_code ec;
	fs::create_directories(dst.parent_path(), ec);
	for (int attempt = 0; attempt < 20; ++attempt) {
		if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
			return true;
		}
		const auto err = GetLastError();
		if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
			const auto fresh = fs::path(dst.wstring() + L".new");
			const auto aside = fs::path(dst.wstring() + L".old");
			DeleteFileW(fresh.c_str());
			if (CopyFileW(src.c_str(), fresh.c_str(), FALSE)) {
				DeleteFileW(aside.c_str());
				if (MoveFileExW(dst.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
					if (MoveFileExW(fresh.c_str(), dst.c_str(), 0)) {
						MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
						return true;
					}
					MoveFileExW(aside.c_str(), dst.c_str(), 0);
				}
				DeleteFileW(fresh.c_str());
			}
		}
		Sleep(150);
	}
	Log(L"Failed to replace file: " + dst.wstring() + L" err=" + std::to_wstring(GetLastError()));
	return false;
}

bool SwapPythonDirectory(const fs::path &stagedPython, const fs::path &targetPython) {
	if (!fs::exists(stagedPython)) {
		return true;
	}
	std::error_code ec;
	const auto oldPython = fs::path(targetPython.wstring() + L".old");
	fs::remove_all(oldPython, ec);

	auto movedOld = false;
	if (fs::exists(targetPython)) {
		for (int attempt = 0; attempt < 20; ++attempt) {
			if (MoveFileExW(targetPython.c_str(), oldPython.c_str(), MOVEFILE_REPLACE_EXISTING)) {
				movedOld = true;
				break;
			}
			Sleep(150);
		}
		if (!movedOld) {
			Log(L"Could not move existing python directory aside; falling back to per-file replace.");
			for (const auto &entry : fs::recursive_directory_iterator(stagedPython)) {
				if (!entry.is_regular_file()) {
					continue;
				}
				const auto rel = fs::relative(entry.path(), stagedPython);
				if (!ReplaceFileAtomic(entry.path(), targetPython / rel)) {
					return false;
				}
			}
			return true;
		}
	}

	if (!MoveFileExW(stagedPython.c_str(), targetPython.c_str(), MOVEFILE_COPY_ALLOWED)) {
		// Copy recursively if staging is on a different volume.
		fs::copy(stagedPython, targetPython, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
		if (ec) {
			Log(L"Failed to install new python directory; restoring previous.");
			fs::remove_all(targetPython, ec);
			if (movedOld) {
				MoveFileExW(oldPython.c_str(), targetPython.c_str(), 0);
			}
			return false;
		}
	}

	if (movedOld) {
		fs::remove_all(oldPython, ec);
		if (fs::exists(oldPython)) {
			MoveFileExW(oldPython.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
		}
	}
	return true;
}

void RelaunchApp(const fs::path &exePath, const fs::path &workDir, const std::vector<std::wstring> &relaunchArgs) {
	auto cmd = QuoteArg(exePath.wstring());
	for (const auto &arg : relaunchArgs) {
		cmd.push_back(L' ');
		cmd.append(QuoteArg(arg));
	}
	auto cmdBuf = std::vector<wchar_t>(cmd.begin(), cmd.end());
	cmdBuf.push_back(L'\0');
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	if (CreateProcessW(
			exePath.c_str(),
			cmdBuf.data(),
			nullptr,
			nullptr,
			FALSE,
			0,
			nullptr,
			workDir.c_str(),
			&si,
			&pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		Log(L"CreateProcessW relaunch failed err=" + std::to_wstring(GetLastError()));
	}
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv || argc < 2) {
		return 1;
	}

	fs::path packagePath;
	fs::path metaPath;
	fs::path targetDir;
	std::wstring expectedSha256;
	std::wstring exeName = L"Ovygram.exe";
	DWORD parentPid = 0;
	ULONGLONG parentCreated = 0;
	bool elevated = false;
	std::vector<std::wstring> relaunchArgs;

	for (int i = 1; i < argc; ++i) {
		const std::wstring arg = argv[i];
		if (arg == L"--package" && i + 1 < argc) {
			packagePath = argv[++i];
		} else if (arg == L"--meta" && i + 1 < argc) {
			metaPath = argv[++i];
		} else if (arg == L"--sha256" && i + 1 < argc) {
			expectedSha256 = ToLower(argv[++i]);
		} else if (arg == L"--target" && i + 1 < argc) {
			targetDir = argv[++i];
		} else if (arg == L"--exe" && i + 1 < argc) {
			exeName = argv[++i];
		} else if (arg == L"--pid" && i + 1 < argc) {
			parentPid = std::stoul(argv[++i]);
		} else if (arg == L"--created" && i + 1 < argc) {
			parentCreated = std::stoull(argv[++i]);
		} else if (arg == L"--elevated") {
			elevated = true;
		} else if (arg == L"--relaunch-args") {
			while (i + 1 < argc) {
				relaunchArgs.emplace_back(argv[++i]);
			}
		}
	}

	const auto selfPath = fs::path(argv[0]);
	LocalFree(argv);

	const auto updatesDir = packagePath.parent_path();
	if (!updatesDir.empty()) {
		gLog.open(updatesDir / L"ovygram_updater.log", std::ios::out | std::ios::trunc);
	}
	Log(L"OvygramUpdater started.");

	if (packagePath.empty() || targetDir.empty() || !fs::exists(packagePath)) {
		Log(L"Missing required package or target arguments.");
		return 2;
	}

	WaitForParentProcess(parentPid, parentCreated);
	TerminateLingeringPythonHosts(targetDir);

	if (!IsDirectoryWritable(targetDir)) {
		if (elevated) {
			Log(L"Target directory still not writable after elevation.");
			RelaunchApp(targetDir / exeName, targetDir, relaunchArgs);
			return 3;
		}
		Log(L"Target directory requires elevation; re-launching with UAC.");
		auto params = std::wstring();
		const auto addParam = [&](const std::wstring &v) {
			if (!params.empty()) params.push_back(L' ');
			params.append(QuoteArg(v));
		};
		addParam(L"--elevated");
		addParam(L"--package"); addParam(packagePath.wstring());
		if (!metaPath.empty()) { addParam(L"--meta"); addParam(metaPath.wstring()); }
		if (!expectedSha256.empty()) { addParam(L"--sha256"); addParam(expectedSha256); }
		addParam(L"--target"); addParam(targetDir.wstring());
		addParam(L"--exe"); addParam(exeName);
		if (!relaunchArgs.empty()) {
			addParam(L"--relaunch-args");
			for (const auto &a : relaunchArgs) addParam(a);
		}
		SHELLEXECUTEINFOW sei = {};
		sei.cbSize = sizeof(sei);
		sei.fMask = SEE_MASK_NOCLOSEPROCESS;
		sei.lpVerb = L"runas";
		sei.lpFile = selfPath.c_str();
		sei.lpParameters = params.c_str();
		sei.lpDirectory = updatesDir.c_str();
		sei.nShow = SW_HIDE;
		if (ShellExecuteExW(&sei) && sei.hProcess) {
			WaitForSingleObject(sei.hProcess, INFINITE);
			CloseHandle(sei.hProcess);
			return 0;
		}
		Log(L"UAC elevation cancelled or failed; relaunching previous version.");
		RelaunchApp(targetDir / exeName, targetDir, relaunchArgs);
		return 4;
	}

	if (!expectedSha256.empty()) {
		std::wstring actualSha;
		if (!ComputeFileSha256Hex(packagePath, actualSha) || actualSha != expectedSha256) {
			Log(L"Package SHA-256 verification failed before extraction.");
			RelaunchApp(targetDir / exeName, targetDir, relaunchArgs);
			return 5;
		}
	}

	const auto stagingDir = updatesDir / L"staging";
	std::error_code ec;
	fs::remove_all(stagingDir, ec);
	fs::create_directories(stagingDir, ec);

	if (!ExtractZipSafe(packagePath, stagingDir)) {
		Log(L"ZIP extraction failed.");
		fs::remove_all(stagingDir, ec);
		RelaunchApp(targetDir / exeName, targetDir, relaunchArgs);
		return 6;
	}

	// Swap python/ atomically so deleted/renamed SDK files are cleaned up.
	const auto stagedPython = stagingDir / L"python";
	if (fs::exists(stagedPython)) {
		if (!SwapPythonDirectory(stagedPython, targetDir / L"python")) {
			Log(L"Failed to update python directory.");
			fs::remove_all(stagingDir, ec);
			RelaunchApp(targetDir / exeName, targetDir, relaunchArgs);
			return 7;
		}
	}

	// Replace all remaining files (Ovygram.exe, OvygramUpdater.exe, modules/, DLLs).
	for (const auto &entry : fs::recursive_directory_iterator(stagingDir)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto rel = fs::relative(entry.path(), stagingDir);
		const auto firstPart = ToLower(rel.begin()->wstring());
		if (firstPart == L"python") {
			continue;
		}
		auto dstRel = rel;
		if (ToLower(rel.wstring()) == L"ovygram.exe" && ToLower(exeName) != L"ovygram.exe") {
			dstRel = fs::path(exeName);
		}
		if (!ReplaceFileAtomic(entry.path(), targetDir / dstRel)) {
			Log(L"Failed replacing " + dstRel.wstring());
			fs::remove_all(stagingDir, ec);
			RelaunchApp(targetDir / exeName, targetDir, relaunchArgs);
			return 8;
		}
	}

	if (!metaPath.empty() && fs::exists(metaPath)) {
		ReplaceFileAtomic(metaPath, targetDir / L"ovygram_build.json");
	}

	fs::remove_all(stagingDir, ec);
	fs::remove(packagePath, ec);
	if (!metaPath.empty()) {
		fs::remove(metaPath, ec);
	}

	Log(L"Update installed successfully; relaunching Ovygram.");
	RelaunchApp(targetDir / exeName, targetDir, relaunchArgs);
	return 0;
}
