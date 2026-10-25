#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <cmath>
#include <string>

class AudioAnalyzer {
public:
    IAudioCaptureClient* pCaptureClient = nullptr;
    IAudioClient* pAudioClient = nullptr;
    float currentBassEnergy = 0.0f;
    float bassHit = 0.0f;
    uint32_t beatIndex = 0;
    UINT beatCount = 0;
    float beatAges[8]{};

    ~AudioAnalyzer() {
        if (pCaptureClient) pCaptureClient->Release();
        if (pAudioClient) {
            pAudioClient->Stop();
            pAudioClient->Release();
        }
        if (comInitialized) CoUninitialize();
    }

    bool InitWASAPILoopback() {
        if (!comInitialized) {
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(comResult)) {
                std::cerr << "CoInitializeEx failed: 0x" << std::hex << comResult << '\n';
                return false;
            }
            comInitialized = true;
        }

        IMMDeviceEnumerator* pEnumerator = nullptr;
        IMMDevice* pDevice = nullptr;

        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(&pEnumerator));
        if (SUCCEEDED(result)) {
            result = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        }
        LPWSTR deviceId = nullptr;
        if (SUCCEEDED(result)) result = pDevice->GetId(&deviceId);
        if (SUCCEEDED(result)) {
            result = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&pAudioClient));
        }
        WAVEFORMATEX* pwfx = nullptr;
        if (SUCCEEDED(result)) result = pAudioClient->GetMixFormat(&pwfx);
        if (SUCCEEDED(result)) {
            isFloatFormat = pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                pwfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
                const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
                isFloatFormat = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                isPcmFormat = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
            } else {
                isPcmFormat = pwfx->wFormatTag == WAVE_FORMAT_PCM;
            }
            channels = pwfx->nChannels;
            bitsPerSample = pwfx->wBitsPerSample;
            sampleRate = pwfx->nSamplesPerSec;
            result = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_LOOPBACK,
                                              1000000, 0, pwfx, nullptr);
        }
        if (pwfx) CoTaskMemFree(pwfx);
        if (SUCCEEDED(result)) {
            result = pAudioClient->GetService(__uuidof(IAudioCaptureClient),
                                              reinterpret_cast<void**>(&pCaptureClient));
        }
        if (SUCCEEDED(result)) result = pAudioClient->Start();

        if (pDevice) pDevice->Release();
        if (pEnumerator) pEnumerator->Release();
        if (deviceId) {
            if (SUCCEEDED(result)) activeEndpointId = deviceId;
            CoTaskMemFree(deviceId);
        }
        if (FAILED(result)) {
            std::cerr << "WASAPI loopback initialization failed: 0x" << std::hex
                      << result << '\n';
            return false;
        }
        if ((!isFloatFormat && !isPcmFormat) || channels == 0 || sampleRate == 0 ||
            (isFloatFormat && bitsPerSample != 32) ||
            (isPcmFormat && bitsPerSample != 16 && bitsPerSample != 24 &&
             bitsPerSample != 32)) {
            std::cerr << "Unsupported WASAPI mix format\n";
            return false;
        }
        return true;
    }

    void CheckForDefaultDeviceChange() {
        if (!comInitialized) return;
        const ULONGLONG now = GetTickCount64();
        if (now < nextDeviceCheck) return;
        nextDeviceCheck = now + 1000;

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(result)) {
            result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        LPWSTR currentId = nullptr;
        if (SUCCEEDED(result)) result = device->GetId(&currentId);
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (FAILED(result)) {
            std::cerr << "Could not check the default audio endpoint: 0x"
                      << std::hex << result << '\n';
            return;
        }

        const bool changed = activeEndpointId != currentId;
        CoTaskMemFree(currentId);
        if (!changed) return;

        std::cerr << "Default audio endpoint changed; restarting WASAPI loopback\n";
        ReleaseAudioClient();
        currentBassEnergy = 0.0f;
        bassHit = 0.0f;
        bassLowPass = 0.0f;
        beatCount = 0;
        std::fill(std::begin(beatAges), std::end(beatAges), 0.0f);
        if (!InitWASAPILoopback()) {
            std::cerr << "WASAPI loopback could not reconnect to the new endpoint\n";
        }
    }

    void ProcessAudioFrame() {
        const ULONGLONG now = GetTickCount64();
        const float elapsed = lastProcessTime == 0
            ? 0.0f
            : static_cast<float>(now - lastProcessTime) / 1000.0f;
        lastProcessTime = now;
        for (UINT i = 0; i < beatCount; ++i) {
            beatAges[i] += elapsed;
        }
        if (!pCaptureClient) return;
        UINT32 packetLength = 0;
        if (FAILED(pCaptureClient->GetNextPacketSize(&packetLength))) return;

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            if (FAILED(pCaptureClient->GetBuffer(&data, &framesAvailable, &flags,
                                                 nullptr, nullptr))) {
                return;
            }

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && framesAvailable > 0) {
                const float packetSeconds =
                    static_cast<float>(framesAvailable) / sampleRate;
                const float hitDecay = std::exp(-packetSeconds / 0.15f);
                const UINT32 bytesPerSample = bitsPerSample / 8;
                double energy = 0.0;
                const float lowPassCoefficient =
                    1.0f - std::exp(-6.28318530718f * 150.0f / sampleRate);
                for (UINT32 frame = 0; frame < framesAvailable; ++frame) {
                    float monoSample = 0.0f;
                    for (UINT channel = 0; channel < channels; ++channel) {
                        float sample = 0.0f;
                        const BYTE* sampleData =
                            data + (frame * channels + channel) * bytesPerSample;
                        if (isFloatFormat) {
                            std::memcpy(&sample, sampleData, sizeof(sample));
                        } else if (bitsPerSample == 16) {
                            int16_t value;
                            std::memcpy(&value, sampleData, sizeof(value));
                            sample = value / 32768.0f;
                        } else if (bitsPerSample == 24) {
                            int32_t value = static_cast<int32_t>(sampleData[0]) |
                                            (static_cast<int32_t>(sampleData[1]) << 8) |
                                            (static_cast<int32_t>(sampleData[2]) << 16);
                            if (value & 0x800000) value |= ~0xFFFFFF;
                            sample = value / 8388608.0f;
                        } else {
                            int32_t value;
                            std::memcpy(&value, sampleData, sizeof(value));
                            sample = static_cast<float>(value / 2147483648.0);
                        }
                        monoSample += sample;
                    }
                    monoSample /= channels;
                    bassLowPass += lowPassCoefficient * (monoSample - bassLowPass);
                    energy += bassLowPass * bassLowPass;
                }
                const float bassRms =
                    static_cast<float>(std::sqrt(energy / framesAvailable));
                const float intensity = (std::max)(0.0f, (bassRms - 0.025f) * 5.0f);
                const float target = (std::min)(1.0f, intensity);
                const float smoothing = target > currentBassEnergy ? 0.65f : 0.10f;
                const float rise = (std::max)(0.0f, target - currentBassEnergy);
                currentBassEnergy += (target - currentBassEnergy) * smoothing;
                bassHit = (std::max)(rise * 8.0f, bassHit * hitDecay);
                const ULONGLONG now = GetTickCount64();
                if (rise >= 0.055f && now - lastBeatTime >= 160) {
                    ++beatIndex;
                    lastBeatTime = now;
                    for (UINT i = 7; i > 0; --i) {
                        beatAges[i] = beatAges[i - 1];
                    }
                    beatAges[0] = 0.0f;
                    beatCount = (std::min)(beatCount + 1, 8u);
                }
            } else {
                bassLowPass *= 0.98f;
                currentBassEnergy *= 0.92f;
                const float packetSeconds =
                    static_cast<float>(framesAvailable) / sampleRate;
                bassHit *= std::exp(-packetSeconds / 0.15f);
            }

            pCaptureClient->ReleaseBuffer(framesAvailable);
            if (FAILED(pCaptureClient->GetNextPacketSize(&packetLength))) return;
        }
    }

private:
    void ReleaseAudioClient() {
        if (pCaptureClient) {
            pCaptureClient->Release();
            pCaptureClient = nullptr;
        }
        if (pAudioClient) {
            pAudioClient->Stop();
            pAudioClient->Release();
            pAudioClient = nullptr;
        }
    }

    bool comInitialized = false;
    bool isFloatFormat = false;
    bool isPcmFormat = false;
    UINT channels = 0;
    WORD bitsPerSample = 0;
    UINT sampleRate = 0;
    float bassLowPass = 0.0f;
    ULONGLONG lastBeatTime = 0;
    ULONGLONG lastProcessTime = 0;
    std::wstring activeEndpointId;
    ULONGLONG nextDeviceCheck = 0;
};