#include "driver.h"

#include <algorithm>
#include <cstring>

#include "ethernet_board.h"
#include "lan8742.h"

namespace net = daveos::net;

namespace {

static_assert(ETH_RX_DESC_CNT == 8 && ETH_TX_DESC_CNT == 4);

struct alignas(32) Rx {
  std::array<std::uint8_t, 1536> bytes;
  Rx* next;
  std::size_t size;
  bool used;
};

struct alignas(32) Tx {
  std::array<std::uint8_t, 1536> bytes;
  bool used;
};

struct alignas(32) Dma {
  ETH_DMADescTypeDef rx_desc[ETH_RX_DESC_CNT];
  ETH_DMADescTypeDef tx_desc[ETH_TX_DESC_CNT];
  Rx rx[16];  // Eight attached to DMA, eight available during RX replacement.
  Tx tx[ETH_TX_DESC_CNT];
};

Dma dma __attribute__((section(".eth_dma")));
static_assert(sizeof(dma) <= 65536);
ETH_HandleTypeDef eth;
lan8742_Object_t phy;
net::Mac mac;
net::Link current = net::Link::down;
bool initialized = false, running = false;
volatile std::uint32_t error_count = 0;
volatile bool fault = false;

std::int32_t IoInit() {
  HAL_ETH_SetMDIOClockRange(&eth);
  return 0;
}

std::int32_t IoDeInit() { return 0; }

std::int32_t IoRead(std::uint32_t addr, std::uint32_t reg,
                    std::uint32_t* value) {
  return HAL_ETH_ReadPHYRegister(&eth, addr, reg, value) == HAL_OK ? 0 : -1;
}

std::int32_t IoWrite(std::uint32_t addr, std::uint32_t reg,
                     std::uint32_t value) {
  return HAL_ETH_WritePHYRegister(&eth, addr, reg, value) == HAL_OK ? 0 : -1;
}

std::int32_t IoTick() { return static_cast<std::int32_t>(HAL_GetTick()); }

bool InitMac() {
  // No DMA is running here. Reset all descriptors and buffer ownership
  // together.
  std::memset(&dma, 0, sizeof(dma));
  eth = {};
  eth.Instance = ETH;
  eth.Init.MACAddr = mac.data();
  eth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  eth.Init.RxDesc = dma.rx_desc;
  eth.Init.TxDesc = dma.tx_desc;
  eth.Init.RxBuffLen = 1536;
  return HAL_ETH_Init(&eth) == HAL_OK;
}

void Stop(void*) {
  if (running) HAL_ETH_Stop_IT(&eth);
  if (initialized) HAL_ETH_DeInit(&eth);
  initialized = running = false;
  current = net::Link::down;
}

bool Init(void*, const net::Mac& address) {
  mac = address;
  fault = false;
  error_count = 0;
  phy = {};
  if (!InitMac()) {
    HAL_ETH_DeInit(&eth);
    return false;
  }
  initialized = true;
  lan8742_IOCtx_t io{IoInit, IoDeInit, IoWrite, IoRead, IoTick};
  if (LAN8742_RegisterBusIO(&phy, &io) != LAN8742_STATUS_OK ||
      LAN8742_Init(&phy) != LAN8742_STATUS_OK) {
    Stop(nullptr);
    return false;
  }
  return true;
}

net::Link ReadLink(void*) {
  if (!initialized || fault) return net::Link::fault;
  auto state = LAN8742_GetLinkState(&phy);
  net::Link next = net::Link::down;
  switch (state) {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX:
      next = net::Link::full100;
      break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX:
      next = net::Link::half100;
      break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:
      next = net::Link::full10;
      break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:
      next = net::Link::half10;
      break;
    default:
      if (state < 0) next = net::Link::fault;
      break;
  }
  if (next == current) return current;
  if (running) {
    HAL_ETH_Stop_IT(&eth);
    running = false;
  }
  if (next != net::Link::down && next != net::Link::fault) {
    // Reset descriptor ownership on reconnect, discarding stale queued frames.
    HAL_ETH_DeInit(&eth);
    if (!InitMac()) {
      fault = true;
      return net::Link::fault;
    }
    ETH_MACConfigTypeDef config{};
    HAL_ETH_GetMACConfig(&eth, &config);
    config.Speed = (next == net::Link::full100 || next == net::Link::half100)
                       ? ETH_SPEED_100M
                       : ETH_SPEED_10M;
    config.DuplexMode =
        (next == net::Link::full100 || next == net::Link::full10)
            ? ETH_FULLDUPLEX_MODE
            : ETH_HALFDUPLEX_MODE;
    config.ChecksumOffload =
        DISABLE;  // lwIP performs all checksums in software.
    if (HAL_ETH_SetMACConfig(&eth, &config) != HAL_OK ||
        HAL_ETH_Start_IT(&eth) != HAL_OK) {
      fault = true;
      return net::Link::fault;
    }
    running = true;
  }
  current = next;
  return current;
}

void Poll(void*) {
  if (fault && running) {
    HAL_ETH_Stop_IT(&eth);
    running = false;
  }
  if (running) HAL_ETH_ReleaseTxPacket(&eth);
}

std::size_t Receive(void*, std::span<std::uint8_t> bytes) {
  if (!running) return 0;
  Rx* frame = nullptr;
  if (HAL_ETH_ReadData(&eth, reinterpret_cast<void**>(&frame)) != HAL_OK ||
      !frame)
    return 0;
  std::size_t total = 0;
  for (auto* part = frame; part;) {
    if (total <= bytes.size() && part->size <= part->bytes.size() &&
        part->size <= bytes.size() - total)
      std::copy_n(part->bytes.begin(), part->size, bytes.begin() + total);
    total += part->size;
    auto* next = part->next;
    part->used = false;
    part = next;
  }
  return total;
}

bool Transmit(void*, std::span<const std::uint8_t> bytes) {
  if (!running || bytes.size() > 1536) return false;
  HAL_ETH_ReleaseTxPacket(&eth);
  for (auto& slot : dma.tx) {
    if (slot.used) continue;
    std::copy(bytes.begin(), bytes.end(), slot.bytes.begin());
    slot.used = true;
    ETH_BufferTypeDef buffer{};
    buffer.buffer = slot.bytes.data();
    buffer.len = bytes.size();
    ETH_TxPacketConfigTypeDef packet{};
    packet.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
    packet.CRCPadCtrl = ETH_CRC_PAD_INSERT;
    packet.Length = bytes.size();
    packet.TxBuffer = &buffer;
    packet.pData = &slot;
    __DMB();
    if (HAL_ETH_Transmit_IT(&eth, &packet) == HAL_OK) return true;
    slot.used = false;
    return false;
  }
  return false;
}
}  // namespace

namespace daveos::net::stm32 {


net::Driver ethernet_driver() {
  return {nullptr,  Init,
          Stop,     Poll,
          ReadLink, Receive,
          Transmit, [](void*) -> std::uint32_t { return error_count; }};
}

net::Config board_network_config() {
  net::Config config;
  // Locally administered unicast MAC derived from all three UID words.
  const std::uint32_t words[]{HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
  std::uint32_t hash = 2166136261U;
  for (auto word : words)
    for (unsigned n = 0; n < 4; ++n) {
      hash = (hash ^ ((word >> (8 * n)) & 255)) * 16777619U;
    }
  config.mac = {2,
                static_cast<std::uint8_t>(words[0] >> 8),
                static_cast<std::uint8_t>(hash >> 24),
                static_cast<std::uint8_t>(hash >> 16),
                static_cast<std::uint8_t>(hash >> 8),
                static_cast<std::uint8_t>(hash)};
  return config;
}


}  // namespace daveos::net::stm32

extern "C" void ETH_IRQHandler() { HAL_ETH_IRQHandler(&eth); }

extern "C" void HAL_ETH_RxAllocateCallback(std::uint8_t** bytes) {
  *bytes = nullptr;
  for (auto& slot : dma.rx)
    if (!slot.used) {
      slot.used = true;
      slot.next = nullptr;
      slot.size = 0;
      *bytes = slot.bytes.data();
      return;
    }
}

extern "C" void HAL_ETH_RxLinkCallback(void** start, void** end,
                                       std::uint8_t* bytes,
                                       std::uint16_t size) {
  auto* slot = reinterpret_cast<Rx*>(bytes);
  slot->size = size;
  slot->next = nullptr;
  // ReadData clears only the head after handing a packet to the caller. The
  // retained tail belongs to the previous packet until a new head is set.
  if (*start)
    static_cast<Rx*>(*end)->next = slot;
  else
    *start = slot;
  *end = slot;
}

extern "C" void HAL_ETH_TxFreeCallback(std::uint32_t* data) {
  static_cast<Tx*>(static_cast<void*>(data))->used = false;
}

extern "C" void HAL_ETH_ErrorCallback(ETH_HandleTypeDef*) {
  error_count = error_count + 1;
  // Fatal bus errors require a reset; RX-buffer-unavailable is recoverable.
  if (eth.DMAErrorCode & ETH_DMACSR_FBE) fault = true;
}
