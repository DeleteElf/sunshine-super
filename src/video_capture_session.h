#pragma once
#include <thread>
#include <list>

#include "globals.h"
#include "display_control.h"
#include "input.h"
#include "display_device/vdd_utils.h"
namespace video {

  class CaptureSession {
  private:
    std::thread capture_thread;
    std::thread encode_thread;
  public:
    int sessionIndex;
    explicit CaptureSession(int index);
    ~CaptureSession(){
      capture_thread_ctx=nullptr;
    }
  private:
    std::shared_ptr<capture_thread_ctx_t> capture_thread_ctx;
    capture_ctx_t capture_ctx;
    config_t config;

    safe::mail_t session_mail;
  public:
    capture_ctx_t& addCaptureContext(img_event_t& images,config_t& _config);
    int start_capture_async(encoder_t *encoder);
    void end_capture_async();
    int start_encode_async(safe::mail_t& mail,input::touch_ports& touchPorts,void *channel_data);
    void end_encode_async();
    std::shared_ptr<capture_thread_ctx_t> getContext(){
      return capture_thread_ctx;
    }
    safe::mail_t getSessionMail(){
      return session_mail;
    };
  };
}
