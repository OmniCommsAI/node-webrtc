/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *  Modified (c) 2026 OmniCommsAI — BufferedAudioDeviceModule.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/**
 * BufferedAudioDeviceModule — Production-grade AudioDeviceModule replacement.
 *
 * What: Drop-in replacement for TestAudioDeviceModule that improves the audio
 *       processing loop timing. Uses the same Capturer/Renderer interfaces and
 *       factory pattern as the original.
 *
 * Why:  TestAudioDeviceModule was ported from Google's WebRTC test scaffolding.
 *       Its ProcessAudio loop logs "ProcessAudio is too slow" on any scheduling
 *       drift and never recovers, leading to bursty frame processing. This
 *       replacement handles drift gracefully by resetting the time base when
 *       falling behind, preventing back-to-back frame bursts that corrupt audio.
 *
 * The class preserves the same public API surface as TestAudioDeviceModule so
 * existing call sites (PeerConnectionFactory, MediaStreamTrack) require only a
 * type-name swap.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>

#include <webrtc/api/array_view.h>
#include <webrtc/api/scoped_refptr.h>
#include <webrtc/modules/audio_device/include/audio_device.h>
#include <webrtc/modules/audio_device/include/test_audio_device.h>
#include <webrtc/rtc_base/buffer.h>
#include <webrtc/rtc_base/event.h>
#include <webrtc/rtc_base/ref_counted_object.h>

namespace webrtc {
class AudioTransport;
}

namespace node_webrtc {

class BufferedAudioDeviceModule
    : public rtc::RefCountedObject<webrtc::AudioDeviceModule> {
public:
  /** Returns the number of samples in a 10ms frame for the given sample rate. */
  static size_t SamplesPerFrame(int sampling_frequency_in_hz);

  ~BufferedAudioDeviceModule() override = default;

  /**
   * Factory method — drop-in replacement for
   * TestAudioDeviceModule::CreateTestAudioDeviceModule().
   *
   * |capturer| — produces audio data for recording. May be nullptr.
   * |renderer| — receives rendered (playout) audio data. May be nullptr.
   * |speed|    — processing cadence multiplier. 1.0 = real-time (10ms ticks).
   */
  static rtc::scoped_refptr<BufferedAudioDeviceModule> Create(
      std::unique_ptr<webrtc::TestAudioDeviceModule::Capturer> capturer,
      std::unique_ptr<webrtc::TestAudioDeviceModule::Renderer> renderer,
      float speed = 1);

  /** Creates a Capturer that produces silence (all-zero samples). */
  static std::unique_ptr<webrtc::TestAudioDeviceModule::Capturer>
  CreateZeroCapturer(int sampling_frequency_in_hz, int num_channels);

  // AudioDeviceModule interface — implemented in .cc
  int32_t Init() override = 0;
  int32_t RegisterAudioCallback(webrtc::AudioTransport* callback) override = 0;
  int32_t StartPlayout() override = 0;
  int32_t StopPlayout() override = 0;
  int32_t StartRecording() override = 0;
  int32_t StopRecording() override = 0;
  bool Playing() const override = 0;
  bool Recording() const override = 0;

  /** Blocks until the Renderer stops accepting data or timeout. */
  virtual bool WaitForPlayoutEnd(int timeout_ms = rtc::Event::kForever) = 0;

  /** Blocks until the Capturer stops producing data or timeout. */
  virtual bool WaitForRecordingEnd(int timeout_ms = rtc::Event::kForever) = 0;
};

} // namespace node_webrtc
