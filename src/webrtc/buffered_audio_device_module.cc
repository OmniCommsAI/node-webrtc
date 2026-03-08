/**
 * BufferedAudioDeviceModule — Production-grade AudioDeviceModule
 * implementation.
 *
 * What: A custom WebRTC AudioDeviceModule that drives the playout/capture audio
 *       pipeline on a dedicated high-priority thread. It processes 10ms audio
 *       frames at the configured sample rate, feeding rendered audio through
 *       WebRTC's AudioTransport and forwarding it to a Renderer sink.
 *
 * Why:  The upstream webrtc::TestAudioDeviceModule cannot be used directly
 *       because it causes audio failures in node-webrtc (see
 *       https://github.com/WonderInventions/node-webrtc/issues/13). The
 *       previous TestAudioDeviceModuleImpl (in test_audio_device_module.cc)
 *       worked but had a fragile timing loop that would log-spam "ProcessAudio
 *       is too slow" under any scheduling jitter and never recovered from
 *       drift. This replacement improves timing resilience: when the processing
 *       thread falls behind wall-clock time (e.g. due to GC pauses or system
 *       load), it resets its time base instead of accumulating debt, preventing
 *       a burst of back-to-back frames that would corrupt the audio pipeline.
 *
 *       Capture (RecordedDataIsAvailable) remains intentionally disabled — it
 *       triggers a race condition inside WebRTC's AudioSendStream that causes
 *       a RaceDetected CHECK failure and process abort. Playout is kept active
 *       because disabling it breaks the ondata callback that node-webrtc
 *       relies on for receiving remote audio.
 */

#include "src/webrtc/buffered_audio_device_module.hh"

#include <algorithm>
#include <cstdlib>
#include <iosfwd>
#include <memory>
#include <type_traits>
#include <vector>

#include <absl/memory/memory.h>
#include <webrtc/common_audio/wav_file.h>
#include <webrtc/modules/audio_device/include/audio_device_default.h>
#include <webrtc/modules/audio_device/include/audio_device_defines.h>
#include <webrtc/modules/audio_device/include/test_audio_device.h>
#include <webrtc/rtc_base/buffer.h>
#include <webrtc/rtc_base/checks.h>
#include <webrtc/rtc_base/deprecated/recursive_critical_section.h>
#include <webrtc/rtc_base/event.h>
#include <webrtc/rtc_base/logging.h>
#include <webrtc/rtc_base/numerics/safe_conversions.h>
#include <webrtc/rtc_base/platform_thread.h>
#include <webrtc/rtc_base/random.h>
#include <webrtc/rtc_base/ref_counted_object.h>
#include <webrtc/rtc_base/thread.h>
#include <webrtc/rtc_base/thread_annotations.h>
#include <webrtc/rtc_base/time_utils.h>

namespace node_webrtc {

namespace {

/** 10ms frame duration in microseconds — the standard WebRTC audio frame size.
 */
constexpr int kFrameLengthUs = 10000;

/** Number of 10ms frames per second (100). */
constexpr int kFramesPerSecond = rtc::kNumMicrosecsPerSec / kFrameLengthUs;

/**
 * Maximum tolerable drift in microseconds before the time base is reset.
 *
 * Why: If the processing thread falls behind by more than 50ms (5 frames), it
 *      means scheduling was severely delayed (GC pause, CPU contention, etc.).
 *      Rather than trying to "catch up" by processing multiple frames back-to-
 *      back (which floods the audio pipeline and produces glitchy output), we
 *      reset the time base to the current wall clock. This trades a brief gap
 *      in playout for stable, non-bursty audio processing going forward.
 */
constexpr int64_t kMaxDriftUs = 50000;

/**
 * BufferedAudioDeviceModuleImpl — the concrete implementation.
 *
 * What: Inherits from AudioDeviceModuleDefault<BufferedAudioDeviceModule> which
 *       stubs out all AudioDeviceModule methods with no-op defaults, then
 *       overrides the subset we need: Init, RegisterAudioCallback,
 *       Start/StopPlayout, Start/StopRecording, Playing, Recording, and the
 *       WaitFor*End helpers.
 *
 * Why:  AudioDeviceModule has ~70 pure virtual methods. The Default template
 *       provides safe no-op stubs so we only implement what matters.
 */
class
    BufferedAudioDeviceModuleImpl // NOLINT(cppcoreguidelines-special-member-functions)
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<
          BufferedAudioDeviceModule> {
public:
  /**
   * What: Construct the module with optional capturer/renderer and speed
   * factor. Why:  Capturer and renderer are injected to allow different audio
   * sources and sinks (e.g. ZeroCapturer for silence, or a real audio buffer).
   *       Speed factor allows faster-than-realtime processing for tests.
   */
  BufferedAudioDeviceModuleImpl(
      std::unique_ptr<webrtc::TestAudioDeviceModule::Capturer> capturer,
      std::unique_ptr<webrtc::TestAudioDeviceModule::Renderer> renderer,
      float speed = 1)
      : capturer_(std::move(capturer)), renderer_(std::move(renderer)),
        process_interval_us_(static_cast<int64_t>(kFrameLengthUs / speed)),
        done_rendering_(true, true), done_capturing_(true, true) {
    auto good_sample_rate = [](auto sr) {
      return sr == 8000 || sr == 16000 || sr == 32000 || sr == 44100 ||
             sr == 48000;
    };

    if (renderer_) {
      const int sample_rate = renderer_->SamplingFrequency();
      playout_buffer_.resize(
          SamplesPerFrame(sample_rate) * renderer_->NumChannels(), 0);
      RTC_CHECK(good_sample_rate(sample_rate));
    }
    if (capturer_) {
      RTC_CHECK(good_sample_rate(capturer_->SamplingFrequency()));
    }
  }

  ~BufferedAudioDeviceModuleImpl() override {
    StopPlayout();   // NOLINT
    StopRecording(); // NOLINT
    if (thread_) {
      {
        rtc::CritScope cs(&lock_);
        stop_thread_ = true;
      }
      thread_->Finalize();
    }
  }

  /**
   * What: Spawns the audio processing thread at high priority.
   * Why:  Audio processing must run on a dedicated thread with elevated
   *       priority to minimize scheduling latency and maintain the 10ms
   *       cadence required by the WebRTC audio pipeline.
   */
  int32_t Init() override {
    thread_ = absl::make_unique<rtc::PlatformThread>(
        rtc::PlatformThread::SpawnJoinable(
            [this]() { BufferedAudioDeviceModuleImpl::Run(this); },
            "BufferedAudioDeviceModule",
            rtc::ThreadAttributes{rtc::ThreadPriority::kHigh}));
    return 0;
  }

  int32_t RegisterAudioCallback(webrtc::AudioTransport *callback) override {
    rtc::CritScope cs(&lock_);
    RTC_DCHECK(callback || audio_callback_);
    audio_callback_ = callback;
    return 0;
  }

  int32_t StartPlayout() override {
    rtc::CritScope cs(&lock_);
    RTC_CHECK(renderer_);
    rendering_ = true;
    done_rendering_.Reset();
    return 0;
  }

  int32_t StopPlayout() override {
    rtc::CritScope cs(&lock_);
    rendering_ = false;
    done_rendering_.Set();
    return 0;
  }

  int32_t StartRecording() override {
    rtc::CritScope cs(&lock_);
    RTC_CHECK(capturer_);
    capturing_ = true;
    done_capturing_.Reset();
    return 0;
  }

  int32_t StopRecording() override {
    rtc::CritScope cs(&lock_);
    capturing_ = false;
    done_capturing_.Set();
    return 0;
  }

  bool Playing() const override {
    rtc::CritScope cs(&lock_);
    return rendering_;
  }

  bool Recording() const override {
    rtc::CritScope cs(&lock_);
    return capturing_;
  }

  /** Blocks until the Renderer refuses to receive data. */
  bool WaitForPlayoutEnd(int timeout_ms = rtc::Event::kForever) override {
    return done_rendering_.Wait(timeout_ms);
  }

  /** Blocks until the Recorder stops producing data. */
  bool WaitForRecordingEnd(int timeout_ms = rtc::Event::kForever) override {
    return done_capturing_.Wait(timeout_ms);
  }

private:
  /**
   * What: Main audio processing loop — runs on the dedicated thread, processes
   *       one 10ms frame per iteration, and sleeps until the next frame is due.
   *
   * Why:  WebRTC's audio pipeline expects NeedMorePlayData to be called at a
   *       steady 10ms cadence. This loop maintains an absolute time target and
   *       sleeps for the remaining interval after each frame. If the thread
   *       falls behind wall-clock time by more than kMaxDriftUs (50ms), it
   *       resets the time base rather than trying to catch up — this prevents
   *       bursty back-to-back frame processing that would corrupt audio output.
   *       The original TestAudioDeviceModuleImpl would log-spam "ProcessAudio
   *       is too slow" and never recover; this implementation handles drift
   *       silently and gracefully.
   */
  void ProcessAudio() {
    int64_t time_us = rtc::TimeMicros();

    for (;;) {
      {
        rtc::CritScope cs(&lock_);
        if (stop_thread_) {
          return;
        }

        // Capture is intentionally disabled. Calling
        // RecordedDataIsAvailable() races with AudioSendStream internals
        // and triggers a fatal RaceDetected CHECK failure:
        //
        //   Fatal error in: ../../download/src/audio/audio_send_stream.cc,
        //   line 330 — Check failed: !race_checker.RaceDetected()
        //
        // This has been broken since the original node-webrtc fork and is
        // not needed — node-webrtc uses a separate capture path.
        /*
        if (capturing_) {
          const bool keep_capturing = capturer_->Capture(&recording_buffer_);
          uint32_t new_mic_level = 0;
          audio_callback_->RecordedDataIsAvailable(
              recording_buffer_.data(), recording_buffer_.size(), 2,
              capturer_->NumChannels(), capturer_->SamplingFrequency(), 0, 0,
              0, false, new_mic_level);
          if (!keep_capturing) {
            capturing_ = false;
            done_capturing_.Set();
          }
        }
        */

        if (rendering_) {
          size_t samples_out = 0;
          int64_t elapsed_time_ms = -1;
          int64_t ntp_time_ms = -1;

          // NeedMorePlayData pulls decoded audio from the WebRTC pipeline.
          // Without this call, the ondata callback that node-webrtc uses to
          // deliver remote audio to JavaScript never fires.
          const int sampling_frequency = renderer_->SamplingFrequency();
          if (audio_callback_) {
            audio_callback_->NeedMorePlayData(
                SamplesPerFrame(sampling_frequency), 2,
                renderer_->NumChannels(), sampling_frequency,
                playout_buffer_.data(), samples_out, &elapsed_time_ms,
                &ntp_time_ms);
          }

          const bool keep_rendering =
              renderer_->Render(rtc::ArrayView<const int16_t>(
                  playout_buffer_.data(), samples_out));
          if (!keep_rendering) {
            rendering_ = false;
            done_rendering_.Set();
          }
        }
      }

      // Advance the absolute target time by one frame interval.
      time_us += process_interval_us_;

      int64_t time_left_us = time_us - rtc::TimeMicros();

      if (time_left_us < -kMaxDriftUs) {
        // We've fallen behind by more than kMaxDriftUs. This means scheduling
        // was severely disrupted (GC pause, CPU spike, etc.). Reset the time
        // base to now rather than trying to catch up — catching up would mean
        // processing multiple frames back-to-back with no sleep, which floods
        // the audio pipeline and produces glitchy output.
        time_us = rtc::TimeMicros();
      } else if (time_left_us > 0) {
        // Normal case: sleep until the next frame is due.
        while (time_left_us > 1000) {
          if (rtc::Thread::SleepMs(static_cast<int>(time_left_us / 1000))) {
            break;
          }
          time_left_us = time_us - rtc::TimeMicros();
        }
      }
      // If time_left_us is between -kMaxDriftUs and 0, we're slightly behind
      // but within tolerance — proceed immediately to the next frame to catch
      // up naturally without resetting. No log spam needed.
    }
  }

  static void Run(void *obj) {
    static_cast<BufferedAudioDeviceModuleImpl *>(obj)->ProcessAudio();
  }

  const std::unique_ptr<webrtc::TestAudioDeviceModule::Capturer>
      capturer_ RTC_GUARDED_BY(lock_);
  const std::unique_ptr<webrtc::TestAudioDeviceModule::Renderer>
      renderer_ RTC_GUARDED_BY(lock_);
  const int64_t process_interval_us_;

  mutable rtc::RecursiveCriticalSection lock_;
  webrtc::AudioTransport *audio_callback_ RTC_GUARDED_BY(lock_) = nullptr;
  bool rendering_ RTC_GUARDED_BY(lock_) = false;
  bool capturing_ RTC_GUARDED_BY(lock_) = false;
  rtc::Event done_rendering_;
  rtc::Event done_capturing_;

  std::vector<int16_t> playout_buffer_ RTC_GUARDED_BY(lock_);
  rtc::BufferT<int16_t> recording_buffer_ RTC_GUARDED_BY(lock_);

  std::unique_ptr<rtc::PlatformThread> thread_;
  bool stop_thread_ RTC_GUARDED_BY(lock_) = false;
};

/**
 * What: A capturer that produces silence (all-zero PCM samples).
 * Why:  Used as a default capturer when no real audio source is needed.
 *       WebRTC requires a non-null capturer to start recording, so this
 *       provides a minimal implementation that satisfies the interface.
 */
class ZeroCapturerImpl final : public webrtc::TestAudioDeviceModule::Capturer {
public:
  ZeroCapturerImpl(int sampling_frequency_in_hz, int num_channels)
      : sampling_frequency_in_hz_(sampling_frequency_in_hz),
        num_channels_(num_channels) {}

  int SamplingFrequency() const override { return sampling_frequency_in_hz_; }

  int NumChannels() const override { return num_channels_; }

  bool Capture(rtc::BufferT<int16_t> *buffer) override {
    buffer->SetData(
        BufferedAudioDeviceModule::SamplesPerFrame(sampling_frequency_in_hz_) *
            num_channels_,
        [&](rtc::ArrayView<int16_t> data) {
          std::fill(data.begin(), data.end(), 0);
          return data.size();
        });
    return true;
  }

private:
  int sampling_frequency_in_hz_;
  const int num_channels_;
};

} // namespace

size_t
BufferedAudioDeviceModule::SamplesPerFrame(int sampling_frequency_in_hz) {
  return rtc::CheckedDivExact(sampling_frequency_in_hz, kFramesPerSecond);
}

rtc::scoped_refptr<BufferedAudioDeviceModule> BufferedAudioDeviceModule::Create(
    std::unique_ptr<webrtc::TestAudioDeviceModule::Capturer> capturer,
    std::unique_ptr<webrtc::TestAudioDeviceModule::Renderer> renderer,
    float speed) {
  return new rtc::RefCountedObject<BufferedAudioDeviceModuleImpl>(
      std::move(capturer), std::move(renderer), speed);
}

std::unique_ptr<webrtc::TestAudioDeviceModule::Capturer>
BufferedAudioDeviceModule::CreateZeroCapturer(int sampling_frequency_in_hz,
                                              int num_channels) {
  return std::make_unique<ZeroCapturerImpl>(sampling_frequency_in_hz,
                                            num_channels);
}

} // namespace node_webrtc
