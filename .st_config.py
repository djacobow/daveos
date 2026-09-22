"""Repository search/navigation settings for https://github.com/djacobow/st.

Examples: st g Rearm -core; st f platform -host; st p blink.
Generated code, vendor sources and build artifacts require an explicit scope.
"""

# st applies exclusions to complete paths using regular expressions.
EXCLUDE = (
    r"/(?:build|\.git)/"
    r"|/tools/external/"
    r"|/third_party/"
    r"|/platform/stm32/nucleo/h755/(?:Drivers|Common|EWARM)/"
    r"|/platform/stm32/nucleo/h755/CM[47]/Core/"
    r"|/platform/stm32/nucleo/h755/.*\.(?:cmake|ioc|ld)$"
    r"|/platform/stm32/nucleo/h563/(?:Core|Drivers|cmake)/"
    r"|/platform/stm32/nucleo/h563/(?:CMakeLists\.txt|CMakePresets\.json|"
    r"startup_stm32h563xx\.s|STM32H563xx_(?:FLASH|RAM)\.ld|blinky_demo\.ioc)$"
)

CONFIG = {
    "subpaths": {
        "rr": ("repository root", []),
        "lib": ("libraries", ["lib"]),
        "apps": ("applications", ["apps"]),
        "core": ("core components", ["lib", "core"]),
        "host": ("host adapter", ["platform", "host"]),
        "fake": ("fake adapter", ["platform", "fake"]),
        "stm32": ("STM32H5 adapter", ["platform", "stm32h5"]),
        "examples": ("examples", ["examples"]),
        "blink": ("STM32H563 console", ["platform", "stm32", "nucleo", "h563"]),
        "h755": ("STM32H755 console", ["platform", "stm32", "nucleo", "h755"]),
        "stm32h7": ("STM32H7 adapter", ["platform", "stm32h7"]),
        "tests": ("tests", ["tests"]),
        "tools": ("tools", ["tools"]),
        "vendor": ("STM32CubeH5", ["third_party", "STM32CubeH5"]),
    },
    "code_file_types": {
        "cpp": {"default": True},
        "hpp": {"default": True},
        "h": {"default": True},
        "py": {"default": True, "grep_extra_glob": [".st_config.py"]},
        "md": {"default": True},
        "build": {"default": True},
        "options": {"default": True},
        "ini": {"default": True},
        "s": {"default": False},
        "ld": {"default": False},
        "ioc": {"default": False},
        "map": {"default": False},
    },
    "code_search_paths": {
        "daveos": {
            "repo": {"default": True, "include": ["."], "exclude": EXCLUDE},
            "lib": {"default": False, "include": ["lib"]},
            "apps": {"default": False, "include": ["apps"]},
            "core": {"default": False, "include": ["lib/core"]},
            "host": {
                "default": False,
                "include": ["platform/host", "platform/detail"],
            },
            "fake": {
                "default": False,
                "include": ["platform/fake", "platform/detail"],
            },
            "stm32": {
                "default": False,
                "include": ["platform/stm32h5", "platform/detail"],
                "exclude": EXCLUDE,
            },
            "stm32h7": {"default": False, "include": ["platform/stm32h7", "platform/detail"], "exclude": EXCLUDE},
            "examples": {"default": False, "include": ["examples"], "exclude": EXCLUDE},
            "tests": {"default": False, "include": ["tests"]},
            "tools": {"default": False, "include": ["tools"], "exclude": EXCLUDE},
            "generated": {"default": False, "include": ["platform/stm32/nucleo/h563"], "exclude": r"/(?:Drivers|cmake)/"},
            "vendor": {"default": False, "include": ["third_party/STM32CubeH5/Drivers"]},
            "artifacts": {"default": False, "include": ["build"]},
        },
    },
}
