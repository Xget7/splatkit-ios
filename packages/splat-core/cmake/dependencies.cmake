include(FetchContent)
# Archive timestamps from extraction time (CMake 3.24+); older CMake, like the Android SDK one, ignores this.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# nianticlabs/spz: reference SPZ reader and writer (MIT). Pinned to a commit, not a branch.
set(SPZ_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(SPZ_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  spz
  GIT_REPOSITORY https://github.com/nianticlabs/spz.git
  GIT_TAG affd0ecea7fbb4c265ee119475af7ee5b2997482
  GIT_SHALLOW OFF
)
FetchContent_MakeAvailable(spz)

# nlohmann/json for the glTF JSON chunk (MIT). Header only.
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

if(SPLAT_CORE_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
