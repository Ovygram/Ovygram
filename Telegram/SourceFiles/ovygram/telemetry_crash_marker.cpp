#include "ovygram/telemetry_crash_marker.h"

#include <QtCore/QFile>

#include <array>
#include <csignal>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#else // Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif // Q_OS_WIN

namespace Ovygram::Telemetry::CrashMarker {
namespace {

// The handlers only write a one-byte marker, then defer to the normal crash path.
#ifdef Q_OS_WIN
HANDLE Marker = INVALID_HANDLE_VALUE;
LPTOP_LEVEL_EXCEPTION_FILTER PreviousExceptionFilter = nullptr;
using SignalHandler = void (*)(int);
SignalHandler PreviousAbortHandler = SIG_DFL;

void Mark() {
	constexpr auto byte = '1';
	DWORD written = 0;
	WriteFile(Marker, &byte, 1, &written, nullptr);
}

LONG WINAPI HandleException(EXCEPTION_POINTERS *details) {
	Mark();
	return PreviousExceptionFilter
		? PreviousExceptionFilter(details)
		: EXCEPTION_CONTINUE_SEARCH;
}

void HandleAbort(int signal) {
	Mark();
	if (PreviousAbortHandler != SIG_ERR) {
		std::signal(signal, PreviousAbortHandler);
		std::raise(signal);
	}
}
#else // Q_OS_WIN
constexpr auto Signals = std::array{ SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV };
std::array<struct sigaction, Signals.size()> PreviousActions;
std::array<bool, Signals.size()> Active = {};
volatile std::sig_atomic_t Marker = -1;

void HandleSignal(int signal) {
	const auto fd = Marker;
	if (fd >= 0) {
		constexpr auto byte = '1';
		const auto written = ::write(fd, &byte, 1);
		(void)written;
	}
	for (auto i = size_t(0); i != Signals.size(); ++i) {
		if (Signals[i] == signal) {
			::sigaction(signal, &PreviousActions[i], nullptr);
			::raise(signal);
			break;
		}
	}
}
#endif // Q_OS_WIN

} // namespace

void Arm(const QString &path) {
#ifdef Q_OS_WIN
	Marker = CreateFileW(
		reinterpret_cast<LPCWSTR>(path.utf16()),
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_DELETE,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr);
	if (Marker != INVALID_HANDLE_VALUE) {
		PreviousExceptionFilter = SetUnhandledExceptionFilter(HandleException);
		PreviousAbortHandler = std::signal(SIGABRT, HandleAbort);
	}
#else // Q_OS_WIN
	Marker = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (Marker >= 0) {
		struct sigaction action = {};
		action.sa_handler = HandleSignal;
		::sigemptyset(&action.sa_mask);
		for (auto i = size_t(0); i != Signals.size(); ++i) {
			Active[i] = (::sigaction(Signals[i], &action, &PreviousActions[i]) == 0);
		}
	}
#endif // Q_OS_WIN
}

void Disarm() {
#ifdef Q_OS_WIN
	if (Marker != INVALID_HANDLE_VALUE) {
		SetUnhandledExceptionFilter(PreviousExceptionFilter);
		if (PreviousAbortHandler != SIG_ERR) {
			std::signal(SIGABRT, PreviousAbortHandler);
		}
		CloseHandle(Marker);
		Marker = INVALID_HANDLE_VALUE;
	}
#else // Q_OS_WIN
	const auto fd = Marker;
	if (fd >= 0) {
		Marker = -1;
		for (auto i = size_t(0); i != Signals.size(); ++i) {
			if (Active[i]) {
				::sigaction(Signals[i], &PreviousActions[i], nullptr);
				Active[i] = false;
			}
		}
		::close(fd);
	}
#endif // Q_OS_WIN
}

} // namespace Ovygram::Telemetry::CrashMarker
