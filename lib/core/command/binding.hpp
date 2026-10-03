#pragma once

#include "source.hpp"

namespace daveos::core {


  // Optional wiring module. Construction only copies source pointers. Connect
  // a dispatcher before init/run; stage2 binds sources after all stage1 hooks
  // and fails initialization with the dispatcher's status if its command routes
  // are invalid or duplicated (no source is bound then). The dispatcher and
  // source objects must outlive this module's use. The instance name defaults
  // to "commands" and must be unique.
  template <typename Event, std::size_t Sources>
  class CommandBinding final
      : public Module<CommandBinding<Event, Sources>, Event> {
   public:
    explicit CommandBinding(CommandSourceList<Sources> sources,
                            const char* name = nullptr)
        : Module<CommandBinding<Event, Sources>, Event>(name),
          sources_(sources) {}

    static constexpr const char* name() { return "commands"; }

    template <typename Dispatcher>
    void connect(Dispatcher& dispatcher) {
      dispatcher_ = &dispatcher;
      bind_ = [](void* context, CommandSourceList<Sources> sources) {
        return static_cast<Dispatcher*>(context)->bind_sources(sources);
      };
    }

    Status init(InitStage stage) {
      if (stage == InitStage::stage2) {
        if (!bind_) {
          return Status::not_running;
        }
        return bind_(dispatcher_, sources_);
      }
      return Status::ok;
    }

   private:
    CommandSourceList<Sources> sources_;
    void* dispatcher_ = nullptr;
    Status (*bind_)(void*, CommandSourceList<Sources>) = nullptr;
  };

  template <typename Event, std::size_t Sources>
  auto make_command_binding(CommandSourceList<Sources> sources) {
    return CommandBinding<Event, Sources>(sources);
  }


}  // namespace daveos::core
