#include "AudioEngine.h"
#include "CBEngine.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "avrt.lib")

namespace {
struct ComInit {
    HRESULT hr;
    ComInit()  { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

template <class T>
void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::string hrToString(HRESULT hr) {
    char buf[64];
    _snprintf_s(buf, _TRUNCATE, "HRESULT=0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// The endpoint's display name ("Speakers (Realtek Audio)"), for the
// announcement. Empty if the property store will not say.
std::wstring friendlyName(IMMDevice* device) {
    std::wstring name;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            name = v.pwszVal;
        PropVariantClear(&v);
        props->Release();
    }
    return name;
}

// The render loop writes float32 straight into the device buffer, so the mix
// format has to BE float32 - either plain IEEE float or an extensible format
// whose sub-format says so. Shared mode reports float on every current
// Windows endpoint, but an extensible integer format used to be accepted here
// unchecked and would have been fed floats.
bool isFloatFormat(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return f->wBitsPerSample == 32;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && f->wBitsPerSample == 32;
    }
    return false;
}
} // namespace

// Windows calls this on its own notification thread. Only the default-render
// change matters; it raises a flag for the render loop and returns at once.
// Registered for the lifetime of the render thread, which owns the enumerator.
class DefaultDeviceWatcher final : public IMMNotificationClient {
public:
    explicit DefaultDeviceWatcher(AudioEngine& a) : a_(a) {}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole)
            a_.defaultChanged_.store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    // Lives on the render thread's stack for exactly as long as it is
    // registered, so reference counting has nothing to manage.
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
private:
    AudioEngine& a_;
};

AudioEngine::AudioEngine(CBEngine& engine) : engine_(engine) {}

AudioEngine::~AudioEngine() {
    stop();
}

void AudioEngine::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

std::string AudioEngine::lastError() const {
    std::lock_guard<std::mutex> g(infoMu_);
    return lastError_;
}

std::wstring AudioEngine::deviceName() const {
    std::lock_guard<std::mutex> g(infoMu_);
    return deviceName_;
}

void AudioEngine::setError(const std::string& e) {
    std::lock_guard<std::mutex> g(infoMu_);
    lastError_ = e;
}

bool AudioEngine::start() {
    stop_.store(false);
    thread_ = std::thread([this] { renderThread(); });
    // Wait briefly for the thread to publish a sample rate or an error.
    for (int i = 0; i < 200 && sampleRate() == 0.0 && lastError().empty(); ++i) {
        Sleep(5);
    }
    return lastError().empty() && sampleRate() > 0.0;
}

void AudioEngine::renderThread() {
    ComInit com;
    if (FAILED(com.hr)) { setError("CoInitializeEx failed: " + hrToString(com.hr)); return; }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) { setError("MMDeviceEnumerator create failed: " + hrToString(hr)); return; }
    // Without it the program still plays; it just will not follow a change of
    // default output made in Windows. Not worth failing start-up over.
    DefaultDeviceWatcher watcher(*this);
    const bool watching = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(&watcher));

    bool first = true;
    while (!stop_.load()) {
        const SessionEnd end = runSession(enumerator);
        if (end == SessionEnd::Stopped) break;
        // A new default was chosen in Windows: open it straight away. Nothing
        // was lost, so there is no "lost" announcement - the reopen bumps the
        // generation and the UI says which device it is on now.
        if (end == SessionEnd::DefaultChanged) { first = false; continue; }
        // Failing to open the very first device is a start-up failure, which
        // start() reports and the program exits on. After that, a failure means
        // there is no usable output right now: say so once, then keep trying.
        if (first && end == SessionEnd::OpenFailed) break;
        first = false;
        if (!lost_.exchange(true, std::memory_order_acq_rel))
            generation_.fetch_add(1, std::memory_order_acq_rel);
        for (int i = 0; i < 10 && !stop_.load(); ++i) Sleep(50);
    }
    if (watching) enumerator->UnregisterEndpointNotificationCallback(&watcher);
    safeRelease(enumerator);
}

AudioEngine::SessionEnd AudioEngine::runSession(void* enumeratorPtr) {
    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumeratorPtr);
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    HANDLE evt = nullptr;
    HANDLE taskHandle = nullptr;
    UINT32 bufferFrames = 0;
    int channels = 0;
    SessionEnd result = SessionEnd::OpenFailed;

    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) { setError("GetDefaultAudioEndpoint failed: " + hrToString(hr)); goto cleanup; }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (FAILED(hr)) { setError("Activate IAudioClient failed: " + hrToString(hr)); goto cleanup; }

    hr = client->GetMixFormat(&mixFormat);
    if (FAILED(hr)) { setError("GetMixFormat failed: " + hrToString(hr)); goto cleanup; }

    if (!isFloatFormat(mixFormat)) {
        setError("Unexpected default audio format (not 32-bit float).");
        goto cleanup;
    }

    evt = CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!evt) { setError("CreateEventEx failed"); goto cleanup; }

    {
        const REFERENCE_TIME bufferDuration = 300000;  // 30 ms
        hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            bufferDuration, 0, mixFormat, nullptr);
        if (FAILED(hr)) { setError("IAudioClient::Initialize failed: " + hrToString(hr)); goto cleanup; }
    }

    hr = client->SetEventHandle(evt);
    if (FAILED(hr)) { setError("SetEventHandle failed: " + hrToString(hr)); goto cleanup; }

    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    if (FAILED(hr)) { setError("GetService render failed: " + hrToString(hr)); goto cleanup; }

    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) { setError("GetBufferSize failed: " + hrToString(hr)); goto cleanup; }

    {
        DWORD task = 0;
        taskHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &task);
    }

    // The engine learns the rate on THIS thread, before it renders a sample at
    // it - a new device can run at a different rate from the one it replaced.
    engine_.setSampleRate(mixFormat->nSamplesPerSec);

    hr = client->Start();
    if (FAILED(hr)) { setError("IAudioClient::Start failed: " + hrToString(hr)); goto cleanup; }

    // Open. Publish it: name and rate for the announcement, then the generation
    // bump that tells the UI to make one.
    {
        std::lock_guard<std::mutex> g(infoMu_);
        deviceName_ = friendlyName(device);
        lastError_.clear();
    }
    sampleRate_.store(mixFormat->nSamplesPerSec, std::memory_order_release);
    lost_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);

    channels = mixFormat->nChannels;
    result = SessionEnd::Stopped;
    // A change announced while this device was being opened is already
    // satisfied - this IS the current default.
    defaultChanged_.store(false, std::memory_order_release);
    {
        std::vector<float> scratch;
        scratch.reserve(static_cast<size_t>(bufferFrames) * 2);

        while (!stop_.load()) {
            const DWORD wait = WaitForSingleObject(evt, 500);
            if (defaultChanged_.exchange(false, std::memory_order_acq_rel)) {
                result = SessionEnd::DefaultChanged;
                break;
            }
            UINT32 padding = 0;
            // A device that has gone away does not always fail loudly: it can
            // simply stop signalling. So a timeout is a reason to ask, not to
            // carry on waiting - the padding query fails on an invalidated
            // stream (AUDCLNT_E_DEVICE_INVALIDATED), which is the signal.
            if (FAILED(client->GetCurrentPadding(&padding))) { result = SessionEnd::DeviceLost; break; }
            if (wait != WAIT_OBJECT_0) continue;
            const UINT32 available = bufferFrames - padding;
            if (available == 0) continue;

            BYTE* dst = nullptr;
            if (FAILED(render->GetBuffer(available, &dst))) { result = SessionEnd::DeviceLost; break; }

            scratch.assign(static_cast<size_t>(available) * 2, 0.0f);
            engine_.process(scratch.data(), static_cast<int>(available));

            float* out = reinterpret_cast<float*>(dst);
            if (channels == 2) {
                std::copy(scratch.begin(), scratch.end(), out);
            } else {
                // Fan stereo into N channels, zero the rest.
                for (UINT32 f = 0; f < available; ++f) {
                    const float L = scratch[f * 2];
                    const float R = scratch[f * 2 + 1];
                    for (int c = 0; c < channels; ++c) {
                        out[f * channels + c] = (c == 0) ? L : (c == 1 ? R : 0.0f);
                    }
                }
            }

            if (FAILED(render->ReleaseBuffer(available, 0))) { result = SessionEnd::DeviceLost; break; }
        }

        client->Stop();
    }

cleanup:
    if (taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    if (evt) CloseHandle(evt);
    if (mixFormat) CoTaskMemFree(mixFormat);
    safeRelease(render);
    safeRelease(client);
    safeRelease(device);
    return result;
}
