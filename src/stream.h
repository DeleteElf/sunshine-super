/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <utility>
// lib includes

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"
#include "video_capture_session.h"
namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9; //视频的端口，我们最好改造一下，变成可以传输两层流
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11; //音频端口 客户端发回服务端的麦克风数据也走这个

  struct session_t;

  struct config_t {
    audio::config_t audio;
    // video::config_t monitor;
    std::vector<std::shared_ptr<video::config_t>> monitors;//支持多个显示器

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void join(session_t &session);
    state_e state(session_t &session);

    std::vector<std::shared_ptr<video::CaptureSession>> getCaptureSessions(session_t* session);
  }  // namespace session
}  // namespace stream
