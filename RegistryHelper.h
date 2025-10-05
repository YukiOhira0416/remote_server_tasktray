#ifndef REGISTRY_HELPER_H
#define REGISTRY_HELPER_H

#include <string>
#include <vector>
#include <utility>

class RegistryHelper {
public:
    // Writes/reads the selected GPU's Vendor and Device ID.
    static bool WriteRegistry(const std::string& vendorID, const std::string& deviceID);
    static std::pair<std::string, std::string> ReadRegistry();

    // Writes a display serial number at a specific 1-based index (e.g., SerialNumber1, SerialNumber2).
    static bool WriteDISPInfoToRegistryAt(int index, const std::string& serial);

    // Reads all display serial numbers in port order by reading SerialNumber1, SerialNumber2, etc.
    static std::vector<std::string> ReadDISPInfoFromRegistry();

    // Writes the serial number of the user-selected display to persist the choice.
    static bool WriteSelectedSerialToRegistry(const std::string& serial);

    // Reads the serial number of the user-selected display.
    static std::string ReadSelectedSerialFromRegistry();

    // Deletes all values under the display info registry path.
    static bool ClearDISPInfoFromRegistry();

    static bool WriteCaptureTypeToRegistry(const std::string& captureType);

    static std::string ReadCaptureTypeFromRegistry();
};

#endif
