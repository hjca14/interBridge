#include "audio.h"

namespace interbridge {

bool NullAudioIO::startCapture() {
    return false;
}

void NullAudioIO::stopCapture() {}

bool NullAudioIO::startPlayback() {
    return false;
}

void NullAudioIO::stopPlayback() {}

bool NullAudioIO::configure(const AudioConfig& config) {
    (void)config;
    return false;
}

} // namespace interbridge
