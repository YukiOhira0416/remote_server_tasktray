// Utility.cpp
#include "Utility.h"
#include <windows.h>

std::string WideStringToMultiByte(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::string utf16_to_utf8(const std::wstring& utf16_str) {
    if (utf16_str.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), (int)utf16_str.size(), NULL, 0, NULL, NULL);
    if (size_needed <= 0) {
        return "";
    }
    std::string strTo(size_needed, 0);
    if (WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), (int)utf16_str.size(), &strTo[0], size_needed, NULL, NULL) <= 0) {
        return "";
    }
    return strTo;
}

std::wstring utf8_to_utf16(const std::string& utf8_str) {
    if (utf8_str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), (int)utf8_str.size(), NULL, 0);
    if (size_needed <= 0) {
        return L"";
    }
    std::wstring wstrTo(size_needed, 0);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), (int)utf8_str.size(), &wstrTo[0], size_needed) <= 0) {
        return L"";
    }
    return wstrTo;
}
