#pragma once

#include <array>
#include <limits>

#include "core/schedule/module.hpp"

namespace daveos::core {


// Fixed-storage complete-line dispatcher. Modules and scheduler must outlive
// it. Call only on the scheduler thread after initialization, from a callback.
// Input collection/serialization belongs to the application, never this class.
// LineCapacity counts input bytes (no terminator); ArgumentCapacity includes
// prefix and command. Help/errors use ordinary best-effort buffered logging.
template <typename Event, typename... Modules, std::size_t LineCapacity,
          std::size_t ArgumentCapacity>
class CommandDispatcher<Event, ModuleList<Modules...>, LineCapacity,
                        ArgumentCapacity> {
  static_assert(LineCapacity > 0 && ArgumentCapacity >= 2);
  static_assert(LineCapacity <= std::numeric_limits<int>::max());
  template <typename M>
  static consteval bool ValidModule() {
    static_assert(std::is_same_v<typename M::EventType, Event>);
    constexpr auto commands = M::commands();
    if (commands.empty()) return true;
    if (!ValidCommandName(M::command_prefix()) ||
        EqualName(M::command_prefix(), "help"))
      return false;
    for (std::size_t i = 0; i < commands.size(); ++i) {
      const auto& c = commands[i];
      if (!ValidCommandName(c.name) || EqualName(c.name, "help") ||
          !c.callback || !c.help || !*c.help || !c.handler || !*c.handler)
        return false;
      for (std::size_t j = 0; j < i; ++j)
        if (EqualName(c.name, commands[j].name)) return false;
    }
    return true;
  }
  static_assert((ValidModule<Modules>() && ...), "invalid command metadata");
  static_assert(UniqueNames(std::array<const char*, sizeof...(Modules)>{(
                                Modules::commands().empty()
                                    ? nullptr
                                    : Modules::command_prefix())...},
                            true),
                "command prefixes must be unique (case-insensitive)");
  struct Entry {
    const char* prefix;
    void* module;
    Status (*dispatch)(CommandDispatcher&, void*, CommandArguments);
    void (*help)(CommandDispatcher&);
  };

 public:
  CommandDispatcher(ModuleList<Modules...> modules,
                    SchedulerInterface<Event>& scheduler)
      : scheduler_(scheduler) {
    std::apply([&](auto*... module) { (Register(module), ...); },
               modules.items);
  }
  CommandDispatcher(const CommandDispatcher&) = delete;
  CommandDispatcher& operator=(const CommandDispatcher&) = delete;

  // Reject a malformed/oversized line before invoking any handler. Borrowed
  // argument views survive until return. Reentry returns busy without touching
  // active storage. No concurrent dispatch or interrupt dispatch is supported.
  Status dispatch(std::string_view line) {
    struct Request {
      CommandDispatcher* self;
      std::string_view line;
    };
    Request request{this, line};
    return scheduler_.Invoke(
        {"core", "command"},
        [](void* argument) {
          auto& r = *static_cast<Request*>(argument);
          Status status = r.self->Dispatch(r.line);
          if (status != Status::ok) r.self->Error(status);
          return status;
        },
        &request);
  }

 private:
  static constexpr bool Space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
  }
  template <typename M>
  void Register(M* module) {
    if (!module) valid_ = false;
    if constexpr (!M::commands().empty()) {
      entries_[count_++] = {
          M::command_prefix(), module,
          [](CommandDispatcher& self, void* object, CommandArguments args) {
            return self.Handle(*static_cast<M*>(object), args);
          },
          [](CommandDispatcher& self) { self.template Help<M>(); }};
    }
  }
  Status Parse(std::string_view line) {
    if (line.size() > LineCapacity) return Status::line_too_long;
    for (char c : line)
      if (!c) return Status::parse_error;
    argc_ = 0;
    std::size_t read = 0, write = 0;
    while (read < line.size()) {
      if (Space(line[read])) {
        ++read;
        continue;
      }
      if (argc_ == ArgumentCapacity) return Status::too_many_arguments;
      const std::size_t start = write;
      bool quoted = line[read] == '"';
      if (quoted) ++read;
      bool closed = !quoted;
      while (read < line.size()) {
        char c = line[read];
        if (c == '\\' && read + 1 < line.size() &&
            (line[read + 1] == '"' || line[read + 1] == '\\')) {
          buffer_[write++] = line[read + 1];
          read += 2;
          continue;
        }
        if (c == '"') {
          if (!quoted) return Status::parse_error;
          ++read;
          closed = true;
          if (read < line.size() && !Space(line[read]))
            return Status::parse_error;
          break;
        }
        if (!quoted && Space(c)) break;
        buffer_[write++] = c;
        ++read;
      }
      if (!closed) return Status::parse_error;
      arguments_[argc_++] =
          std::string_view(buffer_.data() + start, write - start);
    }
    return Status::ok;
  }
  Status Dispatch(std::string_view line) {
    if (busy_) return Status::busy;
    if (!valid_) return Status::invalid_argument;
    struct Active {
      bool& flag;
      explicit Active(bool& flag) : flag(flag) { flag = true; }
      ~Active() { flag = false; }
    } active(busy_);
    Status status = Parse(line);
    if (status != Status::ok || !argc_) return status;
    auto match = lazy_match(arguments_[0], count_ + 1, [&](std::size_t i) {
      return std::string_view(i == count_ ? "help" : entries_[i].prefix);
    });
    if (match.status != Status::ok) return match.status;
    if (match.index == count_) {
      if (argc_ != 1) return Status::invalid_argument;
      DAVEOS_LOG(scheduler_, Level::info, "help - list all commands");
      for (std::size_t i = 0; i < count_; ++i) entries_[i].help(*this);
      return Status::ok;
    }
    auto& entry = entries_[match.index];
    return entry.dispatch(*this, entry.module,
                          CommandArguments(arguments_.data() + 1, argc_ - 1));
  }
  template <typename M>
  void Help() {
#if DAVEOS_LOGGING
    scheduler_.log(Level::info, "%s:", M::command_prefix());
    scheduler_.log(Level::info, "  help - list module commands");
    for (const auto& command : M::commands())
      scheduler_.log(Level::info, "  %s - %s", command.name, command.help);
#endif
  }
  template <typename M>
  Status Handle(M& module, CommandArguments args) {
    constexpr auto commands = M::commands();
    if (args.empty()) {
      Help<M>();
      return Status::ok;
    }
    auto match = lazy_match(args[0], commands.size() + 1, [&](std::size_t i) {
      return std::string_view(i == commands.size() ? "help" : commands[i].name);
    });
    if (match.status != Status::ok) return match.status;
    if (match.index == commands.size()) {
      if (args.size() != 1) return Status::invalid_argument;
      Help<M>();
      return Status::ok;
    }
    struct Call {
      M& module;
      Status (M::*callback)(CommandArguments);
      CommandArguments args;
    } call{module, commands[match.index].callback, args.subspan(1)};
    return scheduler_.Invoke(
        {M::name(), commands[match.index].handler},
        [](void* argument) {
          auto& c = *static_cast<Call*>(argument);
          return (c.module.*c.callback)(c.args);
        },
        &call);
  }
  void Error([[maybe_unused]] Status status) {
#if DAVEOS_LOGGING
    const char* message = "handler failed";
    switch (status) {
      case Status::parse_error:
        message = "malformed command line";
        break;
      case Status::ambiguous_match:
        message = "ambiguous command";
        break;
      case Status::not_found:
        message = "unknown command";
        break;
      case Status::line_too_long:
        message = "command line too long";
        break;
      case Status::too_many_arguments:
        message = "too many arguments";
        break;
      case Status::busy:
        message = "command dispatcher busy";
        break;
      case Status::invalid_argument:
        message = "invalid command arguments";
        break;
      default:
        break;
    }
    scheduler_.log(Level::error, "%s (status %s)", message, enum_name(status));
#endif
  }
  SchedulerInterface<Event>& scheduler_;
  std::array<Entry, sizeof...(Modules)> entries_{};
  std::array<char, LineCapacity> buffer_{};
  std::array<std::string_view, ArgumentCapacity> arguments_{};
  std::size_t count_ = 0, argc_ = 0;
  bool busy_ = false, valid_ = true;
};

template <typename... Modules, typename Event>
CommandDispatcher(ModuleList<Modules...>, SchedulerInterface<Event>&)
    -> CommandDispatcher<Event, ModuleList<Modules...>>;


}  // namespace daveos::core
