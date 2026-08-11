#pragma once

namespace interbridge {

// Placeholder for future codec/audio configuration. Fields will be added
// once the audio hardware (mic, speaker/amp, I2S vs analog, etc.) and the
// codec are chosen. See CONTEXT.md > Open Questions.
struct AudioConfig {};

// Abstraction over audio capture/playback. Real audio hardware and the
// codec/transport used between the ESP32 and the intercom/app have not
// been defined yet - this interface only establishes the shape future
// implementations must have.
class IAudioIO {
public:
    virtual ~IAudioIO() = default;

    virtual bool startCapture() = 0;
    virtual void stopCapture() = 0;

    virtual bool startPlayback() = 0;
    virtual void stopPlayback() = 0;

    virtual bool configure(const AudioConfig& config) = 0;
};

// No-op implementation used until real audio hardware is selected. All
// operations are stubs and must not be treated as functional; it exists
// so callers (e.g. Intercom, coordinators) have a safe default to hold
// instead of a null/optional pointer.
class NullAudioIO : public IAudioIO {
public:
    bool startCapture() override;
    void stopCapture() override;
    bool startPlayback() override;
    void stopPlayback() override;
    bool configure(const AudioConfig& config) override;
};

} // namespace interbridge
