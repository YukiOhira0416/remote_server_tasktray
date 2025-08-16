#pragma once
#include <string>
#include <windows.h>

// 関数宣言のみ
std::string WideStringToMultiByte(const std::wstring& wstr);
std::string utf16_to_utf8(const std::wstring& utf16_str);
std::wstring utf8_to_utf16(const std::string& utf8_str);
