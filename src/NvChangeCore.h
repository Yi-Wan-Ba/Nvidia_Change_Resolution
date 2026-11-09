// Shared NVAPI display-configuration core for the NvChange CLI and GUI.
//
// The official NVAPI header is designed for MSVC.  For MinGW we neutralise its
// SAL cleanup and make sure every standard header is parsed before nvapi.h.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _MSC_VER
#define __NVAPI_EMPTY_SAL
#endif
#include "nvapi/nvapi.h"

namespace nvchange {

inline constexpr NvU32 kNvApiInitialize = 0x0150e828;
inline constexpr NvU32 kNvApiUnload = 0xd22bdd7e;
inline constexpr NvU32 kNvApiGetErrorMessage = 0x6c2d048c;
inline constexpr NvU32 kNvApiGetInterfaceVersionString = 0x01053fa5;
inline constexpr NvU32 kNvApiEnumPhysicalGPUs = 0xe5ac921f;
inline constexpr NvU32 kNvApiGpuGetFullName = 0xceee8e9f;
inline constexpr NvU32 kNvApiDispGetDisplayConfig = 0x11abccf8;
inline constexpr NvU32 kNvApiDispSetDisplayConfig = 0x5d8cf8de;

using QueryInterfaceFn = void* (__cdecl*)(NvU32);
using InitializeFn = NvAPI_Status(__cdecl*)();
using UnloadFn = NvAPI_Status(__cdecl*)();
using GetErrorMessageFn = NvAPI_Status(__cdecl*)(NvAPI_Status, NvAPI_ShortString);
using GetInterfaceVersionStringFn = NvAPI_Status(__cdecl*)(NvAPI_ShortString);
using EnumPhysicalGPUsFn = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle[NVAPI_MAX_PHYSICAL_GPUS], NvU32*);
using GpuGetFullNameFn = NvAPI_Status(__cdecl*)(NvPhysicalGpuHandle, NvAPI_ShortString);
using GetDisplayConfigFn = NvAPI_Status(__cdecl*)(NvU32*, NV_DISPLAYCONFIG_PATH_INFO*);
using SetDisplayConfigFn = NvAPI_Status(__cdecl*)(NvU32, NV_DISPLAYCONFIG_PATH_INFO*, NvU32);

class NvApi {
public:
    NvApi() = default;
    NvApi(const NvApi&) = delete;
    NvApi& operator=(const NvApi&) = delete;

    ~NvApi() {
        if (initialized_ && unload_) {
            unload_();
        }
        if (module_) {
            FreeLibrary(module_);
        }
    }

    bool Load(std::string& error) {
        wchar_t systemDir[MAX_PATH]{};
        UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            error = "GetSystemDirectoryW failed";
            return false;
        }

        std::wstring dllPath(systemDir);
        dllPath += L"\\nvapi64.dll";
        module_ = LoadLibraryW(dllPath.c_str());
        if (!module_) {
            std::ostringstream oss;
            oss << "cannot load nvapi64.dll (error " << GetLastError() << ')';
            error = oss.str();
            return false;
        }

        FARPROC proc = GetProcAddress(module_, "nvapi_QueryInterface");
        if (!proc) {
            error = "nvapi64.dll has no nvapi_QueryInterface export";
            return false;
        }
        QueryInterfaceFn query = nullptr;
        static_assert(sizeof(query) == sizeof(proc), "unexpected function pointer size");
        std::memcpy(&query, &proc, sizeof(query));
        query_ = query;

        initialize_ = Get<InitializeFn>(kNvApiInitialize);
        unload_ = Get<UnloadFn>(kNvApiUnload);
        getErrorMessage_ = Get<GetErrorMessageFn>(kNvApiGetErrorMessage);
        getInterfaceVersion_ = Get<GetInterfaceVersionStringFn>(kNvApiGetInterfaceVersionString);
        enumPhysicalGPUs_ = Get<EnumPhysicalGPUsFn>(kNvApiEnumPhysicalGPUs);
        gpuGetFullName_ = Get<GpuGetFullNameFn>(kNvApiGpuGetFullName);
        getDisplayConfig_ = Get<GetDisplayConfigFn>(kNvApiDispGetDisplayConfig);
        setDisplayConfig_ = Get<SetDisplayConfigFn>(kNvApiDispSetDisplayConfig);

        if (!initialize_ || !unload_ || !getErrorMessage_ || !getDisplayConfig_ || !setDisplayConfig_) {
            error = "nvapi64.dll is missing one or more required NVAPI functions";
            return false;
        }

        NvAPI_Status status = initialize_();
        if (status != NVAPI_OK) {
            error = ErrorText(status);
            return false;
        }
        initialized_ = true;
        return true;
    }

    std::string ErrorText(NvAPI_Status status) const {
        NvAPI_ShortString text{};
        if (getErrorMessage_) {
            getErrorMessage_(status, text);
        }
        std::ostringstream oss;
        oss << "NVAPI error " << static_cast<int>(status);
        if (text[0] != '\0') {
            oss << " (" << text << ')';
        }
        return oss.str();
    }

    std::string InterfaceVersion() const {
        if (!getInterfaceVersion_) {
            return {};
        }
        NvAPI_ShortString text{};
        if (getInterfaceVersion_(text) != NVAPI_OK) {
            return {};
        }
        return text;
    }

    std::string GpuName() const {
        if (!enumPhysicalGPUs_ || !gpuGetFullName_) {
            return {};
        }
        NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS]{};
        NvU32 count = 0;
        if (enumPhysicalGPUs_(handles, &count) != NVAPI_OK || count == 0) {
            return {};
        }
        NvAPI_ShortString name{};
        if (gpuGetFullName_(handles[0], name) != NVAPI_OK) {
            return {};
        }
        return name;
    }

    NvAPI_Status GetDisplayConfig(NvU32* count, NV_DISPLAYCONFIG_PATH_INFO* paths) const {
        return getDisplayConfig_(count, paths);
    }

    NvAPI_Status SetDisplayConfig(NvU32 count, NV_DISPLAYCONFIG_PATH_INFO* paths, NvU32 flags) const {
        return setDisplayConfig_(count, paths, flags);
    }

private:
    template <typename T>
    T Get(NvU32 id) const {
        return reinterpret_cast<T>(query_(id));
    }

    HMODULE module_ = nullptr;
    QueryInterfaceFn query_ = nullptr;
    InitializeFn initialize_ = nullptr;
    UnloadFn unload_ = nullptr;
    GetErrorMessageFn getErrorMessage_ = nullptr;
    GetInterfaceVersionStringFn getInterfaceVersion_ = nullptr;
    EnumPhysicalGPUsFn enumPhysicalGPUs_ = nullptr;
    GpuGetFullNameFn gpuGetFullName_ = nullptr;
    GetDisplayConfigFn getDisplayConfig_ = nullptr;
    SetDisplayConfigFn setDisplayConfig_ = nullptr;
    bool initialized_ = false;
};

struct DisplayConfig {
    NvU32 count = 0;
    std::vector<NV_DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<std::vector<NV_DISPLAYCONFIG_SOURCE_MODE_INFO>> sources;
    std::vector<std::vector<NV_DISPLAYCONFIG_PATH_TARGET_INFO>> targets;
    std::vector<std::vector<NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO>> details;

    DisplayConfig() = default;
    DisplayConfig(const DisplayConfig&) = delete;
    DisplayConfig& operator=(const DisplayConfig&) = delete;

    void Clear() {
        details.clear();
        targets.clear();
        sources.clear();
        paths.clear();
        count = 0;
    }

    bool Load(const NvApi& api, std::string& error) {
        Clear();

        NvU32 pathCount = 0;
        NvAPI_Status status = api.GetDisplayConfig(&pathCount, nullptr);
        if (status != NVAPI_OK) {
            error = api.ErrorText(status);
            return false;
        }
        if (pathCount == 0) {
            error = "NVAPI returned zero display paths";
            return false;
        }

        paths.resize(pathCount);
        for (auto& path : paths) {
            std::memset(&path, 0, sizeof(path));
            path.version = NV_DISPLAYCONFIG_PATH_INFO_VER;
        }

        status = api.GetDisplayConfig(&pathCount, paths.data());
        if (status != NVAPI_OK) {
            error = api.ErrorText(status);
            return false;
        }
        if (pathCount != paths.size()) {
            error = "display path count changed while reading the configuration";
            return false;
        }

        sources.resize(pathCount);
        targets.resize(pathCount);
        details.resize(pathCount);

        for (NvU32 i = 0; i < pathCount; ++i) {
            sources[i].resize(1);
            std::memset(sources[i].data(), 0, sizeof(NV_DISPLAYCONFIG_SOURCE_MODE_INFO));
            paths[i].sourceModeInfo = sources[i].data();

            targets[i].resize(paths[i].targetInfoCount);
            if (!targets[i].empty()) {
                std::memset(targets[i].data(), 0,
                            targets[i].size() * sizeof(NV_DISPLAYCONFIG_PATH_TARGET_INFO));
            }
            details[i].resize(paths[i].targetInfoCount);
            for (NvU32 j = 0; j < paths[i].targetInfoCount; ++j) {
                std::memset(&details[i][j], 0, sizeof(details[i][j]));
                details[i][j].version = NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO_VER;
                targets[i][j].details = &details[i][j];
            }
            paths[i].targetInfo = targets[i].data();
        }

        status = api.GetDisplayConfig(&pathCount, paths.data());
        if (status != NVAPI_OK) {
            error = api.ErrorText(status);
            return false;
        }
        count = pathCount;
        if (pathCount != paths.size()) {
            error = "display path count changed while reading the configuration";
            return false;
        }
        return true;
    }

    bool SetMode(std::size_t pathIndex, NvU32 width, NvU32 height, NvU32 refreshMilliHz) {
        if (pathIndex >= paths.size() || paths[pathIndex].sourceModeInfo == nullptr) {
            return false;
        }
        auto& source = paths[pathIndex].sourceModeInfo[0];
        source.resolution.width = width;
        source.resolution.height = height;
        source.resolution.colorDepth = 32;
        if (refreshMilliHz != 0) {
            for (NvU32 j = 0; j < paths[pathIndex].targetInfoCount; ++j) {
                auto* targetDetails = paths[pathIndex].targetInfo[j].details;
                if (targetDetails != nullptr) {
                    targetDetails->refreshRate1K = refreshMilliHz;
                }
            }
        }
        return true;
    }

    std::size_t PrimaryPath() const {
        for (std::size_t i = 0; i < paths.size(); ++i) {
            if (paths[i].sourceModeInfo != nullptr && paths[i].sourceModeInfo[0].bGDIPrimary) {
                return i;
            }
        }
        return 0;
    }
};

struct SavedMode {
    NvU32 pathIndex = 0;
    NvU32 displayId = 0;
    NvU32 sourceId = 0;
    NvU32 width = 0;
    NvU32 height = 0;
    NvU32 refreshMilliHz = 0;
};

namespace fs = std::filesystem;

inline std::string AppFilePathString(const char* fileName) {
    wchar_t modulePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    fs::path directory;
    if (length > 0 && length < MAX_PATH) {
        directory = fs::path(modulePath).parent_path();
    } else {
        directory = fs::current_path();
    }
    return (directory / fileName).string();
}

inline std::string BackupPathString() {
    return AppFilePathString("NvChange-last-mode.ini");
}

inline fs::path BackupPath() {
    return fs::path(BackupPathString());
}

inline bool SaveBackup(const DisplayConfig& config, std::string& error) {
    const fs::path file = BackupPath();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "cannot create backup directory: " + ec.message();
        return false;
    }

    std::ofstream out(file);
    if (!out) {
        error = "cannot write backup file: " + file.string();
        return false;
    }
    out << "NvChangeBackup 1\n";
    for (NvU32 i = 0; i < config.count; ++i) {
        const auto& path = config.paths[i];
        if (path.sourceModeInfo == nullptr) {
            continue;
        }
        const NvU32 displayId = path.targetInfoCount > 0 ? path.targetInfo[0].displayId : 0;
        const NvU32 refresh = (path.targetInfoCount > 0 && path.targetInfo[0].details != nullptr)
                                  ? path.targetInfo[0].details->refreshRate1K
                                  : 0;
        out << "path " << i << "\n";
        out << "display " << displayId << "\n";
        out << "source " << path.sourceId << "\n";
        out << "width " << path.sourceModeInfo[0].resolution.width << "\n";
        out << "height " << path.sourceModeInfo[0].resolution.height << "\n";
        out << "refresh " << refresh << "\n";
    }
    return true;
}

inline bool ReadBackup(std::vector<SavedMode>& modes, std::string& error) {
    std::ifstream in(BackupPath());
    if (!in) {
        error = "no backup found at " + BackupPathString();
        return false;
    }

    std::string line;
    if (!std::getline(in, line) || line != "NvChangeBackup 1") {
        error = "backup file has an unsupported format";
        return false;
    }

    modes.clear();
    SavedMode current;
    bool haveCurrent = false;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key)) {
            continue;
        }
        if (key == "path") {
            if (haveCurrent) {
                modes.push_back(current);
            }
            current = SavedMode{};
            iss >> current.pathIndex;
            haveCurrent = true;
        } else if (key == "display") {
            iss >> current.displayId;
        } else if (key == "source") {
            iss >> current.sourceId;
        } else if (key == "width") {
            iss >> current.width;
        } else if (key == "height") {
            iss >> current.height;
        } else if (key == "refresh") {
            iss >> current.refreshMilliHz;
        }
    }
    if (haveCurrent) {
        modes.push_back(current);
    }
    if (modes.empty()) {
        error = "backup file contains no display modes";
        return false;
    }
    return true;
}

struct CustomMode {
    NvU32 width = 0;
    NvU32 height = 0;
};

inline bool IsValidCustomMode(const CustomMode& mode) {
    return mode.width >= 320 && mode.width <= 16384 && mode.height >= 200 &&
           mode.height <= 16384;
}

inline std::string CustomModesPathString() {
    return AppFilePathString("NvChange-custom-modes.ini");
}

inline bool LoadCustomModes(std::vector<CustomMode>& modes, std::string& error) {
    modes.clear();
    std::ifstream in(CustomModesPathString());
    if (!in) {
        // A missing file simply means the user has not defined any modes yet.
        return true;
    }

    std::string line;
    if (!std::getline(in, line) || line != "NvChangeCustomModes 1") {
        error = "custom modes file has an unsupported format";
        return false;
    }

    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key)) {
            continue;
        }
        if (key != "mode") {
            continue;
        }
        CustomMode mode;
        if (!(iss >> mode.width >> mode.height)) {
            error = "custom modes file contains an invalid mode line";
            return false;
        }
        if (!IsValidCustomMode(mode)) {
            error = "custom modes file contains an out-of-range resolution";
            return false;
        }
        for (const CustomMode& existing : modes) {
            if (existing.width == mode.width && existing.height == mode.height) {
                error = "custom modes file contains a duplicate resolution";
                return false;
            }
        }
        if (modes.size() >= 3) {
            error = "custom modes file contains more than three resolutions";
            return false;
        }
        modes.push_back(mode);
    }
    return true;
}

inline bool SaveCustomModes(const std::vector<CustomMode>& modes, std::string& error) {
    if (modes.size() > 3) {
        error = "at most three custom resolutions can be saved";
        return false;
    }
    for (std::size_t i = 0; i < modes.size(); ++i) {
        if (!IsValidCustomMode(modes[i])) {
            error = "custom resolution is outside the supported range";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (modes[i].width == modes[j].width && modes[i].height == modes[j].height) {
                error = "custom resolutions must be unique";
                return false;
            }
        }
    }

    const fs::path file = CustomModesPathString();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "cannot create custom modes directory: " + ec.message();
        return false;
    }

    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        error = "cannot write custom modes file: " + file.string();
        return false;
    }
    out << "NvChangeCustomModes 1\n";
    for (const auto& mode : modes) {
        out << "mode " << mode.width << " " << mode.height << "\n";
    }
    return static_cast<bool>(out);
}

inline std::string CustomRefreshPathString() {
    return AppFilePathString("NvChange-custom-refresh.ini");
}

inline bool LoadCustomRefresh(NvU32& hz, std::string& error) {
    hz = 0;
    std::ifstream in(CustomRefreshPathString());
    if (!in) {
        return true;
    }

    std::string line;
    if (!std::getline(in, line) || line != "NvChangeCustomRefresh 1") {
        error = "custom refresh file has an unsupported format";
        return false;
    }
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key)) {
            continue;
        }
        if (key != "hz") {
            continue;
        }
        NvU32 value = 0;
        if (!(iss >> value) || value < 1 || value > 1000) {
            error = "custom refresh file contains an invalid value";
            return false;
        }
        hz = value;
    }
    return true;
}

inline bool SaveCustomRefresh(NvU32 hz, std::string& error) {
    if (hz < 1 || hz > 1000) {
        error = "custom refresh must be between 1 and 1000 Hz";
        return false;
    }

    const fs::path file = CustomRefreshPathString();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "cannot create custom refresh directory: " + ec.message();
        return false;
    }

    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        error = "cannot write custom refresh file: " + file.string();
        return false;
    }
    out << "NvChangeCustomRefresh 1\n";
    out << "hz " << hz << "\n";
    return static_cast<bool>(out);
}

inline std::string ScalingName(NvU32 scaling) {
    switch (scaling) {
    case NV_SCALING_DEFAULT:
        return "默认";
    case NV_SCALING_GPU_SCALING_TO_CLOSEST:
        return "GPU 缩放（全屏）";
    case NV_SCALING_GPU_SCALING_TO_NATIVE:
        return "GPU 缩放（原生）";
    case NV_SCALING_GPU_SCANOUT_TO_NATIVE:
        return "居中（不缩放）";
    case NV_SCALING_GPU_SCALING_TO_ASPECT_SCANOUT_TO_NATIVE:
        return "GPU 缩放（保持宽高比）";
    case NV_SCALING_GPU_SCALING_TO_ASPECT_SCANOUT_TO_CLOSEST:
        return "GPU 缩放（宽高比，平衡）";
    case NV_SCALING_GPU_SCANOUT_TO_CLOSEST:
        return "居中（平衡）";
    case NV_SCALING_GPU_INTEGER_ASPECT_SCALING:
        return "整数缩放";
    case NV_SCALING_CUSTOMIZED:
        return "自定义";
    default:
        return "未知";
    }
}

inline std::string RotationName(NvU32 rotation) {
    switch (rotation) {
    case NV_ROTATE_0:
        return "0°";
    case NV_ROTATE_90:
        return "90°";
    case NV_ROTATE_180:
        return "180°";
    case NV_ROTATE_270:
        return "270°";
    default:
        return "未知";
    }
}

inline NvU32 ToMilliHz(double hz) {
    return static_cast<NvU32>(std::lround(hz * 1000.0));
}

}  // namespace nvchange
