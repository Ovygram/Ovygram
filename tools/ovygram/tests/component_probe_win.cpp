#include <windows.h>

#include <cwchar>
#include <string_view>

int wmain(int argc, wchar_t *argv[]) {
	if (argc == 2 && std::wcscmp(argv[1], L"wait") == 0) {
		Sleep(60000);
		return 0;
	}
	if (argc == 3 && std::wcscmp(argv[1], L"check-private-file") == 0) {
		const auto file = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			CloseHandle(file);
			return 3;
		}
		return GetLastError() == ERROR_ACCESS_DENIED ? 0 : 4;
	}
	if (argc != 2 || std::wcscmp(argv[1], L"check-stdin") != 0) {
		return 5;
	}
	const auto input = GetStdHandle(STD_INPUT_HANDLE);
	char buffer[64];
	DWORD received = 0;
	if (!ReadFile(input, buffer, DWORD(sizeof(buffer)), &received, nullptr)) {
		return 6;
	}
	return std::string_view(buffer, received) == "needle\n" ? 0 : 7;
}
