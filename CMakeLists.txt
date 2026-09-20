cmake_minimum_required(VERSION 3.16)
project(BladeVR CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "BladeVR solo se compila en Windows (x64, MSVC).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Blade.exe es de 64 bits: configura el generador como x64 (cmake -A x64).")
endif()

# MinHook (hooks de funciones), descargado en el configure.
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
)
FetchContent_MakeAvailable(minhook)

# ---------------------------------------------------------------------------
# BladeVR.dll -- el mod (hookdll/)
# ---------------------------------------------------------------------------
file(GLOB HOOKDLL_SOURCES CONFIGURE_DEPENDS hookdll/*.cpp hookdll/*.h)
add_library(BladeVR SHARED ${HOOKDLL_SOURCES})

target_compile_definitions(BladeVR PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(BladeVR PRIVATE ThirdParty/openvr)
# openvr_api.dll se carga en tiempo de ejecucion (LoadLibrary), no se enlaza.
target_link_libraries(BladeVR PRIVATE minhook d3d11 dxgi dxguid)

if(MSVC)
    target_compile_options(BladeVR PRIVATE /W3 /permissive- /Zi)
    # PDB tambien en Release para poder leer volcados/logs de crash con simbolos.
    target_link_options(BladeVR PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# ---------------------------------------------------------------------------
# dxgi.dll -- proxy que carga BladeVR.dll al arrancar el juego (proxydll/)
# ---------------------------------------------------------------------------
enable_language(ASM_MASM)
add_library(BladeVRDxgiProxy SHARED proxydll/ProxyMain.cpp proxydll/ProxyThunks.asm)
target_compile_definitions(BladeVRDxgiProxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    # Solo para C++: MASM no entiende /permissive-
    target_compile_options(BladeVRDxgiProxy PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3;/permissive->)
endif()
set_target_properties(BladeVRDxgiProxy PROPERTIES OUTPUT_NAME "dxgi")
add_dependencies(BladeVRDxgiProxy BladeVR)

# Todo a output/<Config>/, junto con openvr_api.dll.
foreach(tgt BladeVR BladeVRDxgiProxy)
    set_target_properties(${tgt} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)
endforeach()
add_custom_command(TARGET BladeVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/ThirdParty/openvr/openvr_api.dll
        $<TARGET_FILE_DIR:BladeVR>/openvr_api.dll)
