#pragma once

#include <cstdint>
#include <string>

namespace td {

std::wstring findAdb();
bool adbForward(const std::wstring& adb, const std::wstring& serial, uint16_t port, std::string& output);
std::string adbState(const std::wstring& adb, const std::wstring& serial);
bool adbLaunchApp(const std::wstring& adb, const std::wstring& serial);

}
