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

  /** Ring buffer capacity: 50 frames = 500ms of audio absorption.
   *  Why 50: Absorbs typical JS timer jitter (0-50ms) plus occasional GC pauses
   *  (100-200ms). Low enough that latency stays acceptable for interactive voice. */
  static constexpr size_t kRingCapacity = 50;

  /**
   * What: Pre-allocated audio frame slot in the ring buffer.
   * Why:  Fixed-size allocation avoids heap allocs on the hot path. The
   *       kMaxFrameSamples array is large enough for any supported sample rate
   *       and channel count (up to 48kHz stereo).
   */
  struct CaptureFrame {
    int16_t data[kMaxFrameSamples];
    size_t num_samples = 0;
    uint8_t bits_per_sample = 16;
    uint16_t sample_rate = 48000;
    uint8_t channel_count = 1;
    uint16_t num_frames = 480;
  };

  CaptureFrame ring_buffer_[kRingCapacity];

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

  Napi::Value CreateTrack(const Napi::CallbackInfo &);
  Napi::Value OnData(const Napi::CallbackInfo &);

  rtc::scoped_refptr<RTCAudioTrackSource> _source;
  OwnedWrap<MediaStreamTrack> _track_wrap;
};

} // namespace node_webrtc
