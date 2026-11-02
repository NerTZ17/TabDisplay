#include "adb.h"

#include <windows.h>

#include <filesystem>

namespace td {

std::wstring findAdb() {
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

bool runAdb(const std::wstring& adb, const std::wstring& serial, const std::wstring& args, std::string& output) {
    std::wstring cmd = L"\"" + adb + L"\"";
    if (!serial.empty()) {
        cmd += L" -s " + serial;
    }
    cmd += L" " + args;

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        output = "failed to start adb";
        return false;
    }
    char buf[512];
    DWORD got = 0;
    output.clear();
    while (ReadFile(readPipe, buf, sizeof buf, &got, nullptr) && got > 0) {
        output.append(buf, got);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

std::string narrowAscii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) {
        s.push_back(static_cast<char>(c));
    }
    return s;
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

}
