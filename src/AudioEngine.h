#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class CBEngine;

// WASAPI shared-mode output. Starts a dedicated render thread that pulls audio
// from CBEngine::process and pushes it to the default Windows audio endpoint.
//
// The thread survives losing its device. Unplugging the headphones invalidates
// the stream, and this used to end the render loop silently and for good - the
// program went quiet while the status line went on saying "Playing". Now the
// thread reopens whatever the default output is, retrying until one exists,
// and bumps deviceGeneration() each time the device is lost or (re)opened so
// the UI can say what happened. It also follows the Windows default: choose a
// different output in Windows and playback moves there, the way other players
// behave, rather than staying on a device that still works but is no longer
// the one you picked.
class AudioEngine {
public:
    explicit AudioEngine(CBEngine& engine);
    ~AudioEngine();

    // Starts COM + WASAPI render loop. Returns true on success.
    // On failure, lastError() explains what went wrong.
    bool start();
    void stop();

    // Both are written by the render thread and read by the UI, so they are
    // copied out under infoMu_ rather than handed out by reference.
    std::string  lastError() const;
    std::wstring deviceName() const;
    double sampleRate() const { return sampleRate_.load(std::memory_order_acquire); }
    // Changes whenever the device is lost or a device is (re)opened.
    unsigned deviceGeneration() const { return generation_.load(std::memory_order_acquire); }
    // True between losing a device and opening another.
    bool deviceLost() const { return lost_.load(std::memory_order_acquire); }

private:
    enum class SessionEnd { Stopped, DeviceLost, OpenFailed, DefaultChanged };
    void renderThread();
    // Opens the current default endpoint and renders until stop() or a device
    // error. Everything it acquires it releases before returning.
    SessionEnd runSession(void* enumerator);
    void setError(const std::string& e);

    CBEngine& engine_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    // Never taken by CBEngine::process(); only by this thread around a device
    // (re)open, and by the UI when it reads the strings.
    mutable std::mutex infoMu_;
    std::string  lastError_;
    std::wstring deviceName_;
    std::atomic<double>   sampleRate_{0.0};
    std::atomic<unsigned> generation_{0};
    std::atomic<bool>     lost_{false};
    // Set from Windows' notification thread when the default render endpoint
    // changes; the render loop checks it and reopens. An atomic flag is all
    // that crosses threads - the callback does no audio work itself.
    std::atomic<bool>     defaultChanged_{false};
    friend class DefaultDeviceWatcher;
};
