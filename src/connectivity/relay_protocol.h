#pragma once

#include <cstddef>
#include <cstdint>

// Contrato wire compartilhado com Android. Sem Arduino, heap ou referências
// a buffers externos. O chamador serializa o acesso às instâncias mutáveis.
namespace PhoneRelay {
namespace Protocol {

constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 7;
constexpr size_t kMaxFrame = 180;
constexpr size_t kMaxPayload = kMaxFrame - kHeaderSize;
constexpr uint16_t kMinMtu = 183;
constexpr uint8_t kChannels = 4;
constexpr uint8_t kWindow = 8;
constexpr uint16_t kTlsPort = 443;

enum class Type : uint8_t { Open = 1, Opened, Data, Ack, Close, Error };
enum class Validation : uint8_t {
    Ok, Null, Size, Version, Type, Channel, Payload, Sequence, Port, Utf8
};

struct Frame {
    Type type = Type::Close;
    uint8_t channel = 0;
    uint16_t epoch = 0;
    uint16_t seq = 0;
    size_t length = 0;
    uint8_t payload[kMaxPayload]{};
};

bool validUtf8(const uint8_t* data, size_t length);
Validation validate(const Frame& frame);
// Uma operação GATT contém exatamente um frame; não há campo length no wire.
// Falha não altera o destino. encode retorna 0 na falha.
Validation decode(const uint8_t* wire, size_t size, Frame& frame);
size_t encode(const Frame& frame, uint8_t* wire, size_t capacity);
bool makeOpen(uint8_t channel, uint16_t epoch, const char* host, uint16_t port, Frame& frame);

class TxWindow {
public:
    void reset(uint16_t first = 0);
    void cancel();
    bool reserve(uint16_t& sequence);
    // ACK individual, estritamente em ordem, nunca cumulativo.
    bool acknowledge(uint16_t sequence);
    uint8_t pending() const { return count_; }
    uint8_t credits() const { return enabled_ ? kWindow - count_ : 0; }

private:
    uint16_t next_ = 0;
    uint16_t oldest_ = 0;
    uint8_t count_ = 0;
    bool enabled_ = false;
};

struct Consumption {
    size_t bytes = 0;
    bool ack = false;
    uint16_t sequence = 0;
};

class RxWindow {
public:
    void reset(uint16_t first = 0);
    void cancel();
    bool push(uint16_t sequence, const uint8_t* data, size_t length);
    // Consome no máximo um frame. ACK somente ao drenar inteiramente o slot.
    // Um DATA vazio é consumido com consume(nullptr, 0).
    Consumption consume(uint8_t* out, size_t capacity);
    size_t available() const;
    int peek() const;
    uint8_t slots() const { return count_; }

private:
    struct Slot {
        uint8_t bytes[kMaxPayload]{};
        uint16_t sequence = 0;
        uint16_t length = 0;
        uint16_t offset = 0;
    };
    Slot slots_[kWindow]{};
    uint16_t next_ = 0;
    uint8_t head_ = 0;
    uint8_t count_ = 0;
    bool enabled_ = false;
};

enum class State : uint8_t { Idle, Opening, Open, PeerClosed, Aborted };
enum class Reception : uint8_t { Accepted, StaleEpoch, RemoteClosed, RemoteError, Fault };

class Channel {
public:
    bool start(uint8_t channel);
    void release();
    void abort();
    Reception receive(const Frame& frame);
    bool prepareData(const uint8_t* data, size_t size, Frame& frame);
    Consumption consume(uint8_t* out, size_t capacity) { return rx_.consume(out, capacity); }
    size_t available() const { return rx_.available(); }
    int peek() const { return rx_.peek(); }
    uint8_t credits() const { return state_ == State::Open ? tx_.credits() : 0; }
    uint8_t pending() const { return tx_.pending(); }
    State state() const { return state_; }
    uint16_t epoch() const { return epoch_; }
    uint8_t id() const { return id_; }
    // Token local de cancelamento, independente do contador wire de 16 bits.
    uint32_t generation() const { return generation_; }

private:
    TxWindow tx_;
    RxWindow rx_;
    State state_ = State::Idle;
    uint8_t id_ = 0;
    uint16_t epoch_ = 0;
    uint32_t generation_ = 0;
};

} // namespace Protocol
} // namespace PhoneRelay
