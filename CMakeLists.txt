cmake_minimum_required(VERSION 3.22 FATAL_ERROR)
project(libremidi
  VERSION 4.5.0
  DESCRIPTION "A cross-platform MIDI library"
  LANGUAGES CXX
  HOMEPAGE_URL "https://github.com/jcelerier/libremidi"
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CMakeDependentOption)
include(CheckSymbolExists)
include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(FetchContent)
include(GNUInstallDirs)

### Options ###
option(LIBREMIDI_HEADER_ONLY "Header-only mode" OFF)

cmake_dependent_option(LIBREMIDI_NO_COREMIDI "Disable CoreMidi back-end" OFF "APPLE" OFF)
cmake_dependent_option(LIBREMIDI_NO_WINMM "Disable WinMM back-end" OFF "WIN32" OFF)
cmake_dependent_option(LIBREMIDI_NO_WINUWP "Disable UWP back-end" ON "WIN32" OFF)
cmake_dependent_option(LIBREMIDI_NO_WINMIDI "Disable WinMIDI back-end" ON "WIN32" OFF)
# if(LINUX) in CMake 3.25
cmake_dependent_option(LIBREMIDI_NO_ALSA "Disable ALSA back-end" OFF "UNIX; NOT APPLE" OFF)
cmake_dependent_option(LIBREMIDI_NO_UDEV "Disable udev support for ALSA" OFF "UNIX; NOT APPLE" OFF)
option(LIBREMIDI_NO_JACK "Disable JACK back-end" OFF)
option(LIBREMIDI_NO_PIPEWIRE "Disable PipeWire back-end" OFF)

option(LIBREMIDI_NO_EXPORTS "Disable dynamic symbol exporting" OFF)
option(LIBREMIDI_NO_BOOST "Do not use Boost if available" OFF)
option(LIBREMIDI_SLIM_MESSAGE "Use a fixed-size message format" 0)
option(LIBREMIDI_FIND_BOOST "Actively look for Boost" OFF)
option(LIBREMIDI_EXAMPLES "Enable examples" OFF)
option(LIBREMIDI_TESTS "Enable tests" OFF)
option(LIBREMIDI_NI_MIDI2 "Enable compatibility with ni-midi2" OFF)
option(LIBREMIDI_CI "To be enabled only in CI, some tests cannot run there. Also enables -Werror." OFF)

cmake_dependent_option(LIBREMIDI_NO_WARNINGS "Disables warnings from library compilation" OFF "NOT LIBREMIDI_HEADER_ONLY" ON)

### Basic setup ###
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()

### Dependencies ###
include(libremidi.deps)

### Main library ###
if(LIBREMIDI_HEADER_ONLY)
  add_library(libremidi INTERFACE)
  set(_public INTERFACE)
  set(_private INTERFACE)
  target_compile_definitions(libremidi ${_public} LIBREMIDI_HEADER_ONLY)
else()
  add_library(libremidi)
  include(libremidi.sources)

  set(_public PUBLIC)
  set(_private PRIVATE)

  set_target_properties(libremidi PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION}
  )
endif()
add_library(libremidi::libremidi ALIAS libremidi)

### Library configuration ###
if(NOT LIBREMIDI_NO_WARNINGS)
  include(libremidi.warnings)
endif()

if(LIBREMIDI_SLIM_MESSAGE GREATER 0)
  target_compile_definitions(libremidi ${_public} LIBREMIDI_SLIM_MESSAGE=${LIBREMIDI_SLIM_MESSAGE})
endif()

if(LIBREMIDI_NO_BOOST)
  target_compile_definitions(libremidi ${_public} LIBREMIDI_NO_BOOST)
  message(STATUS "libremidi: Using std::vector for libremidi::message")
else()
  # Use of boost is public as it changes the ABI of libremidi::message
  if(TARGET Boost::boost)
    target_compile_definitions(libremidi ${_public} LIBREMIDI_USE_BOOST)
    target_link_libraries(libremidi ${_public} $<BUILD_INTERFACE:Boost::boost>)
    message(STATUS "libremidi: Using boost::small_vector for libremidi::message")
  elseif(Boost_INCLUDE_DIR)
    target_compile_definitions(libremidi ${_public} LIBREMIDI_USE_BOOST)
    target_include_directories(libremidi SYSTEM ${_public} $<BUILD_INTERFACE:${Boost_INCLUDE_DIR}>)
    message(STATUS "libremidi: Using boost::small_vector for libremidi::message")
  else()
    message(STATUS "libremidi: Using std::vector for libremidi::message")
  endif()
endif()

if(NOT LIBREMIDI_NO_EXPORTS)
  target_compile_definitions(libremidi ${_private} LIBREMIDI_EXPORTS)
endif()

if(LIBREMIDI_NI_MIDI2)
  target_compile_definitions(libremidi ${_public} LIBREMIDI_USE_NI_MIDI2)
  target_link_libraries(libremidi ${_public} $<BUILD_INTERFACE:ni::midi2>)
endif()

target_compile_features(libremidi ${_public} cxx_std_20)

if(CMAKE_THREAD_LIBS_INIT)
  target_link_libraries(libremidi ${_public} ${CMAKE_THREAD_LIBS_INIT})
endif()

target_include_directories(libremidi SYSTEM ${_public}
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
if(NOT LIBREMIDI_HEADER_ONLY)
	target_include_directories(libremidi ${_private} # Makes any above set warnings apply normally, without leaking out to clients
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	)
endif()

### Backends ###
if(EMSCRIPTEN)
  include(libremidi.emscripten)
endif()

if(WIN32)
  if(${CMAKE_SYSTEM_NAME} MATCHES WindowsStore)
    set(LIBREMIDI_NO_WINMM 1)
  endif()

  if(NOT LIBREMIDI_NO_WINMM)
    include(libremidi.winmm)
  endif()

  if(NOT LIBREMIDI_NO_WINUWP)
    include(libremidi.winuwp)
  endif()
endif()

if(APPLE AND NOT LIBREMIDI_NO_COREMIDI)
  include(libremidi.macos)
endif()

if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN AND NOT LIBREMIDI_NO_ALSA)
  include(libremidi.alsa)
endif()

if(NOT LIBREMIDI_NO_JACK)
  include(libremidi.jack)
endif()

if(NOT LIBREMIDI_NO_PIPEWIRE)
  include(libremidi.pipewire)
endif()

### Install ###
include(libremidi.install)

### Examples ###
if(LIBREMIDI_EXAMPLES)
  message(STATUS "libremidi: compiling examples")
  include(libremidi.examples)
endif()

### Tests ###
if(LIBREMIDI_TESTS)
  message(STATUS "libremidi: compiling tests")
  include(libremidi.tests)
endif()
