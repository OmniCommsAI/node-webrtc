/* Copyright (c) 2019 The node-webrtc project authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be found
 * in the LICENSE.md file in the root of the source tree. All contributing
 * project authors may be found in the AUTHORS file in the root of the source
 * tree.
 */

/**
 * RTCAudioSource — JavaScript-accessible audio source for WebRTC tracks.
 *
 * What: Provides the JS onData() API for pushing audio frames into a WebRTC
 *       audio track. Frames are buffered in a lock-free ring buffer and drained
 *       to WebRTC sinks at a steady 10ms cadence by a dedicated high-priority
 *       native thread.
 *
 * Why:  The original implementation pushed audio frames directly to sinks from
 *       the JS thread (onData → PushData → sink->OnData). Since JS event loop
 *       timing is unreliable (setInterval jitter of 0-50ms due to GC, microtask
 *       queue, libuv poll latency), the audio sinks received frames at irregular
 *       intervals — causing underruns (silence/drops) and overruns (bursty audio)
 *       in the outbound WebRTC stream.
 *
 *       By inserting a lock-free SPSC ring buffer between the JS push and sink
 *       delivery, we decouple production from consumption:
 *         - Producer (JS thread): writes frames to the ring buffer at whatever
 *           rate it can manage. Never blocks, never contends with the consumer.
 *         - Consumer (native drain thread): reads frames from the ring buffer at
 *           a steady 10ms cadence and delivers to WebRTC sinks. JS timing jitter
 *           is fully absorbed by the buffer depth.
 *
 *       This is the same architecture used by LiveKit's audio pipeline: JS pushes
 *       freely, a native thread drains at real-time pace.
 *
 * LiveKit-style async APIs (captureFrame, waitForPlayout, clearQueue,
 * queuedDurationMs) added to support backpressure-aware producers that need to
 * know when the ring buffer has space or has fully drained.
 */
#pragma once

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <node-addon-api/napi.h>
#include <webrtc/api/media_stream_interface.h>
#include <webrtc/api/scoped_refptr.h>
#include <webrtc/pc/local_audio_source.h>

#include "src/dictionaries/node_webrtc/rtc_on_data_event_dict.hh"
#include "src/interfaces/media_stream_track.hh"
#include "src/interfaces/rtc_peer_connection/peer_connection_factory.hh"
#include "src/node/wrap.hh"

// Forward declarations for WebRTC threading primitives
namespace rtc {
class PlatformThread;
class Event;
}

namespace node_webrtc {

/**
 * PendingCapture — a frame + deferred promise waiting for ring buffer space.
 *
 * What: When captureFrame() is called but the ring buffer is full, we store
 *       the frame data and a ThreadSafeFunction that will resolve the JS
 *       promise on the main thread once the drain thread makes space.
 *
 * Why:  The drain thread cannot touch JS objects directly (wrong thread).
 *       ThreadSafeFunction is the NAPI mechanism for calling back to the JS
 *       thread from a native thread. The drain thread calls the TSFN after
 *       consuming a frame, which schedules resolution on the JS event loop.
 */
struct PendingCapture {
  int16_t data[960]; // kMaxFrameSamples
  size_t num_samples;
  uint8_t bits_per_sample;
  uint16_t sample_rate;
  uint8_t channel_count;
  uint16_t num_frames;
  Napi::ThreadSafeFunction tsfn;
};

/**
 * RTCAudioTrackSource — local audio source with ring-buffered sink delivery.
 *
 * What: A WebRTC LocalAudioSource that buffers audio frames pushed from JS
 *       and delivers them to registered sinks at a steady 10ms cadence.
 *
 * Why:  Direct JS→sink delivery is timing-sensitive. The ring buffer absorbs
 *       JS event loop jitter (typically 0-50ms) so sinks always receive
 *       frames at the correct rate regardless of when JS pushes them.
 *
 * Thread model:
 *   - JS main thread (producer): calls PushData() which writes to ring buffer.
 *     Lock-free write via std::atomic — never blocks.
 *   - Drain thread (consumer): reads from ring buffer every 10ms and calls
 *     sink->OnData(). Uses std::shared_mutex for sink list access.
 *   - Single Producer, Single Consumer (SPSC) — no locks on the ring buffer
 *     itself, only acquire/release atomics.
 */
class RTCAudioTrackSource : public webrtc::LocalAudioSource {
public:
  RTCAudioTrackSource(const RTCAudioTrackSource &) = delete;
  RTCAudioTrackSource(RTCAudioTrackSource &&) = delete;
  RTCAudioTrackSource &operator=(const RTCAudioTrackSource &) = delete;
  RTCAudioTrackSource &operator=(RTCAudioTrackSource &&) = delete;
  RTCAudioTrackSource() = default;
  ~RTCAudioTrackSource() override;

  [[nodiscard]] SourceState state() const override {
    return webrtc::MediaSourceInterface::SourceState::kLive;
  }

  [[nodiscard]] bool remote() const override { return false; }

  /**
   * What: Push audio data from JS into the ring buffer for paced delivery.
   *
   * Why:  Instead of immediately forwarding to sinks (original behavior that
   *       caused timing-dependent audio drops), data is written to a lock-free
   *       ring buffer. The native drain thread reads at 10ms intervals.
   *
   *       If the ring buffer is full (JS pushing faster than real-time, e.g.
   *       TTS burst), the oldest unread frame is overwritten. This is preferable
   *       to blocking the JS thread or dropping the newest frame, as it keeps
   *       the audio stream flowing with minimal audible artifact.
   */
  void PushData(RTCOnDataEventDict dict);

  /**
   * What: Try to push a frame into the ring buffer, returning true if it fit.
   *
   * Why:  Unlike PushData (fire-and-forget, drops on overflow), this variant
   *       tells the caller whether the frame was actually written. Used by
   *       CaptureFrame to decide whether to resolve the promise immediately
   *       or enqueue a PendingCapture for later resolution by the drain thread.
   */
  bool TryPushData(RTCOnDataEventDict &dict);

  /**
   * What: Reset the ring buffer positions to zero, effectively clearing all
   *       queued audio.
   *
   * Why:  Needed when the caller wants to discard stale audio (e.g. after an
   *       interruption or when switching audio sources). Also resolves any
   *       pending capture promises and the playout waiter, since the queue
   *       is now empty.
   */
  void ClearBuffer();

  /**
   * What: Returns the number of unread frames in the ring buffer.
   *
   * Why:  Allows the NAPI wrapper to compute queuedDurationMs without knowing
   *       the ring buffer internals.
   */
  size_t QueuedFrames() const;

  void AddSink(webrtc::AudioTrackSinkInterface *sink) override {
    std::unique_lock<std::shared_mutex> lock{_sinks_mutex};
    _sinks.push_back(sink);
  }

  void RemoveSink(webrtc::AudioTrackSinkInterface *sink) override {
    std::unique_lock<std::shared_mutex> lock{_sinks_mutex};
    auto it = std::find(_sinks.begin(), _sinks.end(), sink);

    if (it != _sinks.end()) {
      _sinks.erase(it);
    }
  }

  // ── Pending Capture / Playout Waiter State ──────────────────────────────
  //
  // What: State for the async captureFrame() and waitForPlayout() APIs.
  //
  // Why:  These APIs return JS Promises that resolve from the native drain
  //       thread. The drain thread cannot touch V8 directly, so we use
  //       ThreadSafeFunctions as the bridge. The mutex protects the pending
  //       lists — it is held only briefly (push/pop), never during sleep.

  /** Protects pending_capture_ and playout_tsfn_. */
  std::mutex pending_mutex_;

  /** Frames + promise callbacks waiting for ring buffer space. */
  std::vector<PendingCapture> pending_capture_;

  /**
   * ThreadSafeFunction for the waitForPlayout() promise.
   * Non-null only when someone is actively waiting.
   */
  Napi::ThreadSafeFunction playout_tsfn_;

  /** Whether a waitForPlayout() call is outstanding. */
  std::atomic<bool> waiting_for_playout_{false};

private:
  PeerConnectionFactory *_factory = PeerConnectionFactory::GetOrCreateDefault();

  std::shared_mutex _sinks_mutex;
  std::vector<webrtc::AudioTrackSinkInterface *> _sinks;

  // ── Ring Buffer ──────────────────────────────────────────────────────────
  //
  // Lock-free SPSC (Single Producer Single Consumer) ring buffer.
  //
  // What: Fixed-size circular buffer of pre-allocated audio frames. The JS
  //       thread (producer) writes frames at write_pos_, the drain thread
  //       (consumer) reads at read_pos_. Positions use monotonically increasing
  //       counters — the actual slot is pos % capacity.
  //
  // Why:  Lock-free because the producer and consumer never access the same
  //       slot simultaneously (SPSC guarantee). std::atomic with acquire/release
  //       semantics ensures correct memory ordering across threads without
  //       mutex overhead — critical for real-time audio (10ms budget per frame).

  /** Maximum samples in a single 10ms frame: 48kHz * 2ch * 10ms = 960 */
  static constexpr size_t kMaxFrameSamples = 960;

  /** Ring buffer capacity: 100 frames = 1000ms of audio absorption.
   *  Why 100: Matches LiveKit's default queue_size_ms=1000. Absorbs JS event loop
   *  jitter (0-50ms), GC pauses (100-200ms), and bursty TTS frame delivery.
   *  The drain thread still delivers at a steady 10ms cadence — buffer depth
   *  doesn't add latency under normal conditions, only absorbs bursts. */
  static constexpr size_t kRingCapacity = 100;

  /**
   * What: Pre-allocated audio frame slot in the ring buffer.
   * Why:  Fixed-size allocation avoids heap allocs on the hot path. The
   *       kMaxFrameSamples array is large enough for any supported sample rate
   *       and channel count (up to 48kHz stereo).
   */
  struct RingFrame {
    int16_t data[kMaxFrameSamples];
    size_t num_samples = 0;
    uint8_t bits_per_sample = 16;
    uint16_t sample_rate = 48000;
    uint8_t channel_count = 1;
    uint16_t num_frames = 480;
  };

  RingFrame ring_buffer_[kRingCapacity];

  /** Producer position — only written by JS thread (PushData). */
  std::atomic<size_t> write_pos_{0};

  /** Consumer position — only written by drain thread (DrainLoop). */
  std::atomic<size_t> read_pos_{0};

  /** Signal to stop the drain thread during destruction. */
  std::atomic<bool> stop_drain_{false};

  /** Whether the drain thread has been started. Lazily initialized on first PushData. */
  std::atomic<bool> drain_started_{false};

  /** The dedicated drain thread — reads ring buffer at 10ms intervals. */
  std::unique_ptr<rtc::PlatformThread> drain_thread_;

  /** Start the drain thread (called once, lazily from PushData). */
  void StartDrainThread();

  /**
   * What: Ensure the drain thread is running, starting it if needed.
   *
   * Why:  Both PushData and TryPushData need to lazily start the drain thread.
   *       Extracted to avoid duplicating the compare_exchange logic.
   */
  void EnsureDrainThread();

  /**
   * What: Main drain loop — runs on a dedicated high-priority native thread.
   *       Reads one frame from the ring buffer every 10ms and delivers to all
   *       registered WebRTC audio sinks.
   *
   * Why:  This is the core of the timing fix. By delivering frames at a steady
   *       10ms cadence from a native thread, we decouple sink delivery from JS
   *       event loop timing. The ring buffer absorbs any jitter in JS's push
   *       rate, and the native thread provides deterministic pacing.
   *
   *       Uses absolute time tracking with drift compensation (same technique
   *       as ProcessAudio in BufferedAudioDeviceModule). If the thread falls
   *       behind by more than 50ms, it resets rather than bursting.
   *
   *       After consuming each frame, checks for:
   *       1. Pending captureFrame() promises — pushes their frame data and
   *          resolves the promise via ThreadSafeFunction.
   *       2. waitForPlayout() — if the buffer is now empty and someone is
   *          waiting, resolves the playout promise via ThreadSafeFunction.
   */
  void DrainLoop();

  /** Static trampoline for PlatformThread::SpawnJoinable. */
  static void RunDrain(void* obj) {
    static_cast<RTCAudioTrackSource*>(obj)->DrainLoop();
  }
};

class RTCAudioSource : public Napi::ObjectWrap<RTCAudioSource> {
public:
  RTCAudioSource(const Napi::CallbackInfo &);

  static void Init(Napi::Env, Napi::Object);

private:
  static Napi::FunctionReference &constructor();

  // ── Existing APIs (backward-compatible) ─────────────────────────────────

  Napi::Value CreateTrack(const Napi::CallbackInfo &);
  Napi::Value OnData(const Napi::CallbackInfo &);

  // ── LiveKit-style async APIs ────────────────────────────────────────────
  //
  // What: Backpressure-aware audio source APIs modeled after LiveKit's
  //       AudioSource (captureFrame, clearQueue, queuedDurationMs, waitForPlayout).
  //
  // Why:  The original onData() is fire-and-forget — the caller has no way to
  //       know if the ring buffer accepted the frame or if it's full. For TTS
  //       playback, the caller needs:
  //       - captureFrame(): know when the frame was accepted (backpressure)
  //       - clearQueue(): discard stale audio on interruption
  //       - queuedDurationMs: monitor buffer depth for pacing decisions
  //       - waitForPlayout(): know when all queued audio has been delivered
  //         (e.g. to detect end-of-utterance)

  /**
   * What: Push a frame and return a Promise that resolves when the frame is
   *       accepted into the ring buffer.
   *
   * Why:  If the ring buffer has space, resolves immediately. If full, the
   *       promise is held until the drain thread consumes a frame and makes
   *       space, at which point the pending frame is pushed and the promise
   *       resolved via ThreadSafeFunction.
   */
  Napi::Value CaptureFrame(const Napi::CallbackInfo &);

  /**
   * What: Synchronously clear all queued audio from the ring buffer.
   *
   * Why:  Used on interruption — discard stale TTS audio immediately so the
   *       new response can start without waiting for old audio to drain.
   *       Also resolves any pending captureFrame/waitForPlayout promises.
   */
  Napi::Value ClearQueue(const Napi::CallbackInfo &);

  /**
   * What: Getter that returns the duration of audio currently queued (ms).
   *
   * Why:  Allows the JS caller to make pacing decisions (e.g. "don't push
   *       more frames if there's already 200ms queued").
   */
  Napi::Value GetQueuedDurationMs(const Napi::CallbackInfo &);

  /**
   * What: Return a Promise that resolves when the ring buffer drains to empty.
   *
   * Why:  The caller (TTS pipeline) needs to know when all audio has been
   *       delivered to sinks — e.g. to detect end-of-utterance for turn-taking.
   *       If the buffer is already empty, resolves immediately.
   */
  Napi::Value WaitForPlayout(const Napi::CallbackInfo &);

  rtc::scoped_refptr<RTCAudioTrackSource> _source;
  OwnedWrap<MediaStreamTrack> _track_wrap;
};

} // namespace node_webrtc
