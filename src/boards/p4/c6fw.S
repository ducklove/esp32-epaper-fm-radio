// ESP32-C6 (Wi-Fi 코프로세서) 펌웨어를 P4 펌웨어 안에 그대로 넣는다.
// 경로는 빌드 때 tools/p4_c6fw.py 가 c6fw_path.h 로 만들어 준다.
// 플래시(.rodata)에 두고, c6update.cpp 가 SDIO 로 C6 에 밀어 넣는다.
#include "c6fw_path.h"

    .section .rodata.embedded, "a"
    .align 4
    .global c6fw_start
c6fw_start:
    .incbin C6FW_PATH
    .global c6fw_end
c6fw_end:
    .byte 0
