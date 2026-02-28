cmake_minimum_required(VERSION 3.5)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set extension name here
set(TARGET_NAME spatial)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(CMAKE_CXX_STANDARD 11 CACHE STRING "C++ standard")

if(EMSCRIPTEN)
  # _LINKED_LIBS influences only Wasm compilation it's unclear why this is
  # needed, but somehow the global symbol pj_release from PROJ is not properly
  # exported otherwise this solves by basically re-linking (at the moment the
  # Wasm binary is actually produced)
  set(DUCKDB_EXTENSION_SPATIAL_LINKED_LIBS
      "../../vcpkg_installed/wasm32-emscripten/lib/lib*.a")
endif()

project(${TARGET_NAME})

add_definitions(-DDUCKDB_MAJOR_VERSION=${DUCKDB_MAJOR_VERSION})
add_definitions(-DDUCKDB_MINOR_VERSION=${DUCKDB_MINOR_VERSION})
add_definitions(-DDUCKDB_PATCH_VERSION=${DUCKDB_PATCH_VERSION})
add_definitions(-DDUCKDB_SPATIAL_EXTENSION=1)

# Options

# Enable network functionality (OpenSSL and GDAL's CURL based fs/drivers)
option(SPATIAL_USE_NETWORK "Enable network functionality" ON)

# Enable GEOS support
option(SPATIAL_USE_GEOS "Enable GEOS support" ON)

# Enable PROJ support (coordinate transformations)
option(SPATIAL_USE_PROJ "Enable PROJ support" ON)

# Enable GDAL support (vector/raster I/O)
option(SPATIAL_USE_GDAL "Enable GDAL support" ON)

# Enable Shapefile support (requires third_party/shapelib)
option(SPATIAL_USE_SHAPEFILE "Enable Shapefile support" ON)

# Enable OSM support (requires zlib)
option(SPATIAL_USE_OSM "Enable OSM support" ON)

# Enable MVT support (Mapbox Vector Tiles)
option(SPATIAL_USE_MVT "Enable MVT support" ON)

if(SPATIAL_USE_PROJ)
  add_definitions(-DSPATIAL_USE_PROJ=1)
endif()

if(SPATIAL_USE_GDAL)
  add_definitions(-DSPATIAL_USE_GDAL=1)
endif()

if(SPATIAL_USE_SHAPEFILE)
  add_definitions(-DSPATIAL_USE_SHAPEFILE=1)
endif()

if(SPATIAL_USE_OSM)
  add_definitions(-DSPATIAL_USE_OSM=1)
endif()

if(SPATIAL_USE_MVT)
  add_definitions(-DSPATIAL_USE_MVT=1)
endif()

add_subdirectory(src/spatial)
add_subdirectory(src/sgl)
include_directories(src)

include_directories(src/third_party/yyjson/include)
add_subdirectory(src/third_party/yyjson)

include_directories(src/third_party/protozero/include)

if(SPATIAL_USE_SHAPEFILE)
  include_directories(src/third_party/shapelib)
  add_subdirectory(src/third_party/shapelib)
endif()

add_library(${EXTENSION_NAME} STATIC ${EXTENSION_SOURCES})

# annoyingly for expat on windows
set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES} MD.lib)

set(EXTENSION_DEPENDENCIES "")

if(SPATIAL_USE_PROJ OR SPATIAL_USE_GDAL)
  set(ZLIB_USE_STATIC_LIBS ON)
  set(OPENSSL_USE_STATIC_LIBS ON)
  find_package(ZLIB REQUIRED)
  list(APPEND EXTENSION_DEPENDENCIES ZLIB::ZLIB)
endif()

if(SPATIAL_USE_PROJ)
  message(STATUS "Building with PROJ functionality")
  find_package(PROJ CONFIG REQUIRED)
  find_package(EXPAT REQUIRED)
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  list(APPEND EXTENSION_DEPENDENCIES PROJ::proj EXPAT::EXPAT unofficial::sqlite3::sqlite3)
endif()

if(SPATIAL_USE_GDAL)
  message(STATUS "Building with GDAL functionality")
  find_package(GDAL CONFIG REQUIRED)
  list(APPEND EXTENSION_DEPENDENCIES GDAL::GDAL)
endif()

if(SPATIAL_USE_GEOS)
  message(STATUS "Building with GEOS functionality")
  find_package(GEOS REQUIRED)
  # Link both geos_c (C API) and geos (C++ library, provides geos_d2sfixed_buffered_n)
  set(EXTENSION_DEPENDENCIES ${EXTENSION_DEPENDENCIES} GEOS::geos_c GEOS::geos)
  add_definitions(-DSPATIAL_USE_GEOS=1)
endif()

if(EMSCRIPTEN)
  message(STATUS "Building for Emscripten, disabling network functionality")
  set(SPATIAL_USE_NETWORK OFF)
endif()

if(SPATIAL_USE_NETWORK)
  message(STATUS "Building with network functionality")
  message(STATUS "OpenSSL root dir hint: '$ENV{OPENSSL_ROOT_DIR}'")
  find_package(OpenSSL REQUIRED)
  find_package(CURL REQUIRED)
  list(APPEND EXTENSION_DEPENDENCIES CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()

if(WIN32)
  list(APPEND EXTENSION_DEPENDENCIES wbemuuid.lib)
endif()

if((NOT EMSCRIPTEN) AND (NOT IOS))
  if(APPLE)
    find_library(CoreFoundation_Library CoreFoundation)
    find_library(SystemConfiguration_Library SystemConfiguration)
    list(APPEND EXTENSION_DEPENDENCIES ${CoreFoundation_Library}
         ${SystemConfiguration_Library})
  endif()
endif()

# Add dependencies to extension
target_link_libraries(${EXTENSION_NAME} ${EXTENSION_DEPENDENCIES})

# Build extensions
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${EXTENSION_SOURCES})

target_link_libraries(${TARGET_NAME}_loadable_extension
                      ${EXTENSION_DEPENDENCIES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
