#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace virtmic {

class Engine;

inline constexpr wchar_t kControlPipeName[] = L"\\\\.\\pipe\\VirtMicControl";

// Named-pipe control plane for Engine (protocol Command / EventType).
class ControlServer {
 public:
  explicit ControlServer(Engine& engine);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  bool start(std::string* err = nullptr);
  void stop();

 private:
  void accept_loop_();
  void serve_client_(void* pipe_handle);

  Engine& engine_;
  std::thread accept_thread_;
  std::atomic<bool> running_{false};
  std::mutex start_mu_;
};

}  // namespace virtmic
