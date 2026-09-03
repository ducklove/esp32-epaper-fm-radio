# ESP32-P4 빌드 전에 C6 코프로세서 펌웨어의 경로를 헤더로 만들어 준다.
#
# Arduino 코어(framework-arduinoespressif32-libs)는 호스트 라이브러리와 같은
# 버전의 ESP-Hosted 슬레이브 바이너리를 hosted/ 에 넣어 둔다. 그 파일을
# src/boards/p4/c6fw.S 가 .incbin 으로 P4 펌웨어에 박는다.
#
# PlatformIO 의 board_build.embed_files 를 쓰지 않는 이유 둘:
#   - 심볼 이름에 파일 경로 전체가 들어간다 (_binary_C__pio_packages_..._start).
#     빌드 환경마다 달라져서 코드에서 참조할 수 없다.
#   - .data 섹션에 넣어서 부팅 때 RAM 으로 복사하려 든다. 1.2MB 라 안 된다.
# 직접 만든 .S 는 .rodata 에 두고 심볼 이름을 고정한다.
import os

Import("env")

libs = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
fw = os.path.join(libs, "hosted", "esp32c6-v2.12.11.bin")
if not os.path.isfile(fw):
    raise SystemExit("C6 펌웨어를 찾지 못함: " + fw)

build_dir = env.subst("$BUILD_DIR")
os.makedirs(build_dir, exist_ok=True)
header = os.path.join(build_dir, "c6fw_path.h")
# GAS 의 .incbin 은 윈도에서도 슬래시를 받는다. 역슬래시는 이스케이프로 읽는다.
content = '#define C6FW_PATH "%s"\n' % fw.replace("\\", "/")
if not os.path.isfile(header) or open(header).read() != content:
    with open(header, "w") as f:
        f.write(content)

env.Append(CPPPATH=[build_dir])
print("C6 펌웨어 임베드: " + fw + " (%d bytes)" % os.path.getsize(fw))
