#include "video_capture_session.h"

namespace video {

  CaptureSession::CaptureSession(int index):sessionIndex(index) {
    session_mail=std::make_shared<safe::mail_raw_t>();

    capture_thread_ctx=std::make_shared<capture_thread_ctx_t>();
    capture_thread_ctx->idr_event=session_mail->event<bool>(mail::idr);
    capture_thread_ctx->switch_display_event=session_mail->event<int>(mail::switch_display);
    capture_thread_ctx->capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30); //最多支持30个
  }

  capture_ctx_t& CaptureSession::addCaptureContext(img_event_t& images,config_t& _config){
    this->config=_config;
    this->sessionIndex=config.displayIndex;
    this->capture_ctx=capture_ctx_t{1,images,_config};
    this->capture_thread_ctx->capture_ctx_queue->raise(capture_ctx);
    return capture_ctx;
  }

  void captureThread(CaptureSession* captureSession,
        std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue,
        sync_util::sync_t<std::weak_ptr<platf::display_t>> &display_wp,
        safe::signal_t &reinit_event,
        const encoder_t &encoder,
        int displayIndex) {
    std::vector<capture_ctx_t> capture_ctxs;
    BOOST_LOG(debug) << "captureThread started==================>"sv << displayIndex ;
    auto fg = util::fail_guard([&]() {
      BOOST_LOG(debug) << "captureThread error ==================>"sv << displayIndex ;
      capture_ctx_queue->stop();
      // Stop all sessions listening to this thread
      for (auto &capture_ctx : capture_ctxs) {
      capture_ctx.images->stop();
      }
      for (auto &capture_ctx : capture_ctx_queue->unsafe()) {
      capture_ctx.images->stop();
      }
    });
    auto & switch_display_event = captureSession->getContext()->switch_display_event;
    // Wait for the initial capture context or a request to stop the queue
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      return;
    }
    capture_ctxs.emplace_back(std::move(*initial_capture_ctx));
    int currentDisplayIndex=displayIndex;
    // Get all the monitor names now, rather than at boot, to
    // get the most up-to-date list available monitors
    std::vector<std::string> display_names;
    //因为我们需要一开始就指定显示索引，所以需要先获取一下设备清单
    display_names = platf::display_names(encoder.platform_formats->dev_type);  // 获取当前的设备列表
    DisplayControl::refreshDisplays(encoder.platform_formats->dev_type, display_names, currentDisplayIndex);
    auto current_display_name=display_names[currentDisplayIndex];//获取当前的显示设备名称，因为等下其他显示如果正在采样，将获取不到
    auto disp = platf::display(encoder.platform_formats->dev_type, current_display_name, capture_ctxs.front().config);
    if (!disp) {
      return;
    }
    display_wp = disp; //这个时候才实际获取到设备
    BOOST_LOG(debug) << "catch display==================>"sv << displayIndex ;
    constexpr auto capture_buffer_size = 12;
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);

    std::vector<std::optional<std::chrono::steady_clock::time_point>> imgs_used_timestamps;
    const std::chrono::seconds trim_timeot = 3s;
    auto trim_imgs = [&]() {
      // count allocated and used within current pool
      size_t allocated_count = 0;
      size_t used_count = 0;
      for (const auto &img : imgs) {
        if (img) {
          allocated_count += 1;
          if (img.use_count() > 1) {
            used_count += 1;
          }
        }
      }

      // remember the timestamp of currently used count
      const auto now = std::chrono::steady_clock::now();
      if (imgs_used_timestamps.size() <= used_count) {
        imgs_used_timestamps.resize(used_count + 1);
      }
      imgs_used_timestamps[used_count] = now;

      // decide whether to trim allocated unused above the currently used count
      // based on last used timestamp and universal timeout
      size_t trim_target = used_count;
      for (size_t i = used_count; i < imgs_used_timestamps.size(); i++) {
        if (imgs_used_timestamps[i] && now - *imgs_used_timestamps[i] < trim_timeot) {
          trim_target = i;
        }
      }

      // trim allocated unused above the newly decided trim target
      if (allocated_count > trim_target) {
        size_t to_trim = allocated_count - trim_target;
        // prioritize trimming least recently used
        for (auto it = imgs.rbegin(); it != imgs.rend(); it++) {
          auto &img = *it;
          if (img && img.use_count() == 1) {
            img.reset();
            to_trim -= 1;
            if (to_trim == 0) {
              break;
            }
          }
        }
        // forget timestamps that no longer relevant
        imgs_used_timestamps.resize(trim_target + 1);
      }
    };

    auto pull_free_image_callback = [&](std::shared_ptr<platf::img_t> &img_out) -> bool {
      img_out.reset();
      while (capture_ctx_queue->running()) {
        // pick first allocated but unused
        for (auto it = imgs.begin(); it != imgs.end(); it++) {
          if (*it && it->use_count() == 1) {
            img_out = *it;
            if (it != imgs.begin()) {
              // move image to the front of the list to prioritize its reusal
              imgs.erase(it);
              imgs.push_front(img_out);
            }
            break;
          }
        }
        // otherwise pick first unallocated
        if (!img_out) {
          for (auto it = imgs.begin(); it != imgs.end(); it++) {
            if (!*it) {
              // allocate image
              *it = disp->alloc_img();
              img_out = *it;
              if (it != imgs.begin()) {
                // move image to the front of the list to prioritize its reusal
                imgs.erase(it);
                imgs.push_front(img_out);
              }
              break;
            }
          }
        }
        if (img_out) {
          // trim allocated but unused portion of the pool based on timeouts
          trim_imgs();
          img_out->frame_timestamp.reset();
          return true;
        } else {
          // sleep and retry if image pool is full
          std::this_thread::sleep_for(1ms);
        }
      }
      return false;
    };
    BOOST_LOG(debug) << "will loop capture display==================>"sv << displayIndex ;
    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);
            continue;
          }

          if (frame_captured) {
            capture_ctx->images->raise(img);
          }
          ++capture_ctx;
        })

        if (!capture_ctx_queue->running()) {
          return false;
        }

        while (capture_ctx_queue->peek()) {
          capture_ctxs.emplace_back(std::move(*capture_ctx_queue->pop()));
        }

        if (switch_display_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };
      BOOST_LOG(debug) << "capture display now==================>"sv << displayIndex ;
      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);
      BOOST_LOG(debug) << "capture display stop==================>"sv << displayIndex ;
      if (artificial_reinit && status != platf::capture_e::error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit:
          {
            BOOST_LOG(debug) << "capture display will reinit==================>"sv << displayIndex ;
            reinit_event.raise(true);

            // Some classes of images contain references to the display --> display won't delete unless img is deleted
            for (auto &img : imgs) {
              img.reset();
            }

            // display_wp is modified in this thread only
            // Wait for the other shared_ptr's of display to be destroyed.
            // New displays will only be created in this thread.
            while (display_wp->use_count() != 1) {
              // Free images that weren't consumed by the encoders. These can reference the display and prevent
              // the ref count from reaching 1. We do this here rather than on the encoder thread to avoid race
              // conditions where the encoding loop might free a good frame after reinitializing if we capture
              // a new frame here before the encoder has finished reinitializing.
              KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
                if (!capture_ctx->images->running()) {
                  capture_ctx = capture_ctxs.erase(capture_ctx);
                  continue;
                }

                while (capture_ctx->images->peek()) {
                  capture_ctx->images->pop();
                }

                ++capture_ctx;
              });

              std::this_thread::sleep_for(20ms);
            }

            while (capture_ctx_queue->running()) {
              // Release the display before reenumerating displays, since some capture backends
              // only support a single display session per device/application.
              disp.reset();
              //这个时候无法获取到其他正在采样的显示器,那么这里会产生一个冲突，就是获取的显示列表是需要排除掉其他正在采样的列表
              //我们先按之前的显示名称进行查找
              auto  new_display_names = platf::display_names(encoder.platform_formats->dev_type);
              while(capture_ctx_queue->running()&&new_display_names.size()==0){ //等待丢失的设备恢复
                std::this_thread::sleep_for(20ms);
                new_display_names = platf::display_names(encoder.platform_formats->dev_type);
              }
              int new_display_index=-1;
              for (int i =0; i < new_display_names.size(); i++)
              {
                if(current_display_name==new_display_names[i]){
                  new_display_index=i;
                  break;
                }
              }
              if(new_display_index==-1)//没有找到旧设备
                DisplayControl::refreshDisplays(encoder.platform_formats->dev_type, display_names, new_display_index);//找个新的能用的设备

              // Process any pending display switch with the new list of displays
              if (switch_display_event->peek()) {
                auto newDisplayIndex= switch_display_event->pop();
                BOOST_LOG(debug) <<"显示器【"sv <<displayIndex << "】收到客户端请求了新的显示==================>"sv << *newDisplayIndex <<" 当前显示器数量：" << display_names.size();
                new_display_index = std::clamp(*newDisplayIndex, 0, (int) display_names.size() - 1);
                BOOST_LOG(debug) <<"显示器【"sv <<displayIndex << "】适配新的显示索引==================>"sv << new_display_index ;
              }
              current_display_name=display_names[new_display_index];
              BOOST_LOG(debug) <<"显示器【"sv <<displayIndex << "】适配新的显示结果==================>"sv << current_display_name ;
              // reset_display() will sleep between retries
              DisplayControl::resetDisplay(disp, encoder.platform_formats->dev_type, current_display_name, capture_ctxs.front().config);
              if (disp) {
                break;
              }
            }
            if (!disp) {
              return;
            }
            display_wp = disp;
            BOOST_LOG(debug) <<"显示器【"sv <<displayIndex << "】获取显示器成功，新的显示索引======================>"sv << currentDisplayIndex ;
            reinit_event.reset();
            continue;
          }
        case platf::capture_e::ok:
//          BOOST_LOG(debug) << "capture display success==================>"sv << display_p ;
        case platf::capture_e::error:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
          return;
      }
    }
  }

  int CaptureSession::start_capture_async(encoder_t *encoder) {
    this->sessionIndex=config.displayIndex;
    BOOST_LOG(info) << "start_capture_async============================>"sv << sessionIndex ;
    capture_thread_ctx->encoder_p = encoder;
    capture_thread_ctx->reinit_event.reset();
    capture_thread = std::thread {captureThread,this,capture_thread_ctx->capture_ctx_queue,
      std::ref(capture_thread_ctx->display_wp),std::ref(capture_thread_ctx->reinit_event),
      std::ref(*capture_thread_ctx->encoder_p),sessionIndex};
    return 0;
  }

  void CaptureSession::end_capture_async() {
    capture_thread_ctx->capture_ctx_queue->stop();
    if(capture_thread.joinable())
      capture_thread.join();
    BOOST_LOG(info) << "end_capture_async============================>"sv << sessionIndex;
  }

  void encodeRun(safe::mail_t& mail,safe::mail_raw_t::event_t<bool> &idr_event,
                 std::shared_ptr<platf::display_t>& display,safe::signal_t &reinit_event,
                 const encoder_t &encoder,img_event_t& images,config_t& config,int& frameIndex,void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto encode_device = DisplayControl::makeEncodeDevice(*display, encoder, config);
    if (!encode_device) {
      return;
    }
    BOOST_LOG(info) << "makeEncodeDevice success=======================>" << config.displayIndex;
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);
    // 编码发生在此线程上 设置成高优先级
    platf::adjust_thread_priority(platf::thread_priority_e::high);
    // absolute mouse coordinates require that the dimensions of the screen are known
    //   touch_port_event->raise(makePort(disp.get(), config));
    // Update client with our current HDR display state
    hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
    if (colorspace_is_hdr(encode_device->colorspace)) {
      if (display->get_hdr_metadata(hdr_info->metadata)) {
        hdr_info->enabled = true;
      } else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }
    hdr_event->raise(std::move(hdr_info));
    auto session = DisplayControl::makeEncodeSession(display.get(), encoder, config, display->width, display->height, std::move(encode_device));
    if (!session) {
      // encoder_lock.unlock();
      return;
    }
    BOOST_LOG(info) << "makeEncodeSession success=======================>" << config.displayIndex;
    // As a workaround for NVENC hangs and to generally speed up encoder reinit,
    // we will complete the encoder teardown in a separate thread if supported.
    // This will move expensive processing off the encoder thread to allow us
    // to restart encoding as soon as possible. For cases where the NVENC driver
    // hang occurs, this thread may probably never exit, but it will allow
    // streaming to continue without requiring a full restart of Sunshine.
    auto fail_guard = util::fail_guard([&encoder, &session] {
      if (encoder.flags & ASYNC_TEARDOWN) {
        std::thread encoder_teardown_thread {[session = std::move(session)]() mutable {
          BOOST_LOG(info) << "Starting async encoder teardown";
          session.reset();
          BOOST_LOG(info) << "Async encoder teardown complete";
        }};
        encoder_teardown_thread.detach();
      }
    });

    // set max frame time based on client-requested target framerate.
    double minimum_fps_target = (config::video.minimum_fps_target > 0.0) ? config::video.minimum_fps_target : config.framerate;
    std::chrono::duration<double, std::milli> max_frametime {1000.0 / minimum_fps_target};
    BOOST_LOG(info) << "Minimum FPS target set to ~"sv << (minimum_fps_target / 2) << "fps ("sv << max_frametime.count() * 2 << "ms)"sv;

    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
    {
      // Load a dummy image into the AVFrame to ensure we have something to encode
      // even if we timeout waiting on the first frame. This is a relatively large
      // allocation which can be freed immediately after convert(), so we do this
      // in a separate scope.
      auto dummy_img = display->alloc_img();
      if (!dummy_img || display->dummy_img(dummy_img.get()) || session->convert(*dummy_img)) {
        //   encoder_lock.unlock();
        return;
      }
    }
    //   encoder_lock.unlock();
    while (true) {
      // Break out of the encoding loop if any of the following are true:
      // a) The stream is ending
      // b) Sunshine is quitting
      // c) The capture side is waiting to reinit and we've encoded at least one frame
      //
      // If we have to reinit before we have received any captured frames, we will encode
      // the blank dummy frame just to let Moonlight know that we're alive.
      if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frameIndex > 1)) {
        break;
      }
      bool requested_idr_frame = false;

      while (invalidate_ref_frames_events->peek()) {
        if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
          session->invalidate_ref_frames(frames->first, frames->second);
        }
      }

      if (idr_event->peek()) {
        requested_idr_frame = true;
        idr_event->pop();
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
        BOOST_LOG(debug) << "request idr frame =======================>" << config.displayIndex;
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
      // Encode at a minimum FPS to avoid image quality issues with static content
      if (!requested_idr_frame || images->peek()) {
        if (auto img = images->pop(max_frametime)) {
          frame_timestamp = img->frame_timestamp;
          if (session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            return;
          }
        } else if (!images->running()) {
          break;
        }
      }

      if (DisplayControl::encode(frameIndex++, *session, packets, channel_data, frame_timestamp)) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        return;
      }

      session->request_normal_frame();
    }
  }

  void encoding(safe::mail_t mail,input::touch_ports& touchPorts,
                const std::shared_ptr<capture_thread_ctx_t>& capture_thread_ctx,capture_ctx_t& ctx,void *channel_data ) {
    BOOST_LOG(info) << "start_encode_async============================>"sv << ctx.config.displayIndex;
    capture_thread_ctx->encodeRunning = true;
    while (capture_thread_ctx->encodeRunning && capture_thread_ctx->capture_ctx_queue->running() && ctx.images->running()) {  // 一直尝试到获取到设备为止
      // Wait for the main capture event when the display is being reinitialized
      if (capture_thread_ctx->reinit_event.peek()) {
        std::this_thread::sleep_for(20ms);
        continue;
      }
      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      {
        auto lg = capture_thread_ctx->display_wp.lock();
        if (capture_thread_ctx->display_wp->expired()) {
          continue;
        }
        display = capture_thread_ctx->display_wp->lock();
      }
      DisplayControl::makePort(touchPorts, display.get(), ctx.config);
      mail->event<input::touch_ports>(mail::touch_port)->raise(touchPorts);
      encodeRun(mail, capture_thread_ctx->idr_event,display, capture_thread_ctx->reinit_event, *capture_thread_ctx->encoder_p,
                std::ref(ctx.images), std::ref(ctx.config), std::ref(ctx.frameIndex), channel_data);
    }
    BOOST_LOG(info) << "end_encode_async============================>"sv << ctx.config.displayIndex;
  }


  int CaptureSession::start_encode_async(safe::mail_t& mail,input::touch_ports& touchPorts,void *channel_data) {
    encode_thread =std::thread{encoding, mail,std::ref(touchPorts),std::ref(capture_thread_ctx),std::ref(capture_ctx),channel_data};
    return 0;
  }

  void CaptureSession::end_encode_async() {
      if(encode_thread.joinable()){
        encode_thread.join();
      }
      BOOST_LOG(info) << "encode_thread stop ============================>"sv << config.displayIndex;
  }
}