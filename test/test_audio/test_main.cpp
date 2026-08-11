#include <unity.h>

#include "../../src/audio/audio.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_null_audio_capture_is_not_functional() {
    NullAudioIO audio;
    TEST_ASSERT_FALSE(audio.startCapture());
}

void test_null_audio_playback_is_not_functional() {
    NullAudioIO audio;
    TEST_ASSERT_FALSE(audio.startPlayback());
}

void test_null_audio_configure_is_not_functional() {
    NullAudioIO audio;
    AudioConfig config{};
    TEST_ASSERT_FALSE(audio.configure(config));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_null_audio_capture_is_not_functional);
    RUN_TEST(test_null_audio_playback_is_not_functional);
    RUN_TEST(test_null_audio_configure_is_not_functional);
    return UNITY_END();
}
