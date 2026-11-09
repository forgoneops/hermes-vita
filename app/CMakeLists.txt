cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK to a VitaSDK installation (or use scripts/build-vpk.sh, which uses the vitasdk Docker image)")
  endif()
endif()

project(hermes_vita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "HermesVita")
set(VITA_TITLEID "HERM00002")
set(VITA_VERSION "01.00")
string(TIMESTAMP BUILD_ID "%Y%m%d-%H%M%S" UTC)
file(WRITE ${CMAKE_BINARY_DIR}/build_id.txt "${BUILD_ID}")   # scripts/build-vpk.sh copies it to dist/ so the bridge can tell the app a newer build exists
add_compile_definitions(POCKET_HERMES=1 BUILD_ID="${BUILD_ID}")
# with freetype linked the code segment ends where data starts, and vita-elf-create fails without this
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,separate-code")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -Wall -Wextra -O2")

add_executable(${PROJECT_NAME} src/hermes.c src/live.c)

target_link_libraries(${PROJECT_NAME}
  vita2d freetype bz2 jpeg png z
  SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceSysmodule_stub SceAppMgr_stub SceAppUtil_stub
  SceCommonDialog_stub SceIme_stub ScePgf_stub SceProcessmgr_stub SceNet_stub SceRtc_stub
  SceAudioIn_stub SceAudio_stub SceNetCtl_stub SceHttp_stub SceIofilemgr_stub m
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE assets/hermes/plate.jpg assets/hermes/plate.jpg
  FILE assets/hermes/plate_open.jpg assets/hermes/plate_open.jpg
  FILE assets/hermes/plate_talk.jpg assets/hermes/plate_talk.jpg
  FILE assets/hermes/font.ttf assets/hermes/font.ttf
)
