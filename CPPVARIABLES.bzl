"Global C++ compilation flags for SDS."

SDS_BASE_CPPOPTS = select({
    "//:build_macos": [
        "-O3",
        "-flto=thin",
        "-mtune=generic",
        "-fPIC",
        "-Wpedantic",
        "-Wall",
        "-Wno-character-conversion",
        "-Werror",
    ],
    "//:debug_build_macos": [
        "-g",
        "-mtune=generic",
        "-fPIC",
        "-Wpedantic",
        "-Wall",
        "-Wno-character-conversion",
        "-Werror",
    ],
    "//:build_linux": [
        "-O3",
        "-fPIC",
        "-Wpedantic",
        "-Wall",
        "-Wno-character-conversion",
        "-Werror",
    ],
    "//:debug_build_linux": [
        "-g",
        "-O2",
        "-fPIC",
        "-Wpedantic",
        "-Wall",
        "-Wno-character-conversion",
        "-Werror",
    ],
    "//:build_windows": [
        "/W4",
        "/WX",
        "/permissive-",
    ],
    "//:debug_build_windows": [
        "/Zi",
        "/W4",
        "/WX",
        "/permissive-",
    ],
    "//conditions:default": [],
})

SDS_LINKOPTS = select({
    "//:build_macos": ["-flto=thin"],
    "//:debug_build_macos": [],
    "//:build_linux": ["-pthread"],
    "//:debug_build_linux": ["-pthread"],
    "//conditions:default": [],
})
