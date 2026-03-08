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

  // Clean up any pending capture TSFNs to prevent leaks.
  // At this point the drain thread has exited, so no lock contention.
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto &pc : pending_capture_) {
      pc.tsfn.Release();
    }
    pending_capture_.clear();

    if (waiting_for_playout_.load(std::memory_order_relaxed)) {
      playout_tsfn_.Release();
      waiting_for_playout_.store(false, std::memory_order_relaxed);
    }
  }

  PeerConnectionFactory::Release();
  _factory = nullptr;
  _sinks.clear();
}

void RTCAudioTrackSource::EnsureDrainThread() {
  if (!drain_started_.load(std::memory_order_acquire)) {
    // Use compare_exchange to ensure only one thread starts it
    bool expected = false;
    if (drain_started_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
      StartDrainThread();
    }
  }
}

void RTCAudioTrackSource::PushData(RTCOnDataEventDict dict) {
  if (dict.numberOfFrames.IsJust()) {
    const uint16_t num_frames = dict.numberOfFrames.UnsafeFromJust();
    const size_t num_samples =
        static_cast<size_t>(num_frames) * dict.channelCount;

    // Clamp to max frame size to prevent buffer overflow
    if (num_samples <= kMaxFrameSamples) {
      // Start drain thread on first push (lazy initialization)
      EnsureDrainThread();

      // Write to ring buffer — lock-free SPSC write
      const size_t wp = write_pos_.load(std::memory_order_relaxed);
      const size_t rp = read_pos_.load(std::memory_order_acquire);

      if (wp - rp < kRingCapacity) {
        // Buffer has space — write the frame
        RingFrame &slot = ring_buffer_[wp % kRingCapacity];
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

bool RTCAudioTrackSource::TryPushData(RTCOnDataEventDict &dict) {
  if (!dict.numberOfFrames.IsJust()) {
    return false;
  }

  const uint16_t num_frames = dict.numberOfFrames.UnsafeFromJust();
  const size_t num_samples =
      static_cast<size_t>(num_frames) * dict.channelCount;

  if (num_samples > kMaxFrameSamples) {
    return false;
  }

  // Start drain thread on first push (lazy initialization)
  EnsureDrainThread();

  // Write to ring buffer — lock-free SPSC write
  const size_t wp = write_pos_.load(std::memory_order_relaxed);
  const size_t rp = read_pos_.load(std::memory_order_acquire);

  if (wp - rp >= kRingCapacity) {
    // Ring buffer full — caller should enqueue a pending capture
    return false;
  }

  // Buffer has space — write the frame
  RingFrame &slot = ring_buffer_[wp % kRingCapacity];
  std::memcpy(slot.data, dict.samples, num_samples * sizeof(int16_t));
  slot.num_samples = num_samples;
  slot.bits_per_sample = dict.bitsPerSample;
  slot.sample_rate = dict.sampleRate;
  slot.channel_count = dict.channelCount;
  slot.num_frames = num_frames;

  // Release: ensure all slot writes are visible before advancing wp
  write_pos_.store(wp + 1, std::memory_order_release);
  return true;
}

void RTCAudioTrackSource::ClearBuffer() {
  // Reset ring buffer positions atomically. The drain thread will see
  // read_pos_ == write_pos_ and treat it as empty.
  const size_t wp = write_pos_.load(std::memory_order_relaxed);
  read_pos_.store(wp, std::memory_order_release);

  // Resolve/release all pending captures and playout waiter
  std::lock_guard<std::mutex> lock(pending_mutex_);

  for (auto &pc : pending_capture_) {
    // Call the TSFN to resolve the promise on the JS thread, then release.
    // The callback will receive nullptr status indicating "cleared".
    pc.tsfn.NonBlockingCall();
    pc.tsfn.Release();
  }
  pending_capture_.clear();

  if (waiting_for_playout_.load(std::memory_order_relaxed)) {
    playout_tsfn_.NonBlockingCall();
    playout_tsfn_.Release();
    waiting_for_playout_.store(false, std::memory_order_release);
  }
}

size_t RTCAudioTrackSource::QueuedFrames() const {
  const size_t wp = write_pos_.load(std::memory_order_acquire);
  const size_t rp = read_pos_.load(std::memory_order_acquire);
  return (wp >= rp) ? (wp - rp) : 0;
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
      const RingFrame &frame = ring_buffer_[rp % kRingCapacity];

      {
        std::shared_lock<std::shared_mutex> lock{_sinks_mutex};
        for (auto sink : _sinks) {
          sink->OnData(frame.data, frame.bits_per_sample, frame.sample_rate,
                       frame.channel_count, frame.num_frames);
        }
      }

      // Release: advance read position after delivery is complete
      read_pos_.store(rp + 1, std::memory_order_release);

      // ── Post-consume: service pending captures ────────────────────────
      //
      // Why: After consuming a frame, there's now space in the ring buffer.
      //      If captureFrame() calls are waiting, push their frame and resolve
      //      the promise. Only hold the mutex briefly — never during sleep.
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (!pending_capture_.empty()) {
          // Pop the oldest pending capture
          PendingCapture pc = std::move(pending_capture_.front());
          pending_capture_.erase(pending_capture_.begin());

          // Push the pending frame into the ring buffer (we just freed a slot)
          const size_t new_wp = write_pos_.load(std::memory_order_relaxed);
          RingFrame &slot = ring_buffer_[new_wp % kRingCapacity];
          std::memcpy(slot.data, pc.data, pc.num_samples * sizeof(int16_t));
          slot.num_samples = pc.num_samples;
          slot.bits_per_sample = pc.bits_per_sample;
          slot.sample_rate = pc.sample_rate;
          slot.channel_count = pc.channel_count;
          slot.num_frames = pc.num_frames;
          write_pos_.store(new_wp + 1, std::memory_order_release);

          // Resolve the JS promise via ThreadSafeFunction
          pc.tsfn.NonBlockingCall();
          pc.tsfn.Release();
        }
      }

      // ── Post-consume: check waitForPlayout ────────────────────────────
      //
      // Why: If someone called waitForPlayout() and the buffer just became
      //      empty, resolve their promise now.
      {
        const size_t new_rp = read_pos_.load(std::memory_order_acquire);
        const size_t new_wp = write_pos_.load(std::memory_order_acquire);

        if (new_rp >= new_wp &&
            waiting_for_playout_.load(std::memory_order_acquire)) {
          std::lock_guard<std::mutex> lock(pending_mutex_);
          if (waiting_for_playout_.load(std::memory_order_relaxed)) {
            playout_tsfn_.NonBlockingCall();
            playout_tsfn_.Release();
            waiting_for_playout_.store(false, std::memory_order_release);
          }
        }
      }
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

/**
 * What: captureFrame(frameDict) → Promise<void>
 *
 * Why:  Unlike onData() which is fire-and-forget (drops frames when the buffer
 *       is full), captureFrame provides backpressure. The returned Promise only
 *       resolves once the frame has been accepted into the ring buffer. This
 *       lets the caller (TTS pipeline) pace itself — await captureFrame() will
 *       naturally throttle when the buffer is full.
 *
 * Implementation:
 *   1. Parse the frame dict (same as onData).
 *   2. Try to push into the ring buffer via TryPushData.
 *   3. If successful, resolve the promise immediately.
 *   4. If the buffer is full, store the frame + a ThreadSafeFunction in
 *      pending_capture_. The drain thread will push the frame and call the
 *      TSFN to resolve the promise once space opens.
 */
Napi::Value RTCAudioSource::CaptureFrame(const Napi::CallbackInfo &info) {
  auto env = info.Env();
  auto deferred = Napi::Promise::Deferred::New(env);

  // Parse the audio frame dictionary from JS arguments
  CONVERT_ARGS_OR_THROW_AND_RETURN_NAPI(info, dict, RTCOnDataEventDict)

  // Try to push directly into the ring buffer
  if (_source->TryPushData(dict)) {
    // Frame accepted — resolve immediately and free the samples buffer
    delete[] dict.samples;
    deferred.Resolve(env.Undefined());
    return deferred.Promise();
  }

  // Ring buffer full — enqueue a PendingCapture for the drain thread.
  // Copy the frame data into the PendingCapture struct so we can free
  // the dict.samples buffer now (it was allocated by the NAPI converter).
  if (!dict.numberOfFrames.IsJust()) {
    delete[] dict.samples;
    deferred.Reject(
        Napi::Error::New(env, "captureFrame: invalid frame (no numberOfFrames)")
            .Value());
    return deferred.Promise();
  }

  const uint16_t num_frames = dict.numberOfFrames.UnsafeFromJust();
  const size_t num_samples =
      static_cast<size_t>(num_frames) * dict.channelCount;

  PendingCapture pc;
  std::memcpy(pc.data, dict.samples,
              std::min(num_samples, size_t{960}) * sizeof(int16_t));
  pc.num_samples = num_samples;
  pc.bits_per_sample = dict.bitsPerSample;
  pc.sample_rate = dict.sampleRate;
  pc.channel_count = dict.channelCount;
  pc.num_frames = num_frames;

  // Free the samples buffer now — we've copied the data
  delete[] dict.samples;

  // Create a ThreadSafeFunction that resolves the deferred promise.
  // The callback runs on the JS thread when the drain thread calls
  // tsfn.NonBlockingCall().
  pc.tsfn = Napi::ThreadSafeFunction::New(
      env,
      Napi::Function::New(env,
                          [deferred](const Napi::CallbackInfo &cbInfo) {
                            deferred.Resolve(cbInfo.Env().Undefined());
                          }),
      "captureFrameResolve", // resource name for debugging
      0,                     // max queue size (0 = unlimited)
      1                      // initial thread count
  );

  {
    std::lock_guard<std::mutex> lock(_source->pending_mutex_);
    _source->pending_capture_.push_back(std::move(pc));
  }

  return deferred.Promise();
}

/**
 * What: clearQueue() → void (synchronous)
 *
 * Why:  Immediately discards all queued audio. Used when the conversation is
 *       interrupted — stale TTS audio should not play out. Also resolves any
 *       pending captureFrame() and waitForPlayout() promises so those callers
 *       can proceed.
 */
Napi::Value RTCAudioSource::ClearQueue(const Napi::CallbackInfo &info) {
  _source->ClearBuffer();
  return info.Env().Undefined();
}

/**
 * What: queuedDurationMs getter → number
 *
 * Why:  Each frame in the ring buffer is 10ms of audio. The caller can use
 *       this to monitor buffer depth for pacing decisions or diagnostics.
 */
Napi::Value
RTCAudioSource::GetQueuedDurationMs(const Napi::CallbackInfo &info) {
  const size_t queued = _source->QueuedFrames();
  // Each frame is 10ms of audio
  return Napi::Number::New(info.Env(), static_cast<double>(queued * 10));
}

/**
 * What: waitForPlayout() → Promise<void>
 *
 * Why:  Resolves when the ring buffer drains to empty — all queued audio has
 *       been delivered to sinks. The TTS pipeline uses this to know when an
 *       utterance has finished playing (for turn-taking, end-of-speech events).
 *
 *       If the buffer is already empty, resolves immediately.
 *       Only one waitForPlayout can be active at a time (last one wins).
 */
Napi::Value RTCAudioSource::WaitForPlayout(const Napi::CallbackInfo &info) {
  auto env = info.Env();
  auto deferred = Napi::Promise::Deferred::New(env);

  // If buffer is already empty, resolve immediately
  if (_source->QueuedFrames() == 0) {
    deferred.Resolve(env.Undefined());
    return deferred.Promise();
  }

  // Release any previous playout waiter (last caller wins)
  {
    std::lock_guard<std::mutex> lock(_source->pending_mutex_);
    if (_source->waiting_for_playout_.load(std::memory_order_relaxed)) {
      _source->playout_tsfn_.Release();
    }

    _source->playout_tsfn_ = Napi::ThreadSafeFunction::New(
        env,
        Napi::Function::New(env,
                            [deferred](const Napi::CallbackInfo &cbInfo) {
                              deferred.Resolve(cbInfo.Env().Undefined());
                            }),
        "waitForPlayoutResolve", // resource name for debugging
        0,                       // max queue size (0 = unlimited)
        1                        // initial thread count
    );
    _source->waiting_for_playout_.store(true, std::memory_order_release);
  }

  return deferred.Promise();
}

void RTCAudioSource::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(
      env, "RTCAudioSource",
      {
          // Existing APIs (backward-compatible)
          InstanceMethod("createTrack", &RTCAudioSource::CreateTrack),
          InstanceMethod("onData", &RTCAudioSource::OnData),

          // LiveKit-style async APIs
          InstanceMethod("captureFrame", &RTCAudioSource::CaptureFrame),
          InstanceMethod("clearQueue", &RTCAudioSource::ClearQueue),
          InstanceAccessor("queuedDurationMs",
                           &RTCAudioSource::GetQueuedDurationMs, nullptr),
          InstanceMethod("waitForPlayout", &RTCAudioSource::WaitForPlayout),
      });

  constructor() = Napi::Persistent(func);
  constructor().SuppressDestruct();

  exports.Set("RTCAudioSource", func);
}

} // namespace node_webrtc
