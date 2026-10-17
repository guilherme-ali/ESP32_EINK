#include "../../src/connectivity/relay_protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#if defined(NDEBUG)
#error "relay_protocol_tests requer asserts ativos; compile sem NDEBUG"
#endif

namespace P = PhoneRelay::Protocol;

namespace {
P::Frame frame(P::Type type, uint8_t channel = 0, uint16_t epoch = 1, uint16_t seq = 0) {
    P::Frame result;
    result.type = type;
    result.channel = channel;
    result.epoch = epoch;
    result.seq = seq;
    return result;
}
P::Frame data(uint8_t channel, uint16_t epoch, uint16_t seq, const char* text) {
    auto result = frame(P::Type::Data, channel, epoch, seq);
    result.length = std::strlen(text);
    assert(result.length <= P::kMaxPayload);
    std::memcpy(result.payload, text, result.length);
    return result;
}
void establish(P::Channel& channel, uint8_t id = 0) {
    assert(channel.start(id));
    assert(channel.state() == P::State::Opening);
    assert(channel.credits() == 0);
    assert(channel.receive(frame(P::Type::Opened, id, channel.epoch())) == P::Reception::Accepted);
    assert(channel.state() == P::State::Open);
    assert(channel.credits() == P::kWindow);
}

void goldenWireAndBounds() {
    auto source = data(3, 0x1234, 0xabcd, "abc");
    uint8_t wire[P::kMaxFrame + 1]{};
    constexpr uint8_t expected[] = {1, 3, 3, 0x34, 0x12, 0xcd, 0xab, 'a', 'b', 'c'};
    assert(P::encode(source, wire, sizeof(wire)) == sizeof(expected));
    assert(std::memcmp(wire, expected, sizeof(expected)) == 0);
    P::Frame decoded;
    assert(P::decode(expected, sizeof(expected), decoded) == P::Validation::Ok);
    assert(decoded.type == P::Type::Data && decoded.channel == 3);
    assert(decoded.epoch == 0x1234 && decoded.seq == 0xabcd && decoded.length == 3);
    assert(std::memcmp(decoded.payload, "abc", 3) == 0);
    source.length = P::kMaxPayload;
    for (size_t i = 0; i < source.length; ++i) source.payload[i] = static_cast<uint8_t>(i);
    assert(P::encode(source, wire, P::kMaxFrame) == P::kMaxFrame);
    assert(P::decode(wire, P::kMaxFrame, decoded) == P::Validation::Ok);
    assert(decoded.length == P::kMaxPayload);
    assert(std::memcmp(source.payload, decoded.payload, P::kMaxPayload) == 0);
    assert(P::encode(source, wire, P::kMaxFrame - 1) == 0);
    assert(P::decode(wire, P::kMaxFrame + 1, decoded) == P::Validation::Size);
    source.length = P::kMaxPayload + 1;
    assert(P::encode(source, wire, sizeof(wire)) == 0);
    assert(P::validate(source) == P::Validation::Size);
    assert(P::encode(decoded, nullptr, 180) == 0);
    assert(P::decode(nullptr, 7, decoded) == P::Validation::Null);
    for (size_t size = 0; size < P::kHeaderSize; ++size) {
        assert(P::decode(wire, size, decoded) == P::Validation::Size);
    }
}

void corruptHeadersAndAtomicDecode() {
    uint8_t wire[] = {1, 4, 0, 1, 0, 0, 0, 0};
    P::Frame decoded = data(2, 42, 31, "sentinel");
    const auto unchanged = decoded;
    wire[0] = 2;
    assert(P::decode(wire, 7, decoded) == P::Validation::Version);
    assert(decoded.epoch == unchanged.epoch && decoded.length == unchanged.length);
    wire[0] = 1;
    for (uint8_t type : {uint8_t(0), uint8_t(7), uint8_t(255)}) {
        wire[1] = type;
        assert(P::decode(wire, 7, decoded) == P::Validation::Type);
    }
    wire[1] = 4;
    for (uint8_t id : {uint8_t(4), uint8_t(255)}) {
        wire[2] = id;
        assert(P::decode(wire, 7, decoded) == P::Validation::Channel);
    }
    wire[2] = 0;
    assert(P::decode(wire, 8, decoded) == P::Validation::Payload); // ACK com lixo
    assert(decoded.epoch == unchanged.epoch && decoded.length == unchanged.length);
    assert(std::memcmp(decoded.payload, unchanged.payload, unchanged.length) == 0);
    wire[1] = 2;
    wire[5] = 1;
    assert(P::decode(wire, 7, decoded) == P::Validation::Sequence);
    wire[5] = 0;
    assert(P::decode(wire, 8, decoded) == P::Validation::Payload);
    wire[1] = 5;
    assert(P::decode(wire, 8, decoded) == P::Validation::Payload);
}

void openingAndUtf8() {
    P::Frame opened;
    assert(P::makeOpen(1, 0x4321, "api.example.com", 443, opened));
    uint8_t wire[180];
    assert(P::encode(opened, wire, sizeof(wire)) == 24);
    assert(wire[7] == 0xbb && wire[8] == 0x01);
    assert(std::memcmp(wire + 9, "api.example.com", 15) == 0);
    assert(!P::makeOpen(0, 1, "example.com", 80, opened));
    assert(!P::makeOpen(4, 1, "example.com", 443, opened));
    assert(!P::makeOpen(0, 1, "", 443, opened));
    assert(!P::makeOpen(0, 1, nullptr, 443, opened));
    assert(!P::makeOpen(0, 1, "host\nname", 443, opened));
    char longHost[P::kMaxPayload + 1];
    std::memset(longHost, 'a', sizeof(longHost));
    longHost[P::kMaxPayload - 2] = '\0';
    assert(P::makeOpen(0, 1, longHost, 443, opened));
    assert(opened.length == P::kMaxPayload);
    longHost[P::kMaxPayload - 2] = 'a';
    longHost[P::kMaxPayload] = '\0';
    assert(!P::makeOpen(0, 1, longHost, 443, opened));

    const uint8_t valid[] = {'a', 0xc3, 0xa7, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80};
    assert(P::validUtf8(valid, sizeof(valid)));
    assert(P::validUtf8(nullptr, 0));
    assert(!P::validUtf8(nullptr, 1));
    const uint8_t overlong[] = {0xc0, 0xaf};
    const uint8_t surrogate[] = {0xed, 0xa0, 0x80};
    const uint8_t tooHigh[] = {0xf4, 0x90, 0x80, 0x80};
    const uint8_t continuation[] = {0x80};
    assert(!P::validUtf8(overlong, sizeof(overlong)));
    assert(!P::validUtf8(surrogate, sizeof(surrogate)));
    assert(!P::validUtf8(tooHigh, sizeof(tooHigh)));
    assert(!P::validUtf8(continuation, sizeof(continuation)));
    assert(!P::validUtf8(valid + 6, 3));
    auto error = frame(P::Type::Error);
    error.length = sizeof(valid);
    std::memcpy(error.payload, valid, error.length);
    assert(P::validate(error) == P::Validation::Ok);
    error.payload[0] = 0xff;
    assert(P::validate(error) == P::Validation::Utf8);
    assert(P::makeOpen(0, 1, "x", 443, opened));
    opened.payload[2] = 0;
    assert(P::validate(opened) == P::Validation::Payload);
    opened.payload[2] = 0xff;
    assert(P::validate(opened) == P::Validation::Utf8);
    opened.payload[0] = 80;
    assert(P::validate(opened) == P::Validation::Port);
}

void txCreditWindow() {
    P::TxWindow tx;
    uint16_t seq = 99;
    assert(!tx.reserve(seq) && seq == 99);
    tx.reset();
    for (uint16_t i = 0; i < P::kWindow; ++i) {
        assert(tx.reserve(seq) && seq == i);
        assert(tx.pending() == i + 1);
    }
    assert(tx.credits() == 0 && !tx.reserve(seq));
    assert(!tx.acknowledge(1)); // não cumulativo / não pular ACK
    assert(tx.pending() == 8);
    assert(tx.acknowledge(0));
    assert(!tx.acknowledge(0)); // duplicata não devolve crédito
    assert(tx.pending() == 7 && tx.credits() == 1);
    assert(tx.reserve(seq) && seq == 8);
    for (uint16_t i = 1; i <= 8; ++i) assert(tx.acknowledge(i));
    assert(tx.pending() == 0 && !tx.acknowledge(9));
    tx.reset(65534);
    assert(tx.reserve(seq) && seq == 65534);
    assert(tx.reserve(seq) && seq == 65535);
    assert(tx.reserve(seq) && seq == 0);
    assert(tx.acknowledge(65534) && tx.acknowledge(65535) && tx.acknowledge(0));
}

void rxConsumptionAndOwnership() {
    P::RxWindow rx;
    rx.reset();
    uint8_t incoming[] = {'a', 'b', 'c', 'd'};
    assert(rx.push(0, incoming, sizeof(incoming)));
    incoming[0] = 'X'; // frame é de propriedade da janela
    assert(rx.peek() == 'a' && rx.available() == 4);
    uint8_t out[10]{};
    auto consumed = rx.consume(out, 2);
    assert(consumed.bytes == 2 && !consumed.ack);
    assert(std::memcmp(out, "ab", 2) == 0);
    assert(rx.slots() == 1 && rx.available() == 2 && rx.peek() == 'c');
    assert(rx.consume(nullptr, 1).bytes == 0 && rx.available() == 2);
    consumed = rx.consume(out, sizeof(out));
    assert(consumed.bytes == 2 && consumed.ack && consumed.sequence == 0);
    assert(std::memcmp(out, "cd", 2) == 0);
    assert(rx.available() == 0 && rx.slots() == 0 && rx.peek() == -1);
    assert(!rx.consume(out, 1).ack);
    assert(!rx.push(0, incoming, 1)); // duplicata
    assert(!rx.push(2, incoming, 1)); // lacuna
    assert(rx.push(1, incoming, 1));
    assert(!rx.push(2, nullptr, 1));
    assert(!rx.push(2, incoming, P::kMaxPayload + 1));
}

void rxSlotsAndWrap() {
    P::RxWindow rx;
    rx.reset(65532);
    uint8_t payload[P::kMaxPayload];
    std::memset(payload, 0xa5, sizeof(payload));
    for (uint16_t i = 0; i < P::kWindow; ++i) {
        assert(rx.push(static_cast<uint16_t>(65532 + i), payload, sizeof(payload)));
    }
    assert(rx.available() == P::kWindow * P::kMaxPayload);
    assert(!rx.push(4, payload, 1)); // 9º slot, mesmo sequência correta
    uint8_t out[P::kMaxPayload];
    auto consumed = rx.consume(out, P::kMaxPayload - 1);
    assert(!consumed.ack && rx.slots() == 8);
    assert(!rx.push(4, payload, 1)); // ler parte não concede crédito
    consumed = rx.consume(out, 1);
    assert(consumed.ack && consumed.sequence == 65532 && rx.slots() == 7);
    assert(rx.push(4, payload, 1));
    for (uint16_t i = 1; i <= P::kWindow; ++i) {
        consumed = rx.consume(out, sizeof(out));
        assert(consumed.ack && consumed.sequence == static_cast<uint16_t>(65532 + i));
        assert(consumed.bytes == (i == 8 ? 1 : P::kMaxPayload));
        assert(out[0] == 0xa5);
    }
    assert(rx.available() == 0 && rx.slots() == 0);
}

void emptyData() {
    P::RxWindow rx;
    rx.reset();
    const uint8_t x = 'x';
    assert(rx.push(0, nullptr, 0));
    auto consumed = rx.consume(nullptr, 0);
    assert(consumed.ack && consumed.sequence == 0 && consumed.bytes == 0);
    assert(rx.push(1, &x, 1));
    assert(rx.push(2, nullptr, 0));
    assert(!rx.consume(nullptr, 0).ack); // não pula bytes anteriores
    uint8_t out;
    assert(rx.consume(&out, 1).sequence == 1 && out == x);
    consumed = rx.consume(nullptr, 0);
    assert(consumed.ack && consumed.sequence == 2);
    auto empty = frame(P::Type::Data);
    uint8_t wire[7];
    assert(P::encode(empty, wire, sizeof(wire)) == 7);
    assert(P::decode(wire, sizeof(wire), empty) == P::Validation::Ok);
}

void epochsAndCancel() {
    P::Channel channel;
    establish(channel, 2);
    const auto oldEpoch = channel.epoch();
    const auto token = channel.generation();
    const auto incoming = data(2, oldEpoch, 0, "response");
    assert(channel.receive(incoming) == P::Reception::Accepted);
    channel.abort();
    assert(channel.state() == P::State::Aborted && channel.available() == 0 && channel.peek() == -1);
    assert(channel.generation() != token && channel.credits() == 0);
    P::Frame outgoing;
    const uint8_t byte = 1;
    assert(!channel.prepareData(&byte, 1, outgoing));
    assert(!channel.consume(nullptr, 0).ack);
    assert(channel.receive(incoming) == P::Reception::Fault);
    assert(!channel.start(2)); // consumidor deve liberar primeiro
    channel.release();
    establish(channel, 2);
    assert(channel.epoch() == static_cast<uint16_t>(oldEpoch + 1));
    assert(channel.receive(incoming) == P::Reception::StaleEpoch);
    auto oldError = frame(P::Type::Error, 2, oldEpoch);
    oldError.length = 1;
    oldError.payload[0] = 0xff; // filtrar epoch antes de conteúdo antigo
    assert(channel.receive(oldError) == P::Reception::StaleEpoch);
    assert(channel.state() == P::State::Open && channel.available() == 0);
    assert(channel.receive(data(2, channel.epoch(), 0, "new")) == P::Reception::Accepted);

    P::TxWindow tx;
    tx.reset();
    uint16_t seq;
    assert(tx.reserve(seq));
    tx.cancel();
    assert(!tx.reserve(seq) && !tx.acknowledge(0) && tx.pending() == 0 && tx.credits() == 0);
    P::RxWindow rx;
    rx.reset();
    assert(rx.push(0, &byte, 1));
    rx.cancel();
    assert(rx.available() == 0 && !rx.push(1, &byte, 1) && !rx.consume(nullptr, 0).ack);

    // Epoch é um contador wire u16 separado das sequências. release não o zera.
    channel.release();
    uint16_t previous = channel.epoch();
    for (uint32_t i = 0; i < 65537; ++i) {
        assert(channel.start(2));
        assert(channel.epoch() == static_cast<uint16_t>(previous + 1));
        assert(channel.receive(frame(P::Type::Opened, 2, previous)) == P::Reception::StaleEpoch);
        previous = channel.epoch();
        channel.release();
    }
}

void unexpectedSequencesAbort() {
    P::Channel channel;
    establish(channel);
    assert(channel.receive(data(0, channel.epoch(), 1, "gap")) == P::Reception::Fault);
    assert(channel.state() == P::State::Aborted);
    channel.release();
    establish(channel);
    const auto packet = data(0, channel.epoch(), 0, "once");
    assert(channel.receive(packet) == P::Reception::Accepted);
    assert(channel.receive(packet) == P::Reception::Fault);
    assert(channel.available() == 0); // falha explícita, sem replay
    channel.release();
    establish(channel);
    const uint8_t x = 'x';
    P::Frame outgoing;
    assert(channel.prepareData(&x, 1, outgoing));
    assert(channel.receive(frame(P::Type::Ack, 0, channel.epoch(), 1)) == P::Reception::Fault);
    assert(channel.pending() == 0 && channel.state() == P::State::Aborted);
}

void rxOverflowAborts() {
    P::Channel channel;
    establish(channel);
    for (uint16_t i = 0; i < P::kWindow; ++i) {
        assert(channel.receive(data(0, channel.epoch(), i, "x")) == P::Reception::Accepted);
    }
    assert(channel.receive(data(0, channel.epoch(), 8, "overflow")) == P::Reception::Fault);
    assert(channel.state() == P::State::Aborted && channel.available() == 0);
}

void controlStateAndRemoteEof() {
    P::Channel channel;
    assert(!channel.start(4));
    assert(channel.start(0));
    assert(channel.receive(data(0, channel.epoch(), 0, "early")) == P::Reception::Fault);
    channel.release();
    establish(channel);
    assert(channel.receive(frame(P::Type::Opened, 0, channel.epoch())) == P::Reception::Fault);
    channel.release();
    establish(channel);
    assert(channel.receive(frame(P::Type::Open, 0, channel.epoch())) == P::Reception::Fault);
    channel.release();
    establish(channel);
    assert(channel.receive(data(1, channel.epoch(), 0, "wrong channel")) == P::Reception::Fault);
    channel.release();
    establish(channel);
    assert(channel.receive(data(0, channel.epoch(), 0, "last bytes")) == P::Reception::Accepted);
    assert(channel.receive(frame(P::Type::Close, 0, channel.epoch())) == P::Reception::RemoteClosed);
    assert(channel.state() == P::State::PeerClosed && channel.available() == 10);
    uint8_t out[32]{};
    const auto consumed = channel.consume(out, sizeof(out));
    assert(consumed.ack && consumed.bytes == 10 && std::memcmp(out, "last bytes", 10) == 0);
    assert(channel.available() == 0 && channel.state() == P::State::PeerClosed);
    P::Frame outgoing;
    assert(!channel.prepareData(out, 1, outgoing));
    channel.release();
    establish(channel);
    assert(channel.prepareData(out, 1, outgoing));
    assert(channel.receive(frame(P::Type::Close, 0, channel.epoch())) == P::Reception::Fault);
    assert(channel.state() == P::State::Aborted); // CLOSE com TX sem confirmação não vira EOF normal
    channel.release();
    establish(channel);
    assert(channel.receive(data(0, channel.epoch(), 0, "discard on ERROR")) == P::Reception::Accepted);
    assert(channel.receive(frame(P::Type::Error, 0, channel.epoch())) == P::Reception::RemoteError);
    assert(channel.available() == 0 && channel.state() == P::State::Aborted);
}

void fourIndependentChannels() {
    P::Channel channels[4];
    const uint8_t byte = 42;
    for (uint8_t i = 0; i < 4; ++i) establish(channels[i], i);
    P::Frame outgoing;
    for (uint16_t i = 0; i < 8; ++i) assert(channels[0].prepareData(&byte, 1, outgoing));
    assert(!channels[0].prepareData(&byte, 1, outgoing));
    assert(channels[1].prepareData(&byte, 1, outgoing) && outgoing.seq == 0);
    assert(channels[1].receive(data(1, channels[1].epoch(), 0, "r")) == P::Reception::Accepted);
    assert(channels[1].pending() == 1 && channels[1].available() == 1);
    assert(channels[1].receive(frame(P::Type::Ack, 1, channels[1].epoch(), 0)) == P::Reception::Accepted);
    assert(channels[0].pending() == 8 && channels[1].pending() == 0);
    channels[0].abort();
    assert(channels[1].state() == P::State::Open && channels[2].credits() == 8);
}

void fullSequenceWrapBothDirections() {
    P::Channel channel;
    establish(channel);
    const uint8_t value = 0xa5;
    for (uint32_t i = 0; i < 65540; ++i) {
        P::Frame outgoing;
        assert(channel.prepareData(&value, 1, outgoing));
        assert(outgoing.seq == static_cast<uint16_t>(i));
        auto incoming = outgoing; // mesma sequência, direções independentes
        assert(channel.receive(incoming) == P::Reception::Accepted);
        uint8_t out = 0;
        const auto consumed = channel.consume(&out, 1);
        assert(consumed.ack && consumed.sequence == static_cast<uint16_t>(i) && out == value);
        assert(channel.receive(frame(P::Type::Ack, 0, channel.epoch(), outgoing.seq)) == P::Reception::Accepted);
    }
    assert(channel.state() == P::State::Open && channel.credits() == 8 && channel.available() == 0);
}

void boundedMalformedSweep() {
    // Fuzz determinístico: todas as extensões possíveis do header, e entradas
    // pseudoaleatórias. Decodificações válidas precisam roundtrip byte a byte.
    uint32_t random = 0x219452e1;
    uint8_t wire[P::kMaxFrame + 1]{};
    uint8_t encoded[P::kMaxFrame];
    size_t accepted = 0;
    for (size_t iteration = 0; iteration < 6000; ++iteration) {
        const size_t length = iteration % sizeof(wire);
        for (size_t i = 0; i < length; ++i) {
            random = random * 1664525u + 1013904223u;
            wire[i] = static_cast<uint8_t>(random >> 24);
        }
        if (length >= 7 && iteration % 3 == 0) {
            wire[0] = 1;
            wire[1] = static_cast<uint8_t>(1 + (iteration / 3) % 6);
            wire[2] = static_cast<uint8_t>(iteration % 4);
        }
        P::Frame decoded;
        if (P::decode(wire, length, decoded) == P::Validation::Ok) {
            ++accepted;
            assert(P::validate(decoded) == P::Validation::Ok);
            assert(P::encode(decoded, encoded, sizeof(encoded)) == length);
            assert(std::memcmp(wire, encoded, length) == 0);
        }
    }
    assert(accepted > 100); // o sweep deve exercitar também o caminho válido
}
} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"wire golden/limites", goldenWireAndBounds},
        {"corrupcao/version/channel/decode atomico", corruptHeadersAndAtomicDecode},
        {"OPEN porta/UTF-8/host limitado", openingAndUtf8},
        {"janela TX/ACK individual/wrap", txCreditWindow},
        {"RX copia/consumo parcial/ACK tardio", rxConsumptionAndOwnership},
        {"oito slots RX/wrap/credito apos drenar", rxSlotsAndWrap},
        {"DATA vazio sem pular bytes anteriores", emptyData},
        {"epoch atrasado/cancelamento/reabertura", epochsAndCancel},
        {"sequencia inesperada/duplicata aborta", unexpectedSequencesAbort},
        {"overflow RX aborta", rxOverflowAborts},
        {"controle/ERROR/EOF preserva RX", controlStateAndRemoteEof},
        {"quatro canais/direcoes independentes", fourIndependentChannels},
        {"65540 DATA+ACK por direcao", fullSequenceWrapBothDirections},
        {"6000 frames malformados/roundtrip", boundedMalformedSweep},
    };
    for (const auto& test : tests) {
        test.run();
        std::printf("OK: %s\n", test.name);
    }
    std::printf("Relay protocol: %zu/%zu grupos OK\n", sizeof(tests) / sizeof(tests[0]), sizeof(tests) / sizeof(tests[0]));
    return 0;
}
