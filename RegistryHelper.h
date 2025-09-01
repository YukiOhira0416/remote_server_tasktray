#ifndef REGISTRY_HELPER_H
#define REGISTRY_HELPER_H

#include <string>
#include <functional>
#include <algorithm>

class RegistryHelper {
public:
    static bool WriteRegistry(const std::string& vendorID, const std::string& deviceID);
    static std::pair<std::string, std::string> ReadRegistry();
    static bool WriteDISPInfoToRegistry(const std::string& DispInfo);
    static std::vector<std::string> ReadDISPInfoFromRegistry();
    static bool WriteSelectedDisplayToRegistry(const std::string& serial);
    static std::string ReadSelectedDisplayFromRegistry();
};

#endif
