cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(Fidelity C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Fidelity")
set(VITA_TITLEID  "FDLT00001")
set(VITA_VERSION  "01.00")

# Leave a gap after .text: vita-elf-create appends import tables there.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,max-page-size=0x10000")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-unused-function -Wno-format-truncation -ffast-math")

include_directories(third_party/dr_libs third_party/stb)

add_executable(${PROJECT_NAME}
  src/main_vita.c
  src/platform_vita.c
  src/app.c
  src/gfx.c
  src/player.c
  src/decoder.c
  src/tags.c
  src/library.c
)

target_link_libraries(${PROJECT_NAME}
  m
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  ScePower_stub
  SceRtc_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceAVConfig_stub
  SceRegistryMgr_stub
  SceSysmodule_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE assets/Inter-Regular.otf assets/Inter-Regular.otf
  FILE assets/Inter-SemiBold.otf assets/Inter-SemiBold.otf
  FILE assets/Geist-Regular.ttf assets/Geist-Regular.ttf
  FILE assets/Geist-Medium.ttf assets/Geist-Medium.ttf
)
