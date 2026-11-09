// NvChange-cli - change display resolution through NVIDIA NVAPI (command line).
//
// Build (MSVC):   cl /std:c++20 /EHsc /utf-8 /I include src\NvChange.cpp
// Build (MinGW):  g++ -std=c++20 -O2 -static -I include src\NvChange.cpp -o NvChange.exe
//
// The program loads nvapi64.dll dynamically, so no nvapi64.lib is required.

#include "NvChangeCore.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace nvchange;

namespace {

void PrintConfig(const DisplayConfig& config) {
    std::printf("共 %u 条活动显示路径：\n\n", static_cast<unsigned>(config.count));
    for (NvU32 i = 0; i < config.count; ++i) {
        const auto& path = config.paths[i];
        if (path.sourceModeInfo == nullptr) {
            continue;
        }
        const auto& source = path.sourceModeInfo[0];
        const double refresh = (path.targetInfoCount > 0 && path.targetInfo[0].details != nullptr)
                                   ? path.targetInfo[0].details->refreshRate1K / 1000.0
                                   : 0.0;

        std::printf("[%u] %u x %u", static_cast<unsigned>(i),
                    static_cast<unsigned>(source.resolution.width),
                    static_cast<unsigned>(source.resolution.height));
        if (refresh > 0.0) {
            std::printf(" @ %.3f Hz", refresh);
        }
        std::printf("，%u 位色", static_cast<unsigned>(source.resolution.colorDepth));
        if (source.bGDIPrimary) {
            std::printf("，主显示器");
        }
        std::printf("\n");

        for (NvU32 j = 0; j < path.targetInfoCount; ++j) {
            const auto& target = path.targetInfo[j];
            const auto* targetDetails = target.details;
            std::printf("    显示 ID 0x%08X", static_cast<unsigned>(target.displayId));
            if (targetDetails != nullptr) {
                std::printf("，缩放：%s，旋转：%s",
                            ScalingName(targetDetails->scaling).c_str(),
                            RotationName(targetDetails->rotation).c_str());
                if (targetDetails->refreshRate1K != 0) {
                    std::printf("，刷新率：%.3f Hz", targetDetails->refreshRate1K / 1000.0);
                }
                std::printf("\n    时序：%u x %u，像素时钟 %.2f MHz",
                            static_cast<unsigned>(targetDetails->timing.HVisible),
                            static_cast<unsigned>(targetDetails->timing.VVisible),
                            targetDetails->timing.pclk * 10.0 / 1000.0);
            }
            std::printf("\n");
        }
        std::printf("\n");
    }
}

struct Options {
    bool validateOnly = false;
    bool persist = false;
    bool noBackup = false;
    bool haveDisplay = false;
    std::size_t display = 0;
    bool haveRefresh = false;
    double refreshHz = 0.0;
};

void PrintUsage() {
    std::printf(
        "NvChange-cli - 通过 NVAPI 修改 NVIDIA 显示器分辨率\n"
        "\n"
        "用法：\n"
        "  NvChange-cli list                         列出活动显示路径和当前模式\n"
        "  NvChange-cli set <宽> <高> [刷新率] [选项]   设置分辨率\n"
        "  NvChange-cli set <宽>x<高>[@刷新率] [选项]   紧凑写法，例如 1920x1080@60\n"
        "  NvChange-cli restore                      恢复上一次 set 之前保存的模式\n"
        "\n"
        "选项：\n"
        "  -d, --display <序号>   选择显示路径，默认使用主显示器\n"
        "  -r, --refresh <Hz>     刷新率，例如 60、59.94\n"
        "      --validate         只验证模式是否被驱动接受，不真正应用\n"
        "      --persist          同时写入持久化配置（重启后仍生效）\n"
        "      --no-backup        不保存恢复点\n"
        "  -h, --help             显示本帮助\n"
        "\n"
        "示例：\n"
        "  NvChange-cli set 1920 1080 60\n"
        "  NvChange-cli set 1600x900@75 -d 0\n"
        "  NvChange-cli set 2560x1440 --validate\n"
        "  NvChange-cli restore\n");
}

bool ParseUnsigned(const std::string& text, NvU32& value) {
    try {
        std::size_t used = 0;
        unsigned long parsed = std::stoul(text, &used, 10);
        if (used != text.size() || parsed > 0xFFFFFFFFul) {
            return false;
        }
        value = static_cast<NvU32>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseDouble(const std::string& text, double& value) {
    try {
        std::size_t used = 0;
        double parsed = std::stod(text, &used);
        if (used != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseMode(const std::string& text, NvU32& width, NvU32& height, double& refreshHz,
               bool& haveRefresh) {
    std::string value = text;
    std::replace(value.begin(), value.end(), 'X', 'x');
    std::size_t x = value.find('x');
    if (x == std::string::npos) {
        return false;
    }
    std::string widthText = value.substr(0, x);
    std::string rest = value.substr(x + 1);
    std::size_t at = rest.find('@');
    std::string heightText = at == std::string::npos ? rest : rest.substr(0, at);
    if (!ParseUnsigned(widthText, width) || !ParseUnsigned(heightText, height)) {
        return false;
    }
    if (at != std::string::npos) {
        if (!ParseDouble(rest.substr(at + 1), refreshHz)) {
            return false;
        }
        haveRefresh = true;
    }
    return true;
}

int CommandList(NvApi& api) {
    std::string error;
    DisplayConfig config;
    if (!config.Load(api, error)) {
        std::printf("读取显示配置失败：%s\n", error.c_str());
        return 1;
    }

    std::printf("NvAPI 版本：%s\n", api.InterfaceVersion().c_str());
    std::string gpu = api.GpuName();
    if (!gpu.empty()) {
        std::printf("GPU：%s\n", gpu.c_str());
    }
    std::printf("\n");
    PrintConfig(config);
    return 0;
}

int CommandSet(NvApi& api, int argc, char** argv) {
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage();
            return 0;
        } else if (arg == "-d" || arg == "--display") {
            NvU32 displayIndex = 0;
            if (i + 1 >= argc || !ParseUnsigned(argv[++i], displayIndex)) {
                std::printf("错误：--display 需要一个非负整数\n");
                return 2;
            }
            options.display = displayIndex;
            options.haveDisplay = true;
        } else if (arg == "-r" || arg == "--refresh") {
            if (i + 1 >= argc || !ParseDouble(argv[++i], options.refreshHz)) {
                std::printf("错误：--refresh 需要一个有效数值\n");
                return 2;
            }
            options.haveRefresh = true;
        } else if (arg == "--validate") {
            options.validateOnly = true;
        } else if (arg == "--persist") {
            options.persist = true;
        } else if (arg == "--no-backup") {
            options.noBackup = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::printf("错误：未知选项 %s\n", arg.c_str());
            return 2;
        } else {
            positional.push_back(arg);
        }
    }

    NvU32 width = 0;
    NvU32 height = 0;
    double refreshHz = 0.0;
    bool haveRefresh = options.haveRefresh;
    if (options.haveRefresh) {
        refreshHz = options.refreshHz;
    }

    if (positional.empty()) {
        std::printf("错误：缺少分辨率，例如 NvChange-cli set 1920 1080\n");
        return 2;
    }

    if (positional[0].find('x') != std::string::npos ||
        positional[0].find('X') != std::string::npos) {
        if (!ParseMode(positional[0], width, height, refreshHz, haveRefresh)) {
            std::printf("错误：无法解析分辨率 %s\n", positional[0].c_str());
            return 2;
        }
        if (positional.size() > 2) {
            std::printf("错误：参数过多，紧凑写法最多再跟一个刷新率\n");
            return 2;
        }
        if (positional.size() > 1) {
            if (options.haveRefresh) {
                std::printf("错误：刷新率只能指定一次\n");
                return 2;
            }
            if (!ParseDouble(positional[1], refreshHz)) {
                std::printf("错误：无效的刷新率 %s\n", positional[1].c_str());
                return 2;
            }
            haveRefresh = true;
        }
    } else {
        if (positional.size() < 2 || !ParseUnsigned(positional[0], width) ||
            !ParseUnsigned(positional[1], height)) {
            std::printf("错误：需要宽度和高度，例如 NvChange-cli set 1920 1080\n");
            return 2;
        }
        if (positional.size() > 3) {
            std::printf("错误：参数过多，用法：NvChange-cli set <宽> <高> [刷新率]\n");
            return 2;
        }
        if (positional.size() > 2) {
            if (options.haveRefresh) {
                std::printf("错误：刷新率只能指定一次\n");
                return 2;
            }
            if (!ParseDouble(positional[2], refreshHz)) {
                std::printf("错误：无效的刷新率 %s\n", positional[2].c_str());
                return 2;
            }
            haveRefresh = true;
        }
    }

    if (width == 0 || height == 0) {
        std::printf("错误：宽度和高度必须大于 0\n");
        return 2;
    }
    if (haveRefresh && (!std::isfinite(refreshHz) || refreshHz <= 0.0)) {
        std::printf("错误：刷新率必须是大于 0 的有限数值\n");
        return 2;
    }

    std::string error;
    DisplayConfig config;
    if (!config.Load(api, error)) {
        std::printf("读取显示配置失败：%s\n", error.c_str());
        return 1;
    }

    const std::size_t pathIndex = options.haveDisplay ? options.display : config.PrimaryPath();
    if (pathIndex >= config.paths.size()) {
        std::printf("错误：显示路径 %zu 不存在，当前共 %zu 条\n", pathIndex, config.paths.size());
        return 2;
    }

    if (!options.noBackup && !options.validateOnly) {
        if (!SaveBackup(config, error)) {
            std::printf("警告：无法保存恢复点：%s\n", error.c_str());
        }
    }

    if (!config.SetMode(pathIndex, width, height, haveRefresh ? ToMilliHz(refreshHz) : 0)) {
        std::printf("错误：无法修改显示路径 %zu\n", pathIndex);
        return 1;
    }

    NvU32 flags = 0;
    if (options.validateOnly) {
        flags |= NV_DISPLAYCONFIG_VALIDATE_ONLY;
    }
    if (options.persist) {
        flags |= NV_DISPLAYCONFIG_SAVE_TO_PERSISTENCE;
    }

    std::string modeText = std::to_string(width) + " x " + std::to_string(height);
    if (haveRefresh) {
        std::ostringstream oss;
        oss << " @ " << refreshHz << " Hz";
        modeText += oss.str();
    }
    std::printf("正在%s %s（显示路径 %zu）...\n",
                options.validateOnly ? "验证" : "设置",
                modeText.c_str(), pathIndex);

    NvAPI_Status status = api.SetDisplayConfig(config.count, config.paths.data(), flags);
    if (status != NVAPI_OK) {
        std::printf("失败：%s\n", api.ErrorText(status).c_str());
        return 1;
    }

    if (options.validateOnly) {
        std::printf("验证通过：驱动接受该模式（未应用）。\n");
        return 0;
    }

    std::printf("成功。\n");
    Sleep(800);
    DisplayConfig updated;
    if (updated.Load(api, error)) {
        PrintConfig(updated);
    } else {
        std::printf("模式已设置，但重新读取配置失败：%s\n", error.c_str());
    }
    if (!options.noBackup) {
        std::printf("如需恢复，请运行：NvChange-cli restore\n");
    }
    return 0;
}

int CommandRestore(NvApi& api) {
    std::vector<SavedMode> saved;
    std::string error;
    if (!ReadBackup(saved, error)) {
        std::printf("恢复失败：%s\n", error.c_str());
        return 1;
    }

    DisplayConfig config;
    if (!config.Load(api, error)) {
        std::printf("读取当前显示配置失败：%s\n", error.c_str());
        return 1;
    }

    int applied = 0;
    for (const auto& mode : saved) {
        std::size_t pathIndex = config.paths.size();
        if (mode.displayId != 0) {
            for (std::size_t i = 0; i < config.paths.size(); ++i) {
                for (NvU32 j = 0; j < config.paths[i].targetInfoCount; ++j) {
                    if (config.paths[i].targetInfo[j].displayId == mode.displayId) {
                        pathIndex = i;
                        break;
                    }
                }
                if (pathIndex != config.paths.size()) {
                    break;
                }
            }
        }
        if (pathIndex == config.paths.size() && mode.pathIndex < config.paths.size()) {
            pathIndex = mode.pathIndex;
        }
        if (pathIndex == config.paths.size()) {
            std::printf("跳过显示 0x%08X：当前配置中找不到该显示器\n",
                        static_cast<unsigned>(mode.displayId));
            continue;
        }
        if (config.SetMode(pathIndex, mode.width, mode.height, mode.refreshMilliHz)) {
            ++applied;
        }
    }

    if (applied == 0) {
        std::printf("没有可恢复的显示模式。\n");
        return 1;
    }

    NvAPI_Status status = api.SetDisplayConfig(config.count, config.paths.data(), 0);
    if (status != NVAPI_OK) {
        std::printf("恢复失败：%s\n", api.ErrorText(status).c_str());
        return 1;
    }

    std::printf("已恢复 %d 条显示路径。\n", applied);
    Sleep(800);
    DisplayConfig updated;
    if (updated.Load(api, error)) {
        PrintConfig(updated);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        PrintUsage();
        return 0;
    }

    NvApi api;
    std::string error;
    if (!api.Load(error)) {
        std::printf("NVAPI 初始化失败：%s\n", error.c_str());
        return 1;
    }

    if (command == "list" || command == "ls") {
        return CommandList(api);
    }
    if (command == "set") {
        return CommandSet(api, argc, argv);
    }
    if (command == "restore") {
        return CommandRestore(api);
    }

    std::printf("未知命令：%s\n\n", command.c_str());
    PrintUsage();
    return 2;
}
