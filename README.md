# Remote Server TaskTray (English)

## Overview
Remote Server TaskTray is a Windows task tray application that manages GPU information and supports hardware encoding. It runs in the background and provides GPU status and encoding capability information via the system tray.

## Requirements
- Windows 10/11
- CMake 3.16 or later
- Visual Studio 2019 or later (with Desktop development with C++ workload) or MinGW-w64
- Windows SDK (DirectX SDK is included)

## How to Build with CMake

### 1. Create Build Directory
```powershell
mkdir build
cd build
```

### 2. Configure CMake
#### For Visual Studio:
```powershell
cmake -G "Visual Studio 17 2022" -A x64 ..
```
#### For MinGW:
```powershell
cmake -G "MinGW Makefiles" ..
```

### 3. Build
#### Debug build:
```powershell
cmake --build . --config Debug
```
#### Release build:
```powershell
cmake --build . --config Release
```

### 4. Executable Location
After building, the executable will be generated at:
- Debug: `build/Debug/remote_server_tasktray.exe`
- Release: `build/Release/remote_server_tasktray.exe`

## Project Structure
- **remote_server_tasktray.cpp**: Main entry point
- **TaskTrayApp.cpp / .h**: Manages the task tray application
- **GPUManager.cpp / .h**: Retrieves GPU information and checks hardware encoding support
- **RegistryHelper.cpp / .h**: Handles Windows registry operations
- **SharedMemoryHelper.cpp / .h**: Manages shared memory operations
- **DebugLog.cpp / .h**: Outputs debug logs
- **DisplayManager.cpp / .h**: Manages display information
- **Globals.cpp / .h**: Global variables and settings
- **StringConversion.cpp / .h**: String encoding conversion utilities
- **Utility.cpp / .h**: Utility functions

## Features
- Detects GPUs installed in the system
- Checks hardware encoding support (e.g., NVENC)
- Saves GPU information to the registry and shared memory
- Runs as a resident application in the Windows task tray
- Outputs debug logs for troubleshooting

## Dependencies
- DXGI (DirectX Graphics Infrastructure)
- Direct3D 11
- Windows API (User32, Kernel32, Advapi32, Shell32, Ole32, OleAut32, Gdi32, Winspool, Comdlg32, Uuid, Odbc32, Odbccp32, etc.)
- C++17 Standard Library
