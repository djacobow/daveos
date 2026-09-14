#pragma once

#include <array>
#include <string_view>

#include "core/schedule/module.hpp"

namespace daveos::core {


// A non-owning complete-line input endpoint, registered when constructing the
// dispatcher. Sources collect bytes themselves and submit only from scheduler
// callbacks. The dispatcher must outlive all submissions. An unbound source
// returns not_running. Source identity/storage must remain stable after
// binding.
class CommandSource {
 public:
  CommandSource() = default;
  CommandSource(const CommandSource&) = delete;
  CommandSource& operator=(const CommandSource&) = delete;
  Status dispatch(std::string_view line) {
    return dispatch_ ? dispatch_(context_, line) : Status::not_running;
  }

 private:
  template <typename, typename, std::size_t, std::size_t>
  friend class CommandDispatcher;
  template <typename Dispatcher>
  void Bind(Dispatcher& dispatcher) {
    context_ = &dispatcher;
    dispatch_ = [](void* context, std::string_view line) {
      return static_cast<Dispatcher*>(context)->dispatch(line);
    };
  }
  void* context_ = nullptr;
  Status (*dispatch_)(void*, std::string_view) = nullptr;
};
// Constructor registration, like SubscriberList. References exclude null
// sources; no allocation or source polling is added to the dispatcher.
template <std::size_t Size>
struct CommandSourceList {
  std::array<CommandSource*, Size> items;
  template <typename... Sources>
  explicit CommandSourceList(Sources&... sources) : items{&sources...} {}
};
template <typename... Sources>
CommandSourceList(Sources&...) -> CommandSourceList<sizeof...(Sources)>;


}  // namespace daveos::core
