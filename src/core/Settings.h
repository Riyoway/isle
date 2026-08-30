#pragma once

#include <filesystem>

namespace isle {

struct Settings {
    int topOffsetDip{8};
    bool startWithWindows{false};
    bool hideInFullscreen{true};
    bool expandOnHover{false};
    bool showSeconds{false};
    bool monitorAtCursor{false};

    static std::filesystem::path data_directory();
    static std::filesystem::path file_path();
    static Settings load();
    void save() const;
};

} // namespace isle
