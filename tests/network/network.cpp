#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "catch_amalgamated.hpp"
#include "console/tcp.hpp"
#include "core/command/command.hpp"
#include "core/schedule/scheduler.hpp"
#include "net/module.hpp"
#include "net/service.h"
#include "net/tcp_server.h"
#include "net/update.h"
#include "platform/fake/platform.h"
#include "reliability/flash.hpp"
extern "C" {
#include "lwip/pbuf.h"
}


namespace net = daveos::net;
namespace core = daveos::core;

namespace {
  bool track_heap = false;
  unsigned heap_calls = 0;

  struct Packet {
    std::array<std::uint8_t, 1536> bytes{};
    std::size_t size = 0;
  };

  struct Fake {
    std::uint32_t now = 0;
    net::Link state = net::Link::full100;
    bool init_ok = true, tx_ok = true, stopped = false;
    std::array<Packet, 32> rx{}, tx{};
    std::size_t consumed = 0, received = 0, sent = 0;

    net::Driver driver() {
      return {this,
              [](void* p, const net::Mac&) {
                return static_cast<Fake*>(p)->init_ok;
              },
              [](void* p) { static_cast<Fake*>(p)->stopped = true; },
              [](void*) {},
              [](void* p) { return static_cast<Fake*>(p)->state; },
              [](void* p, std::span<std::uint8_t> bytes) -> std::size_t {
                auto& f = *static_cast<Fake*>(p);
                if (f.consumed == f.received) {
                  return 0;
                }
                auto& packet = f.rx[f.consumed++];
                if (packet.size <= bytes.size()) {
                  std::copy_n(packet.bytes.begin(), packet.size, bytes.begin());
                }
                return packet.size;
              },
              [](void* p, std::span<const std::uint8_t> bytes) {
                auto& f = *static_cast<Fake*>(p);
                if (!f.tx_ok || f.sent == f.tx.size()) {
                  return false;
                }
                auto& packet = f.tx[f.sent++];
                packet.size = bytes.size();
                std::copy(bytes.begin(), bytes.end(), packet.bytes.begin());
                return true;
              },
              [](void*) -> std::uint32_t { return 0; }};
    }

    net::Clock clock() {
      return {this, [](void* p) { return static_cast<Fake*>(p)->now; }};
    }

    void queue(const Packet& p) {
      REQUIRE(received < rx.size());
      rx[received++] = p;
    }
  };

  net::Config Static() {
    net::Config c;
    c.dhcp = false;
    return c;
  }

  constexpr net::Mac peer{2, 0, 0, 0, 0, 99};

  void Put16(Packet& p, std::size_t i, unsigned value) {
    p.bytes[i] = value >> 8;
    p.bytes[i + 1] = value;
  }

  std::uint16_t Checksum(const std::uint8_t* p, std::size_t n) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < n; i += 2) {
      sum += (p[i] << 8) | (i + 1 < n ? p[i + 1] : 0);
    }
    while (sum >> 16) {
      sum = (sum & 65535) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum);
  }

  Packet Ethernet(unsigned type, const net::Mac& dest = Static().mac) {
    Packet p;
    std::copy(dest.begin(), dest.end(), p.bytes.begin());
    std::copy(peer.begin(), peer.end(), p.bytes.begin() + 6);
    Put16(p, 12, type);
    return p;
  }

  Packet Arp() {
    auto p = Ethernet(0x806, {255, 255, 255, 255, 255, 255});
    p.size = 42;
    Put16(p, 14, 1);
    Put16(p, 16, 0x800);
    p.bytes[18] = 6;
    p.bytes[19] = 4;
    Put16(p, 20, 1);
    std::copy(peer.begin(), peer.end(), p.bytes.begin() + 22);
    const net::Ipv4 ip{192, 168, 50, 1};
    std::copy(ip.begin(), ip.end(), p.bytes.begin() + 28);
    auto config = Static();
    std::copy(config.address.begin(), config.address.end(),
              p.bytes.begin() + 38);
    return p;
  }

  void Ip(Packet& p, unsigned protocol, const net::Ipv4& dest) {
    p.bytes[14] = 0x45;
    Put16(p, 16, p.size - 14);
    p.bytes[22] = 64;
    p.bytes[23] = protocol;
    const net::Ipv4 ip{192, 168, 50, 1};
    std::copy(ip.begin(), ip.end(), p.bytes.begin() + 26);
    std::copy(dest.begin(), dest.end(), p.bytes.begin() + 30);
    Put16(p, 24, Checksum(p.bytes.data() + 14, 20));
  }

  Packet Ping() {
    auto p = Ethernet(0x800);
    p.size = 46;
    p.bytes[34] = 8;
    Put16(p, 38, 0x1234);
    Put16(p, 40, 1);
    p.bytes[42] = 'p';
    p.bytes[43] = 'i';
    p.bytes[44] = 'n';
    p.bytes[45] = 'g';
    Put16(p, 36, Checksum(p.bytes.data() + 34, 12));
    Ip(p, 1, Static().address);
    return p;
  }

  Packet Dhcp(const Packet& request, unsigned type) {
    auto p = Ethernet(0x800);
    auto* b = p.bytes.data() + 42;
    b[0] = 2;
    b[1] = 1;
    b[2] = 6;
    std::copy_n(request.bytes.begin() + 46, 4, b + 4);
    const net::Ipv4 offered{192, 168, 50, 23};
    std::copy(offered.begin(), offered.end(), b + 16);
    auto mac = Static().mac;
    std::copy(mac.begin(), mac.end(), b + 28);
    const std::uint8_t options[]{
        99,  130, 83,  99,  53, 1, static_cast<std::uint8_t>(type),
        54,  4,   192, 168, 50, 1, 1,
        4,   255, 255, 255, 0,  3, 4,
        192, 168, 50,  1,   51, 4, 0,
        0,   0,   60,  255};
    std::copy(std::begin(options), std::end(options), b + 236);
    p.size = 42 + 236 + sizeof(options);
    Put16(p, 34, 67);
    Put16(p, 36, 68);
    Put16(p, 38, p.size - 34);
    Ip(p, 17, {255, 255, 255, 255});
    return p;
  }
}  // namespace

extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);
extern "C" void __real_free(void*);

extern "C" void* __wrap_malloc(std::size_t n) {
  if (track_heap) {
    ++heap_calls;
  }
  return __real_malloc(n);
}

extern "C" void* __wrap_calloc(std::size_t n, std::size_t s) {
  if (track_heap) {
    ++heap_calls;
  }
  return __real_calloc(n, s);
}

extern "C" void* __wrap_realloc(void* p, std::size_t n) {
  if (track_heap) {
    ++heap_calls;
  }
  return __real_realloc(p, n);
}

extern "C" void __wrap_free(void* p) {
  if (track_heap) {
    ++heap_calls;
  }
  __real_free(p);
}

TEST_CASE("Static IPv4 answers ARP and ping without runtime allocation") {
  Fake f;
  net::Service s(f.driver(), f.clock(), Static());
  heap_calls = 0;
  track_heap = true;
  const bool ok = s.init();
  s.poll();
  track_heap = false;
  REQUIRE(ok);
  CHECK(s.snapshot().state == net::State::ready);
  f.sent = 0;
  f.queue(Arp());
  f.queue(Ping());
  track_heap = true;
  s.poll();
  track_heap = false;
  REQUIRE(f.sent == 2);
  CHECK(f.tx[0].bytes[21] == 2);
  CHECK(f.tx[1].bytes[34] == 0);
  CHECK(Checksum(f.tx[1].bytes.data() + 34, 12) == 0);
  CHECK(f.tx[1].bytes[42] == 'p');
  CHECK(heap_calls == 0);
  track_heap = true;
  s.stop();
  track_heap = false;
  CHECK(heap_calls == 0);
  CHECK(f.stopped);
}

TEST_CASE("DHCP acquires an address and restarts after link loss") {
  Fake f;
  net::Service s(f.driver(), f.clock());
  REQUIRE(s.init());
  s.poll();
  REQUIRE(f.sent > 0);
  auto discover = f.tx[0];
  CHECK(s.snapshot().state == net::State::addressing);
  f.queue(Dhcp(discover, 2));
  s.poll();
  REQUIRE(f.sent >= 2);
  f.queue(Dhcp(discover, 5));
  s.poll();
  CHECK(s.snapshot().address == net::Ipv4{192, 168, 50, 23});
  CHECK(s.snapshot().state == net::State::ready);
  f.state = net::Link::down;
  f.now += 250;
  s.poll();
  CHECK(s.snapshot().address == net::Ipv4{});
  CHECK(s.snapshot().state == net::State::link_down);
  f.state = net::Link::full100;
  f.now += 250;
  s.poll();
  CHECK(s.snapshot().state == net::State::addressing);
}

TEST_CASE("DHCP retries without incoming packets and across clock wrap") {
  Fake f;
  f.now = 0xfffff000U;
  net::Service s(f.driver(), f.clock());
  REQUIRE(s.init());
  s.poll();
  auto count = f.sent;
  for (unsigned i = 0; i < 20; ++i) {
    f.now += 500;
    s.poll();
  }
  CHECK(f.sent > count);
  CHECK(s.snapshot().state == net::State::addressing);
}

TEST_CASE("RX is bounded and packet pool exhaustion recovers") {
  Fake f;
  net::Service s(f.driver(), f.clock(), Static());
  REQUIRE(s.init());
  s.poll();
  for (unsigned i = 0; i < 9; ++i) {
    f.queue(Arp());
  }
  std::array<pbuf*, 32> held{};
  unsigned count = 0;
  while (auto* p = pbuf_alloc(PBUF_RAW, 64, PBUF_POOL)) {
    REQUIRE(count < held.size());
    held[count++] = p;
  }
  REQUIRE(count > 0);
  s.poll();
  CHECK(f.consumed == 4);
  CHECK(s.snapshot().dropped_rx == 4);
  for (unsigned i = 0; i < count; ++i) {
    pbuf_free(held[i]);
  }
  s.poll();
  CHECK(f.consumed == 8);
  CHECK(s.snapshot().rx == 4);
  f.tx_ok = false;
  s.poll();
  CHECK(s.snapshot().dropped_tx > 0);
}

TEST_CASE("Invalid frames do not poison later input") {
  Fake f;
  net::Service s(f.driver(), f.clock(), Static());
  REQUIRE(s.init());
  s.poll();
  Packet oversized;
  oversized.size = 2000;
  f.queue(oversized);
  Packet short_frame;
  short_frame.size = 3;
  f.queue(short_frame);
  f.queue(Arp());
  s.poll();
  CHECK(s.snapshot().dropped_rx >= 1);
  CHECK(f.tx[f.sent - 1].bytes[21] == 2);
}

TEST_CASE("Network configuration is acquired during stage1") {

  using Event = std::variant<std::monostate>;
  Fake driver;
  daveos::platform::fake::Platform platform;
  net::Service service(driver.driver(), driver.clock());
  static int configured = 0;
  configured = 0;
  daveos::net::Module<Event> network(service, [] {
    ++configured;
    auto config = Static();
    config.mac = peer;
    return config;
  });
  auto scheduler =
      core::make_scheduler<Event>(platform, core::ModuleList{&network});
  CHECK(configured == 0);
  CHECK(scheduler.init() == core::Status::ok);
  CHECK(configured == 1);
  CHECK(service.snapshot().mac == peer);
  CHECK_FALSE(service.init(Static()));
  CHECK(service.snapshot().mac == peer);
  CHECK(scheduler.init() == core::Status::already_initialized);
  CHECK(configured == 1);
}

TEST_CASE("Networking hardware failure leaves unrelated module running") {
  using Event = std::variant<std::monostate>;
  Fake driver;
  driver.init_ok = false;
  net::Service s(driver.driver(), driver.clock());
  daveos::platform::fake::Platform platform;
  daveos::net::Module<Event> network(s);

  struct Other : daveos::core::Module<Other, Event> {
    bool called = false;
    core::CommandSource source;
    core::Status status_result{}, extra_result{};

    static constexpr const char* name() { return "other"; }

    static constexpr auto tasks() {
      return std::array{
          daveos::core::TaskDescriptor<Other>{"run", &Other::Run}};
    }

    void Run() {
      called = true;
      status_result = source.dispatch("net status");
      extra_result = source.dispatch("net status extra");
      scheduler().stop();
    }
  } other;

  auto modules = core::ModuleList{&network, &other};
  auto scheduler = core::make_scheduler<Event>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  dispatcher.bind_sources(core::CommandSourceList{other.source});
  REQUIRE(scheduler.schedule(other, &Other::Run, 1) ==
          daveos::core::Status::ok);
  REQUIRE(scheduler.run() == daveos::core::Status::ok);
  CHECK(other.called);
  CHECK(other.status_result == core::Status::ok);
  CHECK(other.extra_result == core::Status::invalid_argument);
  CHECK(s.snapshot().state == net::State::hardware_fault);
  CHECK_FALSE(s.init());
}

// Exercise TCP through real Ethernet/IP/TCP packets rather than calling raw
// lwIP callbacks: handshake, competing client, fragmentation, and reconnect.
namespace {
  std::uint32_t Get32(const Packet& p, std::size_t i) {
    return (std::uint32_t(p.bytes[i]) << 24) |
           (std::uint32_t(p.bytes[i + 1]) << 16) |
           (std::uint32_t(p.bytes[i + 2]) << 8) | p.bytes[i + 3];
  }

  void Put32(Packet& p, std::size_t i, std::uint32_t n) {
    Put16(p, i, n >> 16);
    Put16(p, i + 2, n & 65535);
  }

  Packet Tcp(std::uint16_t port, std::uint32_t seq, std::uint32_t ack,
             std::uint8_t flags, std::string_view text = {}) {
    auto p = Ethernet(0x800);
    p.size = 54 + text.size();
    Put16(p, 34, port);
    Put16(p, 36, 1000);
    Put32(p, 38, seq);
    Put32(p, 42, ack);
    p.bytes[46] = 0x50;
    p.bytes[47] = flags;
    Put16(p, 48, 4096);
    std::copy(text.begin(), text.end(), p.bytes.begin() + 54);
    Ip(p, 6, Static().address);
    Packet pseudo;
    std::copy_n(p.bytes.begin() + 26, 8, pseudo.bytes.begin());
    pseudo.bytes[9] = 6;
    Put16(pseudo, 10, p.size - 34);
    std::copy(p.bytes.begin() + 34, p.bytes.begin() + p.size,
              pseudo.bytes.begin() + 12);
    Put16(p, 50, Checksum(pseudo.bytes.data(), p.size - 22));
    return p;
  }

  std::uint32_t Connect(Fake& f, net::Service& service, std::uint16_t port) {
    f.queue(Tcp(port, 100, 0, 2));
    service.poll();
    REQUIRE(f.sent != 0);
    auto& response = f.tx[f.sent - 1];
    REQUIRE(response.bytes[47] == 0x12);
    auto seq = Get32(response, 38);
    f.queue(Tcp(port, 101, seq + 1, 0x10));
    service.poll();
    return seq + 1;
  }
}  // namespace

TEST_CASE("TCP server isolates clients and bounds session buffers") {
  Fake f;
  net::Service service(f.driver(), f.clock(), Static());
  REQUIRE(service.init());
  service.poll();
  net::TcpServer server(service);
  server.poll();
  f.queue(Arp());  // Teach the stack our test peer's MAC.
  service.poll();
  auto ack = Connect(f, service, 40000);
  REQUIRE(server.connected());
  auto session = server.session();
  Connect(f, service, 40001);
  REQUIRE(server.connected());
  REQUIRE(server.session() == session);
  REQUIRE((f.tx[f.sent - 1].bytes[47] & 4) != 0);  // Second client reset.

  f.queue(Tcp(40000, 101, ack, 0x18, "he"));
  service.poll();
  f.queue(Tcp(40000, 103, ack, 0x18, "lp\r\n"));
  service.poll();
  std::array<char, 16> input{};
  REQUIRE(server.read(input) == 6);
  REQUIRE(std::string_view(input.data(), 6) == "help\r\n");
  const std::array<std::string_view, 2> output{"hello", "\r\n"};
  REQUIRE(server.write(output));
  track_heap = true;
  server.poll();
  track_heap = false;
  REQUIRE(heap_calls == 0);
  auto& reply = f.tx[f.sent - 1];
  REQUIRE(std::string_view(
              reinterpret_cast<const char*>(reply.bytes.data() + 54), 7) ==
          "hello\r\n");
  std::array<char, 8192> large{};
  const std::array<std::string_view, 1> full{
      std::string_view(large.data(), large.size())};
  REQUIRE(server.write(full));
  REQUIRE_FALSE(server.write(output));
  REQUIRE(server.dropped_output() == 1);
  f.queue(Tcp(40000, 107, ack + 7, 0x14));
  service.poll();
  REQUIRE_FALSE(server.connected());
  REQUIRE(server.session() != session);
  Connect(f, service, 40002);
  REQUIRE(server.read(input) == 0);
  REQUIRE(
      server.write(full));  // Previous session's queued output was discarded.
  f.state = net::Link::down;
  f.now += 250;
  service.poll();
  server.poll();
  REQUIRE_FALSE(server.connected());
  f.state = net::Link::full100;
  f.now += 250;
  service.poll();
  server.poll();
  ack = Connect(f, service, 40003);
  REQUIRE(server.connected());
  std::array<char, 1460> payload{};
  payload.fill('x');
  const std::string_view chunk(payload.data(), payload.size());
  f.queue(Tcp(40003, 101, ack, 0x18, chunk));
  f.queue(Tcp(40003, 1561, ack, 0x18, chunk));
  f.queue(Tcp(40003, 3021, ack, 0x18, chunk.substr(0, 1176)));
  service.poll();
  server.consume(3000);
  f.queue(Tcp(40003, 4197, ack, 0x18, chunk.substr(0, 1000)));
  service.poll();
  REQUIRE(server.peek().size() == 1096);
  std::array<char, 2096> received;
  REQUIRE(server.read(received) == received.size());
  REQUIRE(std::all_of(received.begin(), received.end(),
                      [](char c) { return c == 'x'; }));
  f.queue(Tcp(40003, 5197, ack, 0x11));  // FIN, rather than RST.
  service.poll();
  REQUIRE_FALSE(server.connected());
}

namespace {
  using ConsoleEvent = std::variant<std::monostate>;
  using TestPlatform = daveos::platform::fake::Platform;

  struct Receiver : daveos::core::Module<Receiver, ConsoleEvent> {
    explicit Receiver(TestPlatform& p) : platform(p) {}

    static constexpr const char* name() { return "sink"; }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Receiver, Add, "add", "Record invocation")};
    }

    daveos::core::Status Add(daveos::core::CommandArguments) {
      times.at(count++) = platform.now();
      if (count == times.size()) {
        scheduler().stop();
      }
      return daveos::core::Status::ok;
    }

    TestPlatform& platform;
    std::array<daveos::core::Time, 3> times{};
    std::size_t count = 0;
  };
}  // namespace

TEST_CASE(
    "TCP console dispatches one command per tick and preserves burst tails") {
  Fake f;
  net::Service service(f.driver(), f.clock(), Static());
  REQUIRE(service.init());
  service.poll();
  TestPlatform platform;
  daveos::console::TcpConsole<ConsoleEvent, TestPlatform> console(platform,
                                                                  service);
  daveos::console::Line line;
  REQUIRE_FALSE(console.poll_line(line));  // Open listener.
  f.queue(Arp());
  service.poll();
  const auto ack = Connect(f, service, 40000);
  const std::string_view burst = "sink add\r\nsink add\nsink add\nsink ";
  f.queue(Tcp(40000, 101, ack, 0x18, burst));
  service.poll();
  Receiver receiver(platform);

  auto modules = core::ModuleList{&console, &receiver};
  auto scheduler = core::make_scheduler<ConsoleEvent>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  dispatcher.bind_sources(core::CommandSourceList{console.command_source()});
  REQUIRE(scheduler.run() == core::Status::ok);
  REQUIRE(receiver.count == 3);
  CHECK(receiver.times == std::array<core::Time, 3>{1000, 2000, 3000});
  REQUIRE_FALSE(console.poll_line(line));  // Collect unfinished fourth command.
  f.queue(Tcp(40000, 101 + burst.size(), ack, 0x18, "add\n"));
  service.poll();
  REQUIRE(console.poll_line(line));
  CHECK(line.view() == "sink add");
}

TEST_CASE("Network stop and reconnect cannot abort a local update owner") {
  namespace update = daveos::update;
  reliability::MemoryFlash flash;
  auto layout = flash.layout();
  daveos::boot::Snapshot factory;
  factory.counter = 1;
  factory.images[0] = {1, 16, 0, daveos::boot::ImageState::confirmed, 1, 1};
  daveos::boot::Journal journal(flash.driver(), layout);
  REQUIRE(journal.commit(factory, 1000000, true) == core::Status::ok);
  std::ifstream package(REFERENCE_PACKAGE, std::ios::binary);
  std::array<std::byte, 128> header{};
  package.read(reinterpret_cast<char*>(header.data()), header.size());
  REQUIRE(package.good());
  update::Engine engine(flash.driver(), layout, 0);
  std::uint8_t owner = 0;
  REQUIRE(engine.reserve(&owner) == core::Status::ok);
  REQUIRE(engine.begin(header, &owner) == core::Status::ok);
  for (unsigned i = 0;
       i < 1000 && engine.state() != update::Engine::State::receiving; ++i) {
    engine.tick();
  }
  REQUIRE(engine.state() == update::Engine::State::receiving);
  Fake f;
  net::Service service(f.driver(), f.clock(), Static());
  REQUIRE(service.init());
  service.poll();
  net::UpdateServer server(service, engine, {}, 1000);
  server.stop();
  server.poll();
  engine.tick();
  CHECK(engine.state() == update::Engine::State::receiving);
  engine.enable(true);
  server.poll();
  f.queue(Arp());
  service.poll();
  Connect(f, service, 40000);
  server.poll();
  engine.tick();
  CHECK(engine.state() == update::Engine::State::receiving);
  server.stop();
  engine.tick();
  CHECK(engine.state() == update::Engine::State::receiving);
  CHECK(engine.reserved());
}
