// Audio.h 는 콜백 선언 자체에 weak 를 붙인다. 그 헤더를 포함한 파일에서
// 구현하면 사용자 구현도 weak 가 되어 링크 순서에 따라 빈 기본 함수가
// 선택될 수 있다. 이 파일에서는 헤더를 포함하지 않아 강한 심볼을 제공한다.
#include <stdint.h>

void p4ProcessAudioFrame();

void audio_process_raw_samples(int32_t*, int16_t) {
    p4ProcessAudioFrame();
}
