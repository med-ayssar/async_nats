from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout

# Cobalt shipped in Boost 1.84. Conan Center currently tops out at 1.91.
BOOST_VERSION = "1.90.0"

# Cobalt's compiled library depends on container + context. Everything else
# is disabled so Boost does not spend 20 minutes building unused modules.
BOOST_WITHOUT = (
    "atomic",
    "charconv",
    "chrono",
    "contract",
    "coroutine",
    "exception",
    "fiber",
    "filesystem",
    "graph",
    "graph_parallel",
    "iostreams",
    "json",
    "locale",
    "log",
    "math",
    "mpi",
    "nowide",
    "process",
    "program_options",
    "python",
    "random",
    "regex",
    "serialization",
    "stacktrace",
    "test",
    "thread",
    "timer",
    "type_erasure",
    "url",
    "wave",
)


class AsyncConan(ConanFile):
    name = "async"
    version = "0.1.0"
    package_type = "application"
    license = "MIT"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "src/*", "include/*", "cmake/*"

    default_options = {
        "boost/*:header_only": False,
        "boost/*:without_cobalt": False,
        "boost/*:without_container": False,
        "boost/*:without_context": False,
        "boost/*:zlib": False,
        "boost/*:bzip2": False,
        **{f"boost/*:without_{lib}": True for lib in BOOST_WITHOUT},
    }

    def validate(self):
        # Cobalt is C++20; the project defaults to C++23 via the profile.
        check_min_cppstd(self, 20)

    def requirements(self):
        self.requires(f"boost/{BOOST_VERSION}")
        self.requires("spdlog/1.17.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        cppstd = str(self.settings.get_safe("compiler.cppstd") or "23")
        if cppstd.startswith("gnu"):
            cppstd = cppstd[3:]
        if cppstd not in ("20", "23", "26"):
            cppstd = "23"
        tc.cache_variables["CMAKE_CXX_STANDARD"] = cppstd
        tc.cache_variables["CMAKE_CXX_STANDARD_REQUIRED"] = True
        tc.cache_variables["CMAKE_CXX_EXTENSIONS"] = False
        tc.cache_variables["CMAKE_EXPORT_COMPILE_COMMANDS"] = True
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
