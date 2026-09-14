"""Repository search/navigation settings for https://github.com/djacobow/st.

Examples: st g Rearm -core; st f platform -host; st p blink.
Generated code, vendor sources and build artifacts require an explicit scope.
"""

# st applies exclusions to complete paths using regular expressions.
EXCLUDE = (
    r"/(?:build|\.git)/"
    r"|/tools/external/"
    r"|/platform/stm32h[57]/STM32CubeH[57]/"
    r"|/platform/stm32/STM32_USB_Device_Library/"
    r"|/examples/stm32h755_console/(?:Drivers|Common|EWARM)/"
    r"|/examples/stm32h755_console/CM[47]/Core/"
    r"|/examples/stm32h755_console/.*\.(?:cmake|ioc|ld)$"
    r"|/examples/stm32h563_blinky/(?:Core|Drivers|cmake)/"
    r"|/examples/stm32h563_blinky/(?:CMakeLists\.txt|CMakePresets\.json|"
    r"startup_stm32h563xx\.s|STM32H563xx_(?:FLASH|RAM)\.ld|blinky_demo\.ioc)$"
)

CONFIG = {
    "subpaths": {
        "rr": ("repository root", []),
        "core": ("core components", ["core"]),
        "host": ("host adapter", ["platform", "host"]),
        "fake": ("fake adapter", ["platform", "fake"]),
        "stm32": ("STM32H5 adapter", ["platform", "stm32h5"]),
        "examples": ("examples", ["examples"]),
        "blink": ("STM32H563 console", ["examples", "stm32h563_blinky"]),
        "h755": ("STM32H755 console", ["examples", "stm32h755_console"]),
        "stm32h7": ("STM32H7 adapter", ["platform", "stm32h7"]),
        "tests": ("tests", ["tests"]),
        "tools": ("tools", ["tools"]),
        "vendor": ("STM32CubeH5", ["platform", "stm32h5", "STM32CubeH5"]),
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
            "core": {"default": False, "include": ["core"]},
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
            "generated": {"default": False, "include": ["examples/stm32h563_blinky"], "exclude": r"/(?:Drivers|cmake)/"},
            "vendor": {"default": False, "include": ["platform/stm32h5/STM32CubeH5/Drivers"]},
            "artifacts": {"default": False, "include": ["build"]},
        },
    },
}
