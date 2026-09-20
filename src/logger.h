#pragma once

#include <Windows.h>

#include <string>

namespace MELEVR::Logger
{
void LogStartup(HMODULE currentModule) noexcept;
void LogLine(const std::string& message) noexcept;
void LogWindowsError(const char* context, DWORD errorCode) noexcept;
std::wstring GetDataFolderPath() noexcept;
bool EnsureDataFolderExists() noexcept;
std::string WideToUtf8(const std::wstring& value);
}
