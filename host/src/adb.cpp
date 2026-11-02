#include "adb.h"

#include <windows.h>

#include <filesystem>

namespace td {

std::wstring findAdb() {
    wchar_t self[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::filesystem::path dir = std::filesystem::path(self).parent_path();
        for (const auto& candidate : {dir / L"adb.exe", dir / L"platform-tools" / L"adb.exe"}) {
            if (std::filesystem::exists(candidate)) {
                return candidate.wstring();
            }
        }
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        return found;
    }
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::filesystem::path p = std::filesystem::path(local) / L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe";
        if (std::filesystem::exists(p)) {
            return p.wstring();
        }
    }
    return {};
}

namespace {

bool runAdb(const std::wstring& adb, const std::wstring& serial, const std::wstring& args, std::string& output, DWORD timeoutMs = 10000) {
    std::wstring cmd = L"\"" + adb + L"\"";
    if (!serial.empty()) {
        cmd += L" -s " + serial;
    }
    cmd += L" " + args;

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 64 * 1024)) {
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        output = "failed to start adb";
        return false;
    }
    bool finished = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    if (!finished) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    output.clear();
    char buf[512];
    DWORD avail = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD got = 0;
        if (!ReadFile(readPipe, buf, sizeof buf, &got, nullptr) || got == 0) {
            break;
        }
        output.append(buf, got);
    }
    CloseHandle(readPipe);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (!finished) {
        output = "adb timed out";
        return false;
    }
    return code == 0;
}

std::string narrowAscii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) {
        s.push_back(static_cast<char>(c));
    }
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    size_t start = s.find_first_not_of(" \r\n");
    return start == std::string::npos ? std::string() : s.substr(start);
}

}

bool adbForward(const std::wstring& adb, const std::wstring& serial, uint16_t port, std::string& output) {
    std::string mapping = "tcp:" + std::to_string(port) + " tcp:" + std::to_string(port);
    std::string list;
    if (runAdb(adb, L"", L"forward --list", list)) {
        std::string want = serial.empty() ? mapping : narrowAscii(serial) + " " + mapping;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find('\n', start);
            std::string line = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (line.find(want) != std::string::npos) {
                output.clear();
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    return runAdb(adb, serial, L"forward " + std::wstring(mapping.begin(), mapping.end()), output);
}

std::string adbState(const std::wstring& adb, const std::wstring& serial) {
    std::string output;
    bool ok = runAdb(adb, serial, L"get-state", output, 5000);
    output = trim(output);
    if (ok) {
        return output;
    }
    if (output.find("more than one") != std::string::npos) {
        return "multiple";
    }
    if (output.find("offline") != std::string::npos) {
        return "offline";
    }
    if (output.find("unauthorized") != std::string::npos) {
        return "unauthorized";
    }
    if (output == "adb timed out") {
        return "timeout";
    }
    return "none";
}

bool adbLaunchApp(const std::wstring& adb, const std::wstring& serial) {
    std::string output;
    return runAdb(adb, serial, L"shell am start -n com.nertz.tabdisplay/.MainActivity", output, 8000);
}

}
