#include "relay_protocol.h"

#include <cstring>

namespace PhoneRelay {
namespace Protocol {
namespace {
uint16_t load16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
void store16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}
} // namespace

bool validUtf8(const uint8_t* data, size_t length) {
    if (!data && length) return false;
    for (size_t i = 0; i < length;) {
        const uint8_t lead = data[i++];
        if (lead < 0x80) continue;
        uint32_t point;
        size_t tail;
        uint32_t minimum;
        if (lead >= 0xc2 && lead <= 0xdf) {
            point = lead & 0x1f; tail = 1; minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            point = lead & 0x0f; tail = 2; minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            point = lead & 0x07; tail = 3; minimum = 0x10000;
        } else {
            return false;
        }
        if (length - i < tail) return false;
        while (tail--) {
            const uint8_t byte = data[i++];
            if ((byte & 0xc0) != 0x80) return false;
            point = (point << 6) | (byte & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
    }
    return true;
}

Validation validate(const Frame& f) {
    if (f.channel >= kChannels) return Validation::Channel;
    if (f.length > kMaxPayload) return Validation::Size;
    switch (f.type) {
    case Type::Open:
        if (f.seq != 0) return Validation::Sequence;
        if (f.length < 3) return Validation::Payload;
        if (load16(f.payload) != kTlsPort) return Validation::Port;
        for (size_t i = 2; i < f.length; ++i) {
            if (f.payload[i] <= 0x20 || f.payload[i] == 0x7f) return Validation::Payload;
        }
        return validUtf8(f.payload + 2, f.length - 2) ? Validation::Ok : Validation::Utf8;
    case Type::Opened:
        if (f.seq != 0) return Validation::Sequence;
        return f.length == 0 ? Validation::Ok : Validation::Payload;
    case Type::Ack:
    case Type::Close:
        return f.length == 0 ? Validation::Ok : Validation::Payload;
    case Type::Data:
        return Validation::Ok; // Inclusive DATA vazio: nenhum byte a entregar.
    case Type::Error:
        return validUtf8(f.payload, f.length) ? Validation::Ok : Validation::Utf8;
    }
    return Validation::Type;
}

Validation decode(const uint8_t* wire, size_t size, Frame& frame) {
    if (!wire) return Validation::Null;
    if (size < kHeaderSize || size > kMaxFrame) return Validation::Size;
    if (wire[0] != kVersion) return Validation::Version;
    Frame candidate;
    candidate.type = static_cast<Type>(wire[1]);
    candidate.channel = wire[2];
    candidate.epoch = load16(wire + 3);
    candidate.seq = load16(wire + 5);
    candidate.length = size - kHeaderSize;
    if (candidate.length) std::memcpy(candidate.payload, wire + kHeaderSize, candidate.length);
    const Validation result = validate(candidate);
    if (result == Validation::Ok) frame = candidate;
    return result;
}

size_t encode(const Frame& frame, uint8_t* wire, size_t capacity) {
    if (!wire || validate(frame) != Validation::Ok || capacity < kHeaderSize + frame.length) return 0;
    wire[0] = kVersion;
    wire[1] = static_cast<uint8_t>(frame.type);
    wire[2] = frame.channel;
    store16(wire + 3, frame.epoch);
    store16(wire + 5, frame.seq);
    if (frame.length) std::memcpy(wire + kHeaderSize, frame.payload, frame.length);
    return kHeaderSize + frame.length;
}

bool makeOpen(uint8_t channel, uint16_t epoch, const char* host, uint16_t port, Frame& frame) {
    if (!host || port != kTlsPort) return false;
    size_t size = 0;
    while (size <= kMaxPayload - 2 && host[size]) ++size;
    if (!size || size > kMaxPayload - 2) return false;
    Frame candidate;
    candidate.type = Type::Open;
    candidate.channel = channel;
    candidate.epoch = epoch;
    candidate.length = 2 + size;
    store16(candidate.payload, port);
    std::memcpy(candidate.payload + 2, host, size);
    if (validate(candidate) != Validation::Ok) return false;
    frame = candidate;
    return true;
}

void TxWindow::reset(uint16_t first) {
    next_ = oldest_ = first;
    count_ = 0;
    enabled_ = true;
}
void TxWindow::cancel() {
    enabled_ = false;
    count_ = 0;
}
bool TxWindow::reserve(uint16_t& sequence) {
    if (!enabled_ || count_ == kWindow) return false;
    sequence = next_++;
    ++count_;
    return true;
}
bool TxWindow::acknowledge(uint16_t sequence) {
    if (!enabled_ || !count_ || sequence != oldest_) return false;
    ++oldest_;
    --count_;
    return true;
}

void RxWindow::reset(uint16_t first) {
    next_ = first;
    head_ = count_ = 0;
    enabled_ = true;
}
void RxWindow::cancel() {
    enabled_ = false;
    head_ = count_ = 0;
    // Remove os bytes de respostas potencialmente sensíveis ao cancelar.
    for (auto& slot : slots_) slot = Slot{};
}
bool RxWindow::push(uint16_t sequence, const uint8_t* data, size_t length) {
    if (!enabled_ || count_ == kWindow || sequence != next_ || length > kMaxPayload || (!data && length)) return false;
    Slot& slot = slots_[(head_ + count_) % kWindow];
    slot.sequence = sequence;
    slot.length = static_cast<uint16_t>(length);
    slot.offset = 0;
    if (length) std::memcpy(slot.bytes, data, length);
    ++next_;
    ++count_;
    return true;
}
Consumption RxWindow::consume(uint8_t* out, size_t capacity) {
    Consumption result;
    if (!enabled_ || !count_ || (!out && capacity)) return result;
    Slot& slot = slots_[head_];
    const size_t left = slot.length - slot.offset;
    result.bytes = left < capacity ? left : capacity;
    if (result.bytes) std::memcpy(out, slot.bytes + slot.offset, result.bytes);
    slot.offset = static_cast<uint16_t>(slot.offset + result.bytes);
    if (slot.offset == slot.length) {
        result.ack = true;
        result.sequence = slot.sequence;
        slot = Slot{};
        head_ = (head_ + 1) % kWindow;
        --count_;
    }
    return result;
}
size_t RxWindow::available() const {
    size_t result = 0;
    for (uint8_t i = 0; i < count_; ++i) {
        const Slot& slot = slots_[(head_ + i) % kWindow];
        result += slot.length - slot.offset;
    }
    return result;
}
int RxWindow::peek() const {
    for (uint8_t i = 0; i < count_; ++i) {
        const Slot& slot = slots_[(head_ + i) % kWindow];
        if (slot.offset < slot.length) return slot.bytes[slot.offset];
    }
    return -1;
}

bool Channel::start(uint8_t channel) {
    if (state_ != State::Idle || channel >= kChannels) return false;
    id_ = channel;
    ++epoch_; // Contador wire u16, também preservado através de release/stop.
    ++generation_;
    tx_.reset();
    rx_.reset();
    state_ = State::Opening;
    return true;
}
void Channel::release() {
    abort();
    state_ = State::Idle;
}
void Channel::abort() {
    tx_.cancel();
    rx_.cancel();
    state_ = State::Aborted;
    ++generation_;
}
Reception Channel::receive(const Frame& frame) {
    // Filtrar epochs antigos antes de interpretar conteúdo/sequência.
    if (frame.epoch != epoch_) return Reception::StaleEpoch;
    if (frame.channel != id_ || validate(frame) != Validation::Ok) {
        abort();
        return Reception::Fault;
    }
    if (state_ == State::Idle || state_ == State::Aborted) return Reception::Fault;
    switch (frame.type) {
    case Type::Opened:
        if (state_ == State::Opening) {
            state_ = State::Open;
            return Reception::Accepted;
        }
        break;
    case Type::Data:
        if (state_ == State::Open && rx_.push(frame.seq, frame.payload, frame.length)) return Reception::Accepted;
        break;
    case Type::Ack:
        if (state_ == State::Open && tx_.acknowledge(frame.seq)) return Reception::Accepted;
        break;
    case Type::Close:
        // CLOSE normal preserva RX até o consumidor observar EOF e chamar close.
        if (state_ != State::PeerClosed && tx_.pending() == 0) {
            state_ = State::PeerClosed;
            return Reception::RemoteClosed;
        }
        break;
    case Type::Error:
        abort();
        return Reception::RemoteError;
    case Type::Open: // OPEN é exclusivamente ESP -> Android.
        break;
    }
    abort();
    return Reception::Fault;
}
bool Channel::prepareData(const uint8_t* data, size_t size, Frame& frame) {
    if (state_ != State::Open || size > kMaxPayload || (!data && size)) return false;
    Frame candidate;
    candidate.type = Type::Data;
    candidate.channel = id_;
    candidate.epoch = epoch_;
    candidate.length = size;
    if (!tx_.reserve(candidate.seq)) return false;
    if (size) std::memcpy(candidate.payload, data, size);
    frame = candidate;
    return true;
}

} // namespace Protocol
} // namespace PhoneRelay
