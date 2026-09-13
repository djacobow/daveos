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

namespace app {
using namespace daveos::core;
enum class Event {};
// Keep one extra byte to let the dispatcher diagnose overlength input. Once
// full, discard bytes until newline instead of splitting one command into two.
struct Line {
  std::array<char, 257> bytes{};
  std::size_t size = 0;
};
struct Input {
  Queue<Line, 16> lines;
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
        if (input.lines.push(line) == Status::ok) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    line = {};
  }
}
class Console : public Module<Console, Event> {
 public:
  explicit Console(Input& input) : input_(input) {}
  static constexpr const char* name() { return "console"; }
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Console>{"input", &Console::Poll}};
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
  Status init(InitStage stage) {
    if (stage == InitStage::stage1) {
      I_("Type help or console exit");
      return scheduler().schedule(*this, &Console::Poll, 10000, Mode::repeat);
    }
    return Status::ok;
  }

 private:
  void Poll() {
    Line line;
    {
      std::lock_guard lock(input_.mutex);
      if (input_.lines.pop(line) != Status::ok) return;
    }
    dispatch_(dispatcher_, std::string_view(line.bytes.data(), line.size));
  }
  Status Echo(CommandArguments args) {
    for ([[maybe_unused]] auto arg : args)
      I_("%.*s", static_cast<int>(arg.size()), arg.data());
    return Status::ok;
  }
  Status Exit(CommandArguments args) {
    if (!args.empty()) return Status::invalid_argument;
    I_("Exiting");
    return scheduler().stop();
  }
  Input& input_;
  void* dispatcher_ = nullptr;
  Status (*dispatch_)(void*, std::string_view) = nullptr;
};
void Output(void*, const LogRecord& record) {
  LogPrefix prefix(record);
  auto text = prefix.view();
  std::printf("%.*s%.*s\n", static_cast<int>(text.size()), text.data(),
              static_cast<int>(record.message.size()), record.message.data());
  std::fflush(stdout);
}
}  // namespace app
int main() {
  using namespace daveos::core;
  daveos::platform::host::Platform platform;
  app::Input input;
  app::Console console(input);
  auto modules = ModuleList{&console};
  auto logger =
      make_logger(platform, SubscriberList{Subscriber{nullptr, app::Output}});
  auto scheduler = make_scheduler<app::Event>(platform, modules, logger);
  CommandDispatcher dispatcher(modules, scheduler);
  console.dispatcher(dispatcher);
  std::jthread reader([&](std::stop_token stop) { app::Read(stop, input); });
  return scheduler.run() == Status::ok ? 0 : 1;
}
