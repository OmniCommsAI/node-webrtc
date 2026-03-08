/* Copyright (c) 2019 The node-webrtc project authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be found
 * in the LICENSE.md file in the root of the source tree. All contributing
 * project authors may be found in the AUTHORS file in the root of the source
 * tree.
 */
#include "src/interfaces/rtc_audio_source.hh"

#include <algorithm>
#include <cstring>

#include <absl/memory/memory.h>
#include <webrtc/api/peer_connection_interface.h>
#include <webrtc/rtc_base/logging.h>
#include <webrtc/rtc_base/platform_thread.h>
#include <webrtc/rtc_base/ref_counted_object.h>
#include <webrtc/rtc_base/thread.h>
#include <webrtc/rtc_base/time_utils.h>

#include "src/converters.hh"
#include "src/converters/arguments.hh"
#include "src/functional/maybe.hh"
#include "src/interfaces/media_stream_track.hh"

namespace node_webrtc {

// ── Ring Buffer Constants ──────────────────────────────────────────────────

/** 10ms frame duration in microseconds. */
static constexpr int64_t kDrainFrameLengthUs = 10000;

/**
 * Maximum tolerable drift before resetting the time base (50ms = 5 frames).
 *
 * Why: If the drain thread falls behind by more than 50ms (due to system load,
 *      context switches, etc.), catching up by processing frames back-to-back
 *      would produce a burst of audio to sinks. Instead, reset the time base
 *      and accept a brief gap — this is less audible than a burst.
 */
static constexpr int64_t kDrainMaxDriftUs = 50000;

// ── RTCAudioTrackSource Implementation ─────────────────────────────────────

RTCAudioTrackSource::~RTCAudioTrackSource() {
  // Signal the drain thread to stop
  stop_drain_.store(true, std::memory_order_release);

  // Wait for the drain thread to finish
  if (drain_thread_) {
    drain_thread_->Finalize();
  }

  PeerConnectionFactory::Release();
  _factory = nullptr;
  _sinks.clear();
}

void RTCAudioTrackSource::PushData(RTCOnDataEventDict dict) {
  if (dict.numberOfFrames.IsJust()) {
    const uint16_t num_frames = dict.numberOfFrames.UnsafeFromJust();
    const size_t num_samples =
        static_cast<size_t>(num_frames) * dict.channelCount;

    // Clamp to max frame size to prevent buffer overflow
    if (num_samples <= kMaxFrameSamples) {
      // Start drain thread on first push (lazy initialization)
      if (!drain_started_.load(std::memory_order_acquire)) {
        // Use compare_exchange to ensure only one thread starts it
        bool expected = false;
        if (drain_started_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
          StartDrainThread();
        }
      }

      // Write to ring buffer — lock-free SPSC write
      const size_t wp = write_pos_.load(std::memory_order_relaxed);
      const size_t rp = read_pos_.load(std::memory_order_acquire);

      if (wp - rp < kRingCapacity) {
        // Buffer has space — write the frame
        CaptureFrame &slot = ring_buffer_[wp % kRingCapacity];
        std::memcpy(slot.data, dict.samples, num_samples * sizeof(int16_t));
        slot.num_samples = num_samples;
        slot.bits_per_sample = dict.bitsPerSample;
        slot.sample_rate = dict.sampleRate;
        slot.channel_count = dict.channelCount;
        slot.num_frames = num_frames;
        // Release: ensure all slot writes are visible before advancing wp
        write_pos_.store(wp + 1, std::memory_order_release);
      }
      // else: ring buffer full — drop this frame (overflow).
      // This happens when JS pushes faster than real-time (e.g. TTS burst).
      // The drain thread will catch up and space will open.
    }
  }

  // Free the samples buffer allocated by the NAPI converter
  delete[] dict.samples;
}

void RTCAudioTrackSource::StartDrainThread() {
  drain_thread_ =
      absl::make_unique<rtc::PlatformThread>(rtc::PlatformThread::SpawnJoinable(
          [this]() { RTCAudioTrackSource::RunDrain(this); },
          "RTCAudioTrackDrain",
          rtc::ThreadAttributes{rtc::ThreadPriority::kHigh}));
}

void RTCAudioTrackSource::DrainLoop() {
  int64_t time_us = rtc::TimeMicros();

  while (!stop_drain_.load(std::memory_order_acquire)) {
    // Advance the absolute target time by one 10ms frame interval
    time_us += kDrainFrameLengthUs;

    // Try to read one frame from the ring buffer
    const size_t rp = read_pos_.load(std::memory_order_relaxed);
    const size_t wp = write_pos_.load(std::memory_order_acquire);

    if (rp < wp) {
      // Frame available — read and deliver to sinks
      const CaptureFrame &frame = ring_buffer_[rp % kRingCapacity];

      {
        std::shared_lock<std::shared_mutex> lock{_sinks_mutex};
        for (auto sink : _sinks) {
          sink->OnData(frame.data, frame.bits_per_sample, frame.sample_rate,
                       frame.channel_count, frame.num_frames);
        }
      }

      // Release: advance read position after delivery is complete
      read_pos_.store(rp + 1, std::memory_order_release);
    }
    // else: ring buffer empty — underrun. No audio to deliver this tick.
    // This is normal during call setup or brief pauses in TTS output.
    // Sinks simply don't receive a frame this tick — WebRTC handles the gap.

    // Sleep until the next 10ms boundary
    int64_t time_left_us = time_us - rtc::TimeMicros();

    if (time_left_us < -kDrainMaxDriftUs) {
      // Fallen behind by more than 50ms — reset time base.
      // Better to have a brief gap than to burst-deliver multiple frames.
      time_us = rtc::TimeMicros();
    } else if (time_left_us > 0) {
      // Normal case: sleep until next frame is due
      while (time_left_us > 1000) {
        if (rtc::Thread::SleepMs(static_cast<int>(time_left_us / 1000))) {
          break;
        }
        time_left_us = time_us - rtc::TimeMicros();
      }
    }
    // If slightly behind (0 to -50ms), proceed immediately to catch up.
  }
}

// ── RTCAudioSource (NAPI wrapper) Implementation ───────────────────────────

Napi::FunctionReference &RTCAudioSource::constructor() {
  static Napi::FunctionReference constructor;
  return constructor;
}

RTCAudioSource::RTCAudioSource(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<RTCAudioSource>(info) {
  _source = rtc::make_ref_counted<RTCAudioTrackSource>();
}

Napi::Value RTCAudioSource::CreateTrack(const Napi::CallbackInfo &) {
  // TODO(mroberts): Again, we have some implicit factory we are threading
  // around. How to handle?
  auto factory = PeerConnectionFactory::GetOrCreateDefault();
  auto track =
      factory->factory()->CreateAudioTrack(rtc::CreateRandomUuid(), _source);
  return _track_wrap.GetOrCreate(factory, track)->Value();
}

Napi::Value RTCAudioSource::OnData(const Napi::CallbackInfo &info) {
  CONVERT_ARGS_OR_THROW_AND_RETURN_NAPI(info, dict, RTCOnDataEventDict)
  _source->PushData(dict);
  return info.Env().Undefined();
}

void RTCAudioSource::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func =
      DefineClass(env, "RTCAudioSource",
                  {InstanceMethod("createTrack", &RTCAudioSource::CreateTrack),
                   InstanceMethod("onData", &RTCAudioSource::OnData)});

  constructor() = Napi::Persistent(func);
  constructor().SuppressDestruct();

  exports.Set("RTCAudioSource", func);
}

} // namespace node_webrtc
