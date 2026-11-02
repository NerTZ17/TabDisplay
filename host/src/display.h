#pragma once

#include <string>

namespace td {

enum class MonitorState { Active, Inactive, Missing };

MonitorState virtualMonitorState(const std::wstring& match);
bool setVirtualMonitorActive(const std::wstring& match, bool active, std::wstring& error);

}
