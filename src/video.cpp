/**
 * @file src/video.cpp
 * @brief Definitions for video.
 */
// standard includes
#include <atomic>
#include <bitset>
#include <list>
#include <thread>

// lib includes
#include <boost/pointer_cast.hpp>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// local includes
#include "cbs.h"
#include "config.h"
#include "display_device.h"
#include "logging.h"
#include "platform/common.h"
#include "sync.h"
#include "video.h"

#ifdef _WIN32
extern "C" {
  #include <libavutil/hwcontext_d3d11va.h>
}
#endif

#include "video_capture_session.h"
#include "stream.h"
using namespace std::literals;

namespace video {

  namespace {
    /**
     * @brief Check if we can allow probing for the encoders.
     * @return True if there should be no issues with the probing, false if we should prevent it.
     */
    bool allow_encoder_probing() {
      const auto devices {display_device::enumerate_devices()};

      // If there are no devices, then either the API is not working correctly or OS does not support the lib.
      // Either way we should not block the probing in this case as we can't tell what's wrong.
      if (devices.empty()) {
        return true;
      }

      // Since Windows 11 24H2, it is possible that there will be no active devices present
      // for some reason (probably a bug). Trying to probe encoders in such a state locks/breaks the DXGI
      // and also the display device for Windows. So we must have at least 1 active device.
      const bool at_least_one_device_is_active = std::any_of(std::begin(devices), std::end(devices), [](const auto &device) {
        // If device has additional info, it is active.
        return static_cast<bool>(device.m_info);
      });

      if (at_least_one_device_is_active) {
        return true;
      }

      BOOST_LOG(error) << "No display devices are active at the moment! Cannot probe the encoders.";
      return false;
    }
  }  // namespace

  void free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);
  }

  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  void free_buffer(AVBufferRef *ref) {
    av_buffer_unref(&ref);
  }

  namespace nv {

    enum class profile_h264_e : int {
      high = 2,  ///< High profile
      high_444p = 3,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 0,  ///< Main profile
      main_10 = 1,  ///< Main 10 profile
      rext = 2,  ///< Rext profile
    };

  }  // namespace nv

  namespace qsv {

    enum class profile_h264_e : int {
      high = 100,  ///< High profile
      high_444p = 244,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 1,  ///< Main profile
      main_10 = 2,  ///< Main 10 profile
      rext = 4,  ///< RExt profile
    };

    enum class profile_av1_e : int {
      main = 1,  ///< Main profile
      high = 2,  ///< High profile
    };

  }  // namespace qsv

  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);

#ifdef _WIN32
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_nvenc>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::ayuv,
      platf::pix_fmt_e::yuv444p16
    ),
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION | YUV444_SUPPORT | ASYNC_TEARDOWN  // flags
  };
#elif !defined(__APPLE__)
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
  #ifdef _WIN32
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
  #else
      AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_CUDA,
  #endif
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
  #ifdef _WIN32
      dxgi_init_avcodec_hardware_input_buffer
  #else
      cuda_init_avcodec_hardware_input_buffer
  #endif
    ),
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, (int) nv::profile_hevc_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) nv::profile_hevc_e::main_10},
      },
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"coder"s, &config::video.nv_legacy.h264_coder},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, (int) nv::profile_h264_e::high},
      },
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING
  };
#endif

#ifdef _WIN32
  encoder_t quicksync {
    "quicksync"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_QSV,
      AV_PIX_FMT_QSV,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_VUYX,
      AV_PIX_FMT_XV30,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::main},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::high},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::high},
      },
      {},  // Fallback options
      "av1_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"pic_timing_sei"s, 0},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::main_10},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::rext},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::rext},
      },
      {
        // Fallback options
        {"low_power"s, []() {
           return config::video.qsv.qsv_slow_hevc ? 0 : 1;
         }},
      },
      "hevc_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"cavlc"s, &config::video.qsv.qsv_cavlc},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"vcm"s, 1},
        {"pic_timing_sei"s, 0},
        {"max_dec_frame_buffering"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_h264_e::high},
      },
      {},  // HDR-specific options
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_h264_e::high_444p},
      },
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Some old/low-end Intel GPUs don't support low power encoding
      },
      "h264_qsv"s,
    },
    PARALLEL_ENCODING | CBR_WITH_VBR | RELAXED_COMPLIANCE | NO_RC_BUF_LIMIT | YUV444_SUPPORT
  };

  encoder_t amdvce {
    "amdvce"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, "lowest_latency"s},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_av1},
        {"rc"s, &config::video.amd.amd_rc_av1},
        {"usage"s, &config::video.amd.amd_usage_av1},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"gops_per_idr"s, 1},
        {"header_insertion_mode"s, "idr"s},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_hevc},
        {"rc"s, &config::video.amd.amd_rc_hevc},
        {"usage"s, &config::video.amd.amd_usage_hevc},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
        {"level"s, [](const config_t &cfg) {
           auto size = cfg.width * cfg.height;
           // For 4K and below, try to use level 5.1 or 5.2 if possible
           if (size <= 8912896) {
             if (size * cfg.framerate <= 534773760) {
               return "5.1"s;
             } else if (size * cfg.framerate <= 1069547520) {
               return "5.2"s;
             }
           }
           return "auto"s;
         }},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"frame_skipping"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_h264},
        {"rc"s, &config::video.amd.amd_rc_h264},
        {"usage"s, &config::video.amd.amd_usage_h264},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"usage"s, 2 /* AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY */},  // Workaround for https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/410
      },
      "h264_amf"s,
    },
    PARALLEL_ENCODING
  };
#endif

  encoder_t software {
    "software"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_NONE,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_YUV420P,
      AV_PIX_FMT_YUV420P10,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P10,
      nullptr
    ),
    {
      // libsvtav1 takes different presets than libx264/libx265.
      // We set an infinite GOP length, use a low delay prediction structure,
      // force I frames to be key frames, and set max bitrate to default to work
      // around a FFmpeg bug with CBR mode.
      {
        {"svtav1-params"s, "keyint=-1:pred-struct=1:force-key-frames=1:mbr=0"s},
        {"preset"s, &config::video.sw.svtav1_preset},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options

#ifdef ENABLE_BROKEN_AV1_ENCODER
           // Due to bugs preventing on-demand IDR frames from working and very poor
           // real-time encoding performance, we do not enable libsvtav1 by default.
           // It is only suitable for testing AV1 until the IDR frame issue is fixed.
      "libsvtav1"s,
#else
      {},
#endif
    },
    {
      // x265's Info SEI is so long that it causes the IDR picture data to be
      // kicked to the 2nd packet in the frame, breaking Moonlight's parsing logic.
      // It also looks like gop_size isn't passed on to x265, so we have to set
      // 'keyint=-1' in the parameters ourselves.
      {
        {"forced-idr"s, 1},
        {"x265-params"s, "info=0:keyint=-1"s},
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx265"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx264"s,
    },
    H264_ONLY | PARALLEL_ENCODING | ALWAYS_REPROBE | YUV444_SUPPORT
  };

#ifdef __linux__
  encoder_t vaapi {
    "vaapi"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VAAPI,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vaapi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vaapi"s,
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vaapi"s,
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vaapi"s,
    },
    // RC buffer size will be set in platform code if supported
    LIMITED_GOP_SIZE | PARALLEL_ENCODING | NO_RC_BUF_LIMIT
  };
#endif

#ifdef __APPLE__
  encoder_t videotoolbox {
    "videotoolbox"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VIDEOTOOLBOX,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vt_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_videotoolbox"s,
    },
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_videotoolbox"s,
    },
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"flags"s, "-low_delay"},
      },
      "h264_videotoolbox"s,
    },
    DEFAULT
  };
#endif

  static const std::vector<encoder_t *> encoders {
#ifndef __APPLE__
    &nvenc,
#endif
#ifdef _WIN32
    &quicksync,
    &amdvce,
#endif
#ifdef __linux__
    &vaapi,
#endif
#ifdef __APPLE__
    &videotoolbox,
#endif
    &software
  };

  static encoder_t *chosen_encoder;
  int active_hevc_mode;
  int active_av1_mode;
  bool last_encoder_probe_supported_ref_frames_invalidation = false;
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {};

  /**
   * 异步采样逻辑，我们需要重构的地方
   */
  void capture_async(safe::mail_t mail, std::vector<std::shared_ptr<config_t>>& configs,void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);  // 获取退出事件
    BOOST_LOG(info) << "capture_thread_async start============================>"sv;
    input::touch_ports touchPorts;
    auto  sessions= stream::session::getCaptureSessions((stream::session_t*)channel_data);
    auto lg = util::fail_guard([&]() {  // 线程故障保护，出错了，直接暂停采样，并退出
      for (auto & session : sessions) {
        session->getContext()->capture_ctx_queue->stop();
      }
      shutdown_event->raise(true);
    });
    for (int i=0;i<sessions.size();i++) {
      auto images = std::make_shared<img_event_t::element_type>();
      sessions[i]->addCaptureContext(images,*configs[i].get());
      sessions[i]->start_capture_async(chosen_encoder);
      sessions[i]->start_encode_async(std::ref(mail), std::ref(touchPorts),std::ref(channel_data));
    }
    auto close_window_event = mail::man->event<short>(mail::close_window);  // 获取窗口关闭事件
    while (!shutdown_event->peek()) {  // 等待关闭事件
      if(close_window_event->peek()) {
        short displayIndex=*close_window_event->pop();
        BOOST_LOG(info) << "触发窗口关闭事件 ============================>"sv <<displayIndex;
        if(sessions.size()>1) { //超过一个窗口才需要处理，只有一个直接关闭即可
          int index=-1;
          for (int i = 0; i < sessions.size(); ++i) {
            if(sessions[i]->sessionIndex==displayIndex){
              sessions[i]->getContext()->encodeRunning=false;
              sessions[i]->end_capture_async();
              sessions[i]->end_encode_async();
              DisplayControl::removePort(touchPorts,displayIndex);
              mail->event<input::touch_ports>(mail::touch_port)->raise(touchPorts);
              BOOST_LOG(info) << "键盘触控区域已经更新 ============================>"sv <<displayIndex;
              index=i;
            }
          }
          if(index>=0){
            auto session=*sessions.erase(sessions.begin()+index,sessions.begin()+index+1);
          }
        }
      }
      std::this_thread::sleep_for(20ms);
    }
    for (auto & session : sessions) {
      session->getContext()->encodeRunning=false;
      session->end_capture_async();
      session->end_encode_async();
    }
    sessions.clear();
    BOOST_LOG(info) << "encode_thread all stop ============================>"sv;
  }

  void startCapture(safe::mail_t mail,std::vector<std::shared_ptr<config_t>>& configs,void *channel_data) {
    /*重新设计异步采样
       1、所有的控制事件都是全局的
       2、所有的判断是否运行也是全局的
       3、我们只是修改一下屏幕采样和结果编码，这2个需要是在2个队列里的
     */
    auto packets = mail::man->queue<packet_t>(mail::video_packets);//先申请一下队列
    if (chosen_encoder->flags & PARALLEL_ENCODING) {  // 当前sunshine的截图采样有点问题，我们需要改造一下
      capture_async(std::move(mail), configs,channel_data);  // 异步
    } else {  // 同步，暂不支持多个配置，会阻塞
      //   safe::signal_t join_event;
      //   auto ref = capture_thread_sync.ref();
      //   ref->encode_session_ctx_queue.raise(sync_session_ctx_t {
      //     &join_event,
      //     mail->event<bool>(mail::shutdown),
      //     mail::man->queue<packet_t>(mail::video_packets),
      //     std::move(idr_events),
      //     mail->event<hdr_info_t>(mail::hdr),
      //     mail->event<input::touch_port_t>(mail::touch_port),
      //     config,
      //     1,
      //     channel_data,
      //   });
      //   // Wait for join signal
      //   join_event.view();
    }
  }

  int probe_encoders() {
    if (!allow_encoder_probing()) {
      // Error already logged
      return -1;
    }

    auto encoder_list = encoders;

    // If we already have a good encoder, check to see if another probe is required
    if (chosen_encoder && !(chosen_encoder->flags & ALWAYS_REPROBE) && !platf::needs_encoder_reenumeration()) {
      return 0;
    }

    // Restart encoder selection
    auto previous_encoder = chosen_encoder;
    chosen_encoder = nullptr;
    active_hevc_mode = config::video.hevc_mode;
    active_av1_mode = config::video.av1_mode;
    last_encoder_probe_supported_ref_frames_invalidation = false;

    auto adjust_encoder_constraints = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 2 && !encoder->hevc[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC on this system"sv;
        active_hevc_mode = 0;
      }

      if (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 on this system"sv;
        active_av1_mode = 0;
      } else if (active_av1_mode == 2 && !encoder->av1[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 on this system"sv;
        active_av1_mode = 0;
      }
    };

    if (!config::video.encoder.empty()) {
      // If there is a specific encoder specified, use it if it passes validation
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        if (encoder->name == config::video.encoder) {
          // Remove the encoder from the list entirely if it fails validation
          if (!DisplayControl::validateEncoder(*encoder, previous_encoder && previous_encoder != encoder)) {
            pos = encoder_list.erase(pos);
            break;
          }

          // We will return an encoder here even if it fails one of the codec requirements specified by the user
          adjust_encoder_constraints(encoder);

          chosen_encoder = encoder;
          break;
        }

        pos++;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder matching ["sv << config::video.encoder << ']';
      }
    }

    BOOST_LOG(info) << "// Testing for available encoders, this may generate errors. You can safely ignore those errors. //"sv;

    // If we haven't found an encoder yet, but we want one with specific codec support, search for that now.
    if (chosen_encoder == nullptr && (active_hevc_mode >= 2 || active_av1_mode >= 2)) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // Remove the encoder from the list entirely if it fails validation
        if (!DisplayControl::validateEncoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // Skip it if it doesn't support the specified codec at all
        if ((active_hevc_mode >= 2 && !encoder->hevc[encoder_t::PASSED]) ||
            (active_av1_mode >= 2 && !encoder->av1[encoder_t::PASSED])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) ||
            (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE])) {
          pos++;
          continue;
        }

        chosen_encoder = encoder;
        break;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder that meets HEVC/AV1 requirements"sv;
      }
    }

    // If no encoder was specified or the specified encoder was unusable, keep trying
    // the remaining encoders until we find one that passes validation.
    if (chosen_encoder == nullptr) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // If we've used a previous encoder and it's not this one, we expect this encoder to
        // fail to validate. It will use a slightly different order of checks to more quickly
        // eliminate failing encoders.
        if (!DisplayControl::validateEncoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // We will return an encoder here even if it fails one of the codec requirements specified by the user
        adjust_encoder_constraints(encoder);

        chosen_encoder = encoder;
        break;
      });
    }

    if (chosen_encoder == nullptr) {
      const auto output_name {display_device::map_output_name(config::video.output_name)};
      BOOST_LOG(fatal) << "Unable to find display or encoder during startup."sv;
      if (!config::video.adapter_name.empty() || !output_name.empty()) {
        BOOST_LOG(fatal) << "Please ensure your manually chosen GPU and monitor are connected and powered on."sv;
      } else {
        BOOST_LOG(fatal) << "Please check that a display is connected and powered on."sv;
      }
      return -1;
    }

    BOOST_LOG(info);
    BOOST_LOG(info) << "// Ignore any errors mentioned above, they are not relevant. //"sv;
    BOOST_LOG(info);

    auto &encoder = *chosen_encoder;

    last_encoder_probe_supported_ref_frames_invalidation = (encoder.flags & REF_FRAMES_INVALIDATION);
    last_encoder_probe_supported_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                       encoder.h264[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                       encoder.hevc[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                       encoder.av1[encoder_t::YUV444];

    BOOST_LOG(debug) << "------  h264 ------"sv;
    for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
      auto flag = (encoder_t::flag_e) x;
      BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.h264[flag] ? ": supported"sv : ": unsupported"sv);
    }
    BOOST_LOG(debug) << "-------------------"sv;
    BOOST_LOG(info) << "Found H.264 encoder: "sv << encoder.h264.name << " ["sv << encoder.name << ']';

    if (encoder.hevc[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  hevc ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.hevc[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found HEVC encoder: "sv << encoder.hevc.name << " ["sv << encoder.name << ']';
    }

    if (encoder.av1[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  av1 ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.av1[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found AV1 encoder: "sv << encoder.av1.name << " ["sv << encoder.name << ']';
    }

    if (active_hevc_mode == 0) {
      active_hevc_mode = encoder.hevc[encoder_t::PASSED] ? (encoder.hevc[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    if (active_av1_mode == 0) {
      active_av1_mode = encoder.av1[encoder_t::PASSED] ? (encoder.av1[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    return 0;
  }

  // Linux only declaration
  typedef int (*vaapi_init_avcodec_hardware_input_buffer_fn)(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    // If an egl hwdevice
    if (encode_device->data) {
      if (((vaapi_init_avcodec_hardware_input_buffer_fn) encode_device->data)(encode_device, &hw_device_buf)) {
        return -1;
      }

      return hw_device_buf;
    }

    auto render_device = config::video.adapter_name.empty() ? nullptr : config::video.adapter_name.c_str();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VAAPI, render_device, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VAAPI device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1 /* AV_CUDA_USE_PRIMARY_CONTEXT */);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a CUDA device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VideoToolbox device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef _WIN32
}

void do_nothing(void *) {
}

namespace video {
  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t ctx_buf {av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)};
    auto ctx = (AVD3D11VADeviceContext *) ((AVHWDeviceContext *) ctx_buf->data)->hwctx;

    std::fill_n((std::uint8_t *) ctx, sizeof(AVD3D11VADeviceContext), 0);

    auto device = (ID3D11Device *) encode_device->data;

    device->AddRef();
    ctx->device = device;

    ctx->lock_ctx = (void *) 1;
    ctx->lock = do_nothing;
    ctx->unlock = do_nothing;

    auto err = av_hwdevice_ctx_init(ctx_buf.get());
    if (err) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Failed to create FFMpeg hardware device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return err;
    }

    return ctx_buf;
  }
#endif

  platf::mem_type_e map_base_dev_type(AVHWDeviceType type) {
    switch (type) {
      case AV_HWDEVICE_TYPE_D3D11VA:
        return platf::mem_type_e::dxgi;
      case AV_HWDEVICE_TYPE_VAAPI:
        return platf::mem_type_e::vaapi;
      case AV_HWDEVICE_TYPE_CUDA:
        return platf::mem_type_e::cuda;
      case AV_HWDEVICE_TYPE_NONE:
        return platf::mem_type_e::system;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        return platf::mem_type_e::videotoolbox;
      default:
        return platf::mem_type_e::unknown;
    }

    return platf::mem_type_e::unknown;
  }

  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt) {
    switch (fmt) {
      case AV_PIX_FMT_VUYX:
        return platf::pix_fmt_e::ayuv;
      case AV_PIX_FMT_XV30:
        return platf::pix_fmt_e::y410;
      case AV_PIX_FMT_YUV420P10:
        return platf::pix_fmt_e::yuv420p10;
      case AV_PIX_FMT_YUV420P:
        return platf::pix_fmt_e::yuv420p;
      case AV_PIX_FMT_NV12:
        return platf::pix_fmt_e::nv12;
      case AV_PIX_FMT_P010:
        return platf::pix_fmt_e::p010;
      default:
        return platf::pix_fmt_e::unknown;
    }

    return platf::pix_fmt_e::unknown;
  }

}  // namespace video
