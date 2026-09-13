"""Repository search/navigation settings for https://github.com/djacobow/st.

Examples: st g Rearm -core; st f platform -host; st p blink.
Generated code, vendor sources and build artifacts require an explicit scope.
"""

# st applies exclusions to complete paths using regular expressions.
EXCLUDE = (
    r"/(?:build|\.git)/"
    r"|/tools/external/"
    r"|/platform/stm32h5/STM32CubeH5/"
    r"|/examples/stm32h563_blinky/(?:Core|Drivers|cmake)/"
    r"|/examples/stm32h563_blinky/(?:CMakeLists\.txt|CMakePresets\.json|"
    r"startup_stm32h563xx\.s|STM32H563xx_(?:FLASH|RAM)\.ld|blinky_demo\.ioc)$"
)

CONFIG = {
    "subpaths": {
        "rr": ("repository root", []),
        "core": ("core headers", ["include", "daveos", "core"]),
        "src": ("implementation", ["src"]),
        "host": ("host adapter", ["src", "platform", "host"]),
        "fake": ("fake adapter", ["src", "platform", "fake"]),
        "stm32": ("STM32H5 adapter", ["src", "platform", "stm32h5"]),
        "examples": ("examples", ["examples"]),
        "blink": ("STM32H563 blinker", ["examples", "stm32h563_blinky"]),
        "tests": ("tests", ["tests"]),
        "tools": ("tools", ["tools"]),
        "vendor": ("STM32CubeH5", ["platform", "stm32h5", "STM32CubeH5"]),
    },
    "code_file_types": {
        "cc": {"default": True},
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
            "core": {"default": False, "include": ["include/daveos/core", "src/core"]},
            "host": {
                "default": False,
                "include": ["include/daveos/platform/host", "include/daveos/platform/detail", "src/platform"],
                "exclude": r"/src/platform/(?!host(?:/|\.cc$))",
            },
            "fake": {
                "default": False,
                "include": ["include/daveos/platform/fake", "include/daveos/platform/detail", "src/platform"],
                "exclude": r"/src/platform/(?!fake(?:/|\.cc$))",
            },
            "stm32": {
                "default": False,
                "include": ["include/daveos/platform/stm32h5", "src/platform/stm32h5", "platform/stm32h5"],
                "exclude": EXCLUDE,
            },
            "examples": {"default": False, "include": ["examples"], "exclude": EXCLUDE},
            "tests": {"default": False, "include": ["tests"]},
            "tools": {"default": False, "include": ["tools"], "exclude": EXCLUDE},
            "generated": {"default": False, "include": ["examples/stm32h563_blinky"], "exclude": r"/(?:Drivers|cmake)/"},
            "vendor": {"default": False, "include": ["platform/stm32h5/STM32CubeH5/Drivers"]},
            "artifacts": {"default": False, "include": ["build"]},
        },
    },
}
