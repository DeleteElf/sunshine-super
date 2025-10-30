#define WIN32_LEAN_AND_MEAN

#include "vdd_utils.h"

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <filesystem>
#include <sstream>
#include <thread>

#include "src/confighttp.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "src/system_tray.h"
#include "to_string.h"

namespace display_device {
  namespace vdd_utils {

    const wchar_t *kVddPipeName = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";
    const DWORD kPipeTimeoutMs = 5000;
    const DWORD kPipeBufferSize = 4096;
    const std::chrono::milliseconds kDefaultDebounceInterval { 2000L };

    // 上次切换显示器的时间点
    static std::chrono::steady_clock::time_point last_toggle_time { std::chrono::steady_clock::now() };
    // 防抖间隔
    static std::chrono::milliseconds debounce_interval { kDefaultDebounceInterval };

    std::chrono::milliseconds calculate_exponential_backoff(int attempt) {
      auto delay = kInitialRetryDelay * (1 << attempt);
      return std::min(delay, kMaxRetryDelay);
    }

    bool execute_vdd_command(const std::string &action) {
      static const std::string scriptPath = (std::filesystem::path(SUNSHINE_ASSETS_DIR).parent_path() / "scripts" / "vdd" / "virtual-driver-manager.ps1").string();
      static const std::string driverName = "Virtual Display Adapter";
      boost::process::environment _env = boost::this_process::environment();
      auto working_dir = boost::filesystem::path();
      std::error_code ec;

      std::string cmd = std::format("powershell.exe -ExecutionPolicy Bypass -File \"{0}\" {1} --silent true",scriptPath,action);

      for (int attempt = 0; attempt < kMaxRetryCount; ++attempt) {
        auto child = platf::run_command(true, false, cmd, working_dir, _env, nullptr, ec, nullptr);
        if (!ec) {
          BOOST_LOG(info) << "成功执行VDD " << action << " 命令";
          child.detach();
          return true;
        }

        auto delay = calculate_exponential_backoff(attempt);
        BOOST_LOG(warning) << "执行VDD " << action << " 命令失败 (尝试 "
                           << (attempt + 1) << "/" << kMaxRetryCount
                           << "): " << ec.message() << ". 将在 "
                           << delay.count() << "ms 后重试";
        std::this_thread::sleep_for(delay);
      }

      BOOST_LOG(error) << "执行VDD " << action << " 命令失败，已达到最大重试次数";
      return false;
    }

    HANDLE connect_to_pipe_with_retry(const wchar_t *pipe_name, int max_retries) {
      HANDLE hPipe = INVALID_HANDLE_VALUE;
      int attempt = 0;
      auto retry_delay = kInitialRetryDelay;

      while (attempt < max_retries) {
        hPipe = CreateFileW(
          pipe_name,
          GENERIC_READ | GENERIC_WRITE,
          0,
          NULL,
          OPEN_EXISTING,
          FILE_FLAG_OVERLAPPED,  // 使用异步IO
          NULL);

        if (hPipe != INVALID_HANDLE_VALUE) {
          DWORD mode = PIPE_READMODE_MESSAGE;
          if (SetNamedPipeHandleState(hPipe, &mode, NULL, NULL)) {
            return hPipe;
          }
          CloseHandle(hPipe);
        }

        ++attempt;
        retry_delay = calculate_exponential_backoff(attempt);
        std::this_thread::sleep_for(retry_delay);
      }
      return INVALID_HANDLE_VALUE;
    }

    bool execute_pipe_command(const wchar_t *pipe_name, const wchar_t *command, std::string *response) {
      auto hPipe = connect_to_pipe_with_retry(pipe_name);
      if (hPipe == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << "连接MTT虚拟显示管道失败，已重试多次";
        return false;
      }

      // 异步IO结构体
      OVERLAPPED overlapped = { 0 };
      overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

      struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() {
          if (handle) CloseHandle(handle);
        }
      } event_guard { overlapped.hEvent };

      // 发送命令（使用宽字符版本）
      DWORD bytesWritten;
      size_t cmd_len = (wcslen(command) + 1) * sizeof(wchar_t);  // 包含终止符
      if (!WriteFile(hPipe, command, (DWORD) cmd_len, &bytesWritten, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) {
          BOOST_LOG(error) << L"发送" << command << L"命令失败，错误代码: " << GetLastError();
          return false;
        }

        // 等待写入完成
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kPipeTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
          BOOST_LOG(error) << L"发送" << command << L"命令超时";
          return false;
        }
      }

      // 读取响应
      if (response) {
        char buffer[kPipeBufferSize];
        DWORD bytesRead;
        if (!ReadFile(hPipe, buffer, sizeof(buffer), &bytesRead, &overlapped)) {
          if (GetLastError() != ERROR_IO_PENDING) {
            BOOST_LOG(warning) << "读取响应失败，错误代码: " << GetLastError();
            return false;
          }

          DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kPipeTimeoutMs);
          if (waitResult == WAIT_OBJECT_0 && GetOverlappedResult(hPipe, &overlapped, &bytesRead, FALSE)) {
            buffer[bytesRead] = '\0';
            *response = std::string(buffer, bytesRead);
          }
        }
      }

      return true;
    }

    bool reload_driver() {
      std::string response;
      return execute_pipe_command(kVddPipeName, L"RELOAD_DRIVER", &response);
    }

    void enable_vdd() {
      execute_vdd_command("enable");
    }

    void disable_vdd() {
      execute_vdd_command("disable");
    }

    void disable_enable_vdd() {
      disable_vdd();
      enable_vdd();
    }

    bool is_display_on() {
      return !display_device::find_device_by_friendlyname(virtual_name).empty();
    }

    void toggle_display_power() {
      execute_vdd_command("toggle");
    }

    void update_vdd_resolution(const SingleDisplayConfiguration &config, const vdd_utils::VddSettings &vdd_settings,int displayCount) {
      const auto new_setting = to_string(*config.m_resolution) + "@" + to_string(*config.m_refresh_rate);
      if (!confighttp::saveVddSettings(vdd_settings.resolutions, vdd_settings.fps, config::video.adapter_name,displayCount)) {
        BOOST_LOG(error) << "VDD配置保存失败 [resolutions: " << vdd_settings.resolutions << " fps: " << vdd_settings.fps << "]";
        return;
      }
      BOOST_LOG(info) << "VDD配置更新完成: " << new_setting;
      // 配置变更后执行驱动重载
      BOOST_LOG(info) << "重新启用VDD驱动...";
      vdd_utils::enable_vdd();
      std::this_thread::sleep_for(1500ms);
    }

    VddSettings prepare_vdd_settings(const SingleDisplayConfiguration &config) {
      auto is_res_cached = false;
      auto is_fps_cached = false;
      std::ostringstream res_stream, fps_stream;

      res_stream << '[';
      fps_stream << '[';

      // 如果需要更新设置
      bool needs_update = (!is_res_cached || !is_fps_cached) && config.m_resolution;
      if (needs_update) {
        if (!is_res_cached) {
          res_stream << to_string(*config.m_resolution);
        }
        if (!is_fps_cached) {
          fps_stream << to_string(*config.m_refresh_rate);
        }
      }

      // 移除最后的逗号并添加结束括号
      auto res_str = res_stream.str();
      auto fps_str = fps_stream.str();
      if (res_str.back() == ',') res_str.pop_back();
      if (fps_str.back() == ',') fps_str.pop_back();
      res_str += ']';
      fps_str += ']';

      return { res_str, fps_str, needs_update };
    }

    void prepare_vdd(SingleDisplayConfiguration &config, const rtsp_stream::launch_session_t &session,int displayCount) {
      auto vdd_settings = vdd_utils::prepare_vdd_settings(config);
      const bool has_new_resolution = vdd_settings.needs_update && config.m_resolution;
      BOOST_LOG(debug) << "VDD配置状态: needs_update=" << std::boolalpha << vdd_settings.needs_update
                       << ", new_setting=" << (config.m_resolution ? to_string(*config.m_resolution) + "@" + to_string(*config.m_refresh_rate) : "none");
      auto device_virtual = display_device::find_device_by_friendlyname(virtual_name);
      auto lastDeviceId = device_virtual;
      if (displayCount == 1)
        lastDeviceId = "";
      if (has_new_resolution) update_vdd_resolution(config, vdd_settings, displayCount);
      const bool device_found = vdd_utils::retry_with_backoff(
        [&device_virtual, &lastDeviceId]() {
          device_virtual = display_device::find_device_by_friendlyname(virtual_name);
          return device_virtual != lastDeviceId;
        },
        {.max_attempts = 10,
         .initial_delay = 100ms,
         .max_delay = 500ms,
         .context = "等待VDD设备初始化"}
      );
      // 失败后优化处理流程
      if (!device_found) {
        BOOST_LOG(error) << "VDD设备初始化失败，尝试重置驱动";
        for (int retry = 1; retry <= 3; ++retry) {
          BOOST_LOG(info) << "正在执行第" << retry << "次VDD恢复尝试...";
          disable_enable_vdd();
          std::this_thread::sleep_for(1s);

          if (vdd_utils::retry_with_backoff(
                [&device_virtual, &lastDeviceId]() {
                  device_virtual = display_device::find_device_by_friendlyname(virtual_name);
                  return device_virtual != lastDeviceId;
                },
                {.max_attempts = 5,
                 .initial_delay = 233ms,
                 .max_delay = 2000ms,
                 .context = "最终设备检查"}
              )) {
            BOOST_LOG(info) << "VDD设备恢复成功！";
            break;
          }

          BOOST_LOG(error) << "VDD设备检测失败，正在第" << retry << "/3次重试...";
          if (retry < 3) std::this_thread::sleep_for(std::chrono::seconds(1L << retry));
      }
        if (device_virtual == lastDeviceId) {
          BOOST_LOG(error) << "VDD设备最终初始化失败，请检查显卡驱动和设备状态";
        }
      }

      // 更新设备配置
      if (device_virtual != lastDeviceId) {
        config.m_device_id = device_virtual;
        config::video.output_name = device_virtual;
        BOOST_LOG(info) << "成功配置VDD设备: " << device_virtual;
      }
    }
  }  // namespace vdd_utils
}  // namespace display_device