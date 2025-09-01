#ifndef STRING_CONVERSION_H
#define STRING_CONVERSION_H

#include <string>

std::string ConvertWStringToString(const std::wstring& wstr);
std::wstring ConvertStringToWString(const std::string& str);

#endif // STRING_CONVERSION_H
