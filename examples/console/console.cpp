#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <thread>

#include "core/command/command.hpp"
#include "core/logging/log_format.hpp"
#include "core/logging/logger.hpp"
#include "core/schedule/scheduler.hpp"
#include "platform/host/platform.h"

namespace core = daveos::core;

namespace app {

enum class Event {};

// Keep one extra byte to let the dispatcher diagnose overlength input. Once
// full, discard bytes until newline instead of splitting one command into two.
struct Line {
  std::array<char, 257> bytes{};
  std::size_t size = 0;
};

struct Input {
  core::Queue<Line, 16> lines;
  std::mutex mutex;
};

// This thread only collects input. It never calls scheduler/module APIs.
// Polling with a bounded timeout allows explicit exit to join a blocked reader.
void Read(std::stop_token stop, Input& input) {
  Line line;
  while (!stop.stop_requested()) {
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    int ready = ::poll(&fd, 1, 50);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (!ready) continue;
    if (fd.revents & (POLLERR | POLLNVAL)) return;
    char byte;
    auto size = ::read(STDIN_FILENO, &byte, 1);
    if (size < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    // EOF ends collection only. It is not a command and does not stop DaveOS.
    if (size <= 0) return;
    if (byte != '\n') {
      if (line.size < line.bytes.size()) line.bytes[line.size++] = byte;
      continue;
    }
    while (!stop.stop_requested()) {
      {
        std::lock_guard lock(input.mutex);
        if (input.lines.push(line) == core::Status::ok) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    line = {};
  }
}

class Console : public core::Module<Console, Event> {
 public:
  explicit Console(Input& input) : input_(input) {}

  static constexpr const char* name() { return "console"; }

  static constexpr auto tasks() {
    return std::array{core::TaskDescriptor<Console>{"input", &Console::Poll}};
  }

  static constexpr auto commands() {
    return std::array{
        DAVEOS_COMMAND(Console, "echo", Echo, "Log the supplied arguments"),
        DAVEOS_COMMAND(Console, "exit", Exit, "Stop the host program")};
  }

  template <typename Dispatcher>
  void dispatcher(Dispatcher& dispatcher) {
    dispatcher_ = &dispatcher;
    dispatch_ = [](void* context, std::string_view line) {
      return static_cast<Dispatcher*>(context)->dispatch(line);
    };
  }

  core::Status init(core::InitStage stage) {
    if (stage == core::InitStage::stage1) {
      I_("Type help or console exit");
      return scheduler().schedule(*this, &Console::Poll, 10000,
                                  core::Mode::repeat);
    }
    return core::Status::ok;
  }

 private:
  void Poll() {
    Line line;
    {
      std::lock_guard lock(input_.mutex);
      if (input_.lines.pop(line) != core::Status::ok) return;
    }
    dispatch_(dispatcher_, std::string_view(line.bytes.data(), line.size));
  }

  core::Status Echo(core::CommandArguments args) {
    for ([[maybe_unused]] auto arg : args)
      I_("%.*s", static_cast<int>(arg.size()), arg.data());
    return core::Status::ok;
  }

  core::Status Exit(core::CommandArguments args) {
    if (!args.empty()) return core::Status::invalid_argument;
    I_("Exiting");
    return scheduler().stop();
  }

  Input& input_;
  void* dispatcher_ = nullptr;
  core::Status (*dispatch_)(void*, std::string_view) = nullptr;
};

void Output(void*, const core::LogRecord& record) {
  core::LogPrefix prefix(record);
  auto text = prefix.view();
  std::printf("%.*s%.*s\n", static_cast<int>(text.size()), text.data(),
              static_cast<int>(record.message.size()), record.message.data());
  std::fflush(stdout);
}

// Passive application state; threads and scheduler execution begin in main.
Input input;
Console console(input);
}  // namespace app

int main() {

  daveos::platform::host::Platform platform;
  auto modules = core::ModuleList{&app::console};
  auto logger = core::make_logger(
      platform, core::SubscriberList{core::Subscriber{nullptr, app::Output}});
  auto scheduler = core::make_scheduler<app::Event>(platform, modules, logger);
  core::CommandDispatcher dispatcher(modules, scheduler);
  app::console.dispatcher(dispatcher);
  std::jthread reader(
      [](std::stop_token stop) { app::Read(stop, app::input); });
  return scheduler.run() == core::Status::ok ? 0 : 1;
}
