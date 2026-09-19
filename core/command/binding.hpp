#pragma once

#include "source.hpp"

namespace daveos::core {


  // Optional wiring module. Construction only copies source pointers. Connect
  // a dispatcher before init/run; stage2 binds sources after all stage1 hooks.
  // The dispatcher and source objects must outlive this module's use. The
  // module name "commands" must be unique in the application.
  template <typename Event, std::size_t Sources>
  class CommandBinding final
      : public Module<CommandBinding<Event, Sources>, Event> {
   public:
    explicit CommandBinding(CommandSourceList<Sources> sources)
        : sources_(sources) {}

    static constexpr const char* name() { return "commands"; }

    template <typename Dispatcher>
    void connect(Dispatcher& dispatcher) {
      dispatcher_ = &dispatcher;
      bind_ = [](void* context, CommandSourceList<Sources> sources) {
        static_cast<Dispatcher*>(context)->bind_sources(sources);
      };
    }

    Status init(InitStage stage) {
      if (stage == InitStage::stage2) {
        if (!bind_) {
          return Status::not_running;
        }
        bind_(dispatcher_, sources_);
      }
      return Status::ok;
    }

   private:
    CommandSourceList<Sources> sources_;
    void* dispatcher_ = nullptr;
    void (*bind_)(void*, CommandSourceList<Sources>) = nullptr;
  };

  template <typename Event, std::size_t Sources>
  auto make_command_binding(CommandSourceList<Sources> sources) {
    return CommandBinding<Event, Sources>(sources);
  }


}  // namespace daveos::core
