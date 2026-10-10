// Regressao de layout/finalizacao com o LittleFS upstream real, sem Arduino.
// Referencias oficiais v2.9.3: README.md (sync/rename), DESIGN.md (CTZ/COW)
// e lfs.h (configuracao e API). O runner verifica SHA-256 dos quatro sources.
// Os fluxos abaixo emulam os layouts antigo/novo; nao executam Recorder/UI.
#include "lfs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr size_t kPartition = 4864 * 1024;
constexpr lfs_size_t kBlock = 4096;
constexpr lfs_size_t kBlocks = kPartition / kBlock;
constexpr lfs_size_t kRead = 16, kProg = 256, kCache = 512, kLookahead = 128;
constexpr size_t kAppend = 8192, kCheckpoint = 32768;
constexpr uint32_t kBytesPerSecond = 16000 * 2; // Mono, PCM16, 16 kHz.
using Bytes = std::vector<uint8_t>;
using Header = std::array<uint8_t, 44>;
using BlockMap = std::array<bool, kBlocks>;
static_assert(LFS_VERSION == 0x00020009, "O runner deve usar LittleFS v2.9.3");
static_assert(kBlocks == 1216, "Geometria da particao ESP32");

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void success(int result, const std::string &operation) {
    require(result == 0, operation + ": resultado " + std::to_string(result));
}

struct Ops {
    uint64_t reads = 0, readBytes = 0, programs = 0, programBytes = 0;
    uint64_t erases = 0, syncs = 0, protectedPrograms = 0, protectedErases = 0;
};

Ops operator+(const Ops &a, const Ops &b) {
    return {a.reads + b.reads, a.readBytes + b.readBytes,
            a.programs + b.programs, a.programBytes + b.programBytes,
            a.erases + b.erases, a.syncs + b.syncs,
            a.protectedPrograms + b.protectedPrograms,
            a.protectedErases + b.protectedErases};
}

// Apenas o dispositivo de blocos e emulado. Alocacao, CTZ, COW, commits,
// recuperacao, sync e rename sao executados por lfs.c/lfs_util.c oficiais.
struct Nor {
    Bytes bytes;
    Ops ops;
    BlockMap readBlocks{}, protectedBlocks{};
    bool capture = false;
    std::vector<Bytes> snapshots;

    Nor() : bytes(kPartition, 0xff) {}
    explicit Nor(const Bytes &image) : bytes(image) {
        require(bytes.size() == kPartition, "snapshot com geometria incorreta");
    }

    static Nor &device(const lfs_config *c) { return *static_cast<Nor *>(c->context); }

    static bool valid(lfs_block_t block, lfs_off_t off, lfs_size_t size,
                      lfs_size_t alignment) {
        return block < kBlocks && off <= kBlock && size > 0 &&
               size <= kBlock - off && off % alignment == 0 && size % alignment == 0;
    }

    static int read(const lfs_config *c, lfs_block_t block, lfs_off_t off,
                    void *buffer, lfs_size_t size) {
        auto &d = device(c);
        if (!valid(block, off, size, kRead)) return LFS_ERR_IO;
        ++d.ops.reads;
        d.ops.readBytes += size;
        d.readBlocks[block] = true;
        std::memcpy(buffer, d.bytes.data() + block * kBlock + off, size);
        return 0;
    }

    static int prog(const lfs_config *c, lfs_block_t block, lfs_off_t off,
                    const void *buffer, lfs_size_t size) {
        auto &d = device(c);
        if (!valid(block, off, size, kProg)) return LFS_ERR_IO;
        const auto *input = static_cast<const uint8_t *>(buffer);
        auto *output = d.bytes.data() + block * kBlock + off;
        for (lfs_size_t i = 0; i < size; ++i) {
            // NOR so permite 1 -> 0; rejeita tentativa de 0 -> 1 sem erase.
            if ((output[i] & input[i]) != input[i]) return LFS_ERR_IO;
        }
        ++d.ops.programs;
        d.ops.programBytes += size;
        if (d.protectedBlocks[block]) ++d.ops.protectedPrograms;
        for (lfs_size_t i = 0; i < size; ++i) output[i] &= input[i];
        if (d.capture) d.snapshots.push_back(d.bytes);
        return 0;
    }

    static int erase(const lfs_config *c, lfs_block_t block) {
        auto &d = device(c);
        if (block >= kBlocks) return LFS_ERR_IO;
        ++d.ops.erases;
        if (d.protectedBlocks[block]) ++d.ops.protectedErases;
        std::fill_n(d.bytes.data() + block * kBlock, kBlock, uint8_t{0xff});
        if (d.capture) d.snapshots.push_back(d.bytes);
        return 0;
    }

    static int sync(const lfs_config *c) {
        // Sem cache no dispositivo: cada prog/erase ja alterou o disco.
        ++device(c).ops.syncs;
        return 0;
    }
};

struct Mount {
    Nor &disk;
    lfs_t fs{};
    lfs_config config{};
    std::array<uint8_t, kCache> readCache{}, progCache{};
    std::array<uint8_t, kLookahead> lookahead{};
    bool mounted = false;

    explicit Mount(Nor &d, bool format = false) : disk(d) {
        config.context = &disk;
        config.read = Nor::read;
        config.prog = Nor::prog;
        config.erase = Nor::erase;
        config.sync = Nor::sync;
        config.read_size = kRead;
        config.prog_size = kProg;
        config.block_size = kBlock;
        config.block_count = kBlocks;
        config.cache_size = kCache;
        config.lookahead_size = kLookahead;
        config.block_cycles = 512;
        config.read_buffer = readCache.data();
        config.prog_buffer = progCache.data();
        config.lookahead_buffer = lookahead.data();
        if (format) success(lfs_format(&fs, &config), "format");
        success(lfs_mount(&fs, &config), "mount (sem formatar no reboot)");
        mounted = true;
    }

    Mount(const Mount &) = delete;
    Mount &operator=(const Mount &) = delete;
    ~Mount() {
        // v2.9.3 unmount apenas libera buffers; nao fecha/sincroniza arquivos.
        if (mounted) lfs_unmount(&fs);
    }

    void powerOff() {
        // Todos os caches sao estaticos (LFS_NO_MALLOC). Abandona o lfs_t e
        // arquivos abertos, SEM close, sync ou unmount antes do novo mount.
        mounted = false;
    }

    size_t freeBytes() {
        // lfs_fs_size e best-effort e conta duas vezes prefixos COW
        // compartilhados pelo checkpoint e pelo writer ainda aberto.
        BlockMap used{};
        const auto mark = [](void *context, lfs_block_t block) -> int {
            if (block >= kBlocks) return LFS_ERR_CORRUPT;
            (*static_cast<BlockMap *>(context))[block] = true;
            return 0;
        };
        success(lfs_fs_traverse(&fs, mark, &used), "fs_traverse");
        const auto count = std::count(used.begin(), used.end(), true);
        return (kBlocks - static_cast<size_t>(count)) * kBlock;
    }
};

struct File {
    Mount &mount;
    lfs_file_t file{};
    lfs_file_config config{};
    std::array<uint8_t, kCache> cache{};
    bool open = false;

    File(Mount &m, const char *path, int flags) : mount(m) {
        config.buffer = cache.data();
        success(lfs_file_opencfg(&m.fs, &file, path, flags, &config),
                std::string("open ") + path);
        open = true;
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    // Intencionalmente sem close automatico: snapshots de crash nao podem
    // persistir dados por uma chamada de destrutor. Casos normais usam close.
    void write(const uint8_t *data, size_t size) {
        const auto result = lfs_file_write(&mount.fs, &file, data,
                                           static_cast<lfs_size_t>(size));
        require(result == static_cast<lfs_ssize_t>(size),
                "write: " + std::to_string(result) + " esperado " + std::to_string(size));
    }
    void sync() { success(lfs_file_sync(&mount.fs, &file), "file_sync"); }
    void close() {
        require(open, "close duplicado");
        success(lfs_file_close(&mount.fs, &file), "file_close");
        open = false;
    }
};

void put16(Header &h, size_t offset, uint16_t value) {
    h[offset] = static_cast<uint8_t>(value);
    h[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(Header &h, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) h[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

Header header(size_t pcmBytes) {
    Header h{};
    std::memcpy(h.data(), "RIFF", 4);
    put32(h, 4, 36 + static_cast<uint32_t>(pcmBytes));
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    put32(h, 16, 16);
    put16(h, 20, 1);
    put16(h, 22, 1);
    put32(h, 24, 16000);
    put32(h, 28, kBytesPerSecond);
    put16(h, 32, 2);
    put16(h, 34, 16);
    std::memcpy(h.data() + 36, "data", 4);
    put32(h, 40, static_cast<uint32_t>(pcmBytes));
    return h;
}

Bytes pcm(unsigned seconds, uint32_t seed = 0x8f31a72b) {
    Bytes data(static_cast<size_t>(seconds) * kBytesPerSecond);
    // PCM16 little-endian deterministico, nao silencio/repeticao trivial.
    for (size_t i = 0; i < data.size(); i += 2) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        data[i] = static_cast<uint8_t>(seed);
        data[i + 1] = static_cast<uint8_t>(seed >> 8);
    }
    return data;
}

Bytes readFile(Mount &m, const char *path, BlockMap *blocks = nullptr,
               lfs_block_t *head = nullptr) {
    File input(m, path, LFS_O_RDONLY);
    const auto size = lfs_file_size(&m.fs, &input.file);
    require(size >= 0, std::string("size ") + path);
    if (head) *head = input.file.ctz.head;
    // Open pode ler metadata. Depois dele, read de arquivo nao-inline le
    // somente os blocos de dados/ponteiros CTZ, sem inventar um parser CTZ.
    if (blocks) m.disk.readBlocks.fill(false);
    Bytes data(static_cast<size_t>(size));
    size_t offset = 0;
    while (offset < data.size()) {
        const size_t wanted = std::min(kAppend, data.size() - offset);
        const auto got = lfs_file_read(&m.fs, &input.file, data.data() + offset,
                                       static_cast<lfs_size_t>(wanted));
        require(got == static_cast<lfs_ssize_t>(wanted), std::string("read ") + path);
        offset += wanted;
    }
    if (blocks) *blocks = m.disk.readBlocks;
    input.close();
    return data;
}

void expectHeader(Mount &m, const char *path, const Header &expected) {
    const auto saved = readFile(m, path);
    require(saved.size() == expected.size() &&
            std::equal(saved.begin(), saved.end(), expected.begin()),
            std::string("header incorreto: ") + path);
}

bool exists(Mount &m, const char *path) {
    lfs_info info{};
    const int result = lfs_stat(&m.fs, path, &info);
    require(result == 0 || result == LFS_ERR_NOENT, "stat inesperado");
    return result == 0;
}

void writeTemporary(Mount &m, const Header &h) {
    File tmp(m, "/record.wav.tmp", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    tmp.write(h.data(), h.size());
    tmp.sync();
    tmp.close();
    expectHeader(m, "/record.wav.tmp", h);
}

void atomicHeader(Mount &m, const Header &h) {
    writeTemporary(m, h);
    success(lfs_rename(&m.fs, "/record.wav.tmp", "/record.wav"), "rename atomico");
}

void append(File &file, const Bytes &data, bool checkpoints) {
    size_t committed = 0;
    for (size_t offset = 0; offset < data.size();) {
        const size_t size = std::min(kAppend, data.size() - offset);
        file.write(data.data() + offset, size);
        offset += size;
        if (checkpoints && offset - committed >= kCheckpoint) {
            file.sync();
            committed = offset;
        }
    }
}

void writeLegacyWav(Mount &m, const char *path, const Bytes &data) {
    File file(m, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL);
    const auto h = header(data.size());
    file.write(h.data(), h.size());
    append(file, data, false);
    file.close();
}

void expectLegacyWav(Mount &m, const char *path, const Bytes &data) {
    const auto saved = readFile(m, path);
    const auto h = header(data.size());
    require(saved.size() == h.size() + data.size(), "tamanho WAV legado");
    require(std::equal(h.begin(), h.end(), saved.begin()), "header WAV legado");
    require(std::equal(data.begin(), data.end(), saved.begin() + h.size()), "PCM legado mudou");
}

void expectPrevious(Mount &m) {
    expectLegacyWav(m, "/previous1.wav", pcm(5, 1));
    expectLegacyWav(m, "/previous2.wav", pcm(5, 2));
}

Bytes baseline(unsigned seconds, bool lowSpace) {
    Nor disk;
    Mount m(disk, true);
    writeLegacyWav(m, "/previous1.wav", pcm(5, 1));
    writeLegacyWav(m, "/previous2.wav", pcm(5, 2));
    if (lowSpace) {
        // Deixa espaco para UMA gravacao + ~128 KiB, nao para uma segunda
        // copia CTZ do arquivo inteiro. Filler e arquivo LittleFS real.
        const size_t reserve = static_cast<size_t>(seconds) * kBytesPerSecond + 128 * 1024;
        require(m.freeBytes() > reserve, "baseline sem espaco para filler");
        const size_t fillerBytes = ((m.freeBytes() - reserve) / kAppend) * kAppend;
        File filler(m, "/filler.bin", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL);
        std::array<uint8_t, kAppend> chunk{};
        chunk.fill(0x5a);
        for (size_t offset = 0; offset < fillerBytes; offset += chunk.size()) {
            filler.write(chunk.data(), chunk.size());
        }
        filler.close();
    }
    expectPrevious(m);
    return disk.bytes;
}

void report(const std::string &label, const Ops &ops) {
    std::cout << "  " << label << ": read=" << ops.reads << " (" << ops.readBytes
              << " B), prog=" << ops.programs << " (" << ops.programBytes
              << " B), erase=" << ops.erases << ", sync=" << ops.syncs << '\n';
}

struct Result {
    Ops final, headerOnly;
    bool noSpace = false;
    size_t freeAfterAppend = 0;
};

Result oldLayout(const Bytes &image, const Bytes &data, bool lowSpace) {
    Nor disk(image);
    Mount m(disk);
    File wav(m, "/record.wav", LFS_O_RDWR | LFS_O_CREAT | LFS_O_EXCL);
    const auto placeholder = header(0);
    wav.write(placeholder.data(), placeholder.size());
    append(wav, data, false); // Antigo: append sequencial, sem checkpoints.
    require(lfs_file_size(&m.fs, &wav.file) == static_cast<lfs_soff_t>(44 + data.size()),
            "tamanho antes da finalizacao antiga");
    Result result;
    result.freeAfterAppend = m.freeBytes();
    disk.ops = {};
    // Janela medida exata: seek(0), write(44), flush/sync, close.
    const auto h = header(data.size());
    success(static_cast<int>(lfs_file_seek(&m.fs, &wav.file, 0, LFS_SEEK_SET)), "seek(0)");
    const auto written = lfs_file_write(&m.fs, &wav.file, h.data(), h.size());
    require(written == static_cast<lfs_ssize_t>(h.size()) || written == LFS_ERR_NOSPC,
            "header antigo: erro inesperado");
    const int flushed = written < 0 ? static_cast<int>(written) : lfs_file_sync(&m.fs, &wav.file);
    require(flushed == 0 || flushed == LFS_ERR_NOSPC, "flush antigo: erro inesperado");
    result.noSpace = flushed == LFS_ERR_NOSPC;
    wav.close(); // LFS_F_ERRED nao comita a escrita que falhou.
    result.final = disk.ops;
    require(result.noSpace == lowSpace, "final antigo: expectativa de NOSPC divergente");
    if (!result.noSpace) {
        Nor reboot(disk.bytes);
        Mount fresh(reboot);
        expectLegacyWav(fresh, "/record.wav", data);
        expectPrevious(fresh);
    } else {
        Nor reboot(disk.bytes);
        Mount fresh(reboot);
        expectPrevious(fresh);
    }
    return result;
}

struct PhysicalPcm {
    BlockMap blocks{};
    lfs_block_t head = 0;
};

PhysicalPcm inspectPcm(const Bytes &image, const Bytes &expected) {
    Nor disk(image);
    Mount fresh(disk);
    PhysicalPcm physical;
    const auto saved = readFile(fresh, "/record.pcm", &physical.blocks, &physical.head);
    require(saved == expected, "PCM persistido diferente das amostras de entrada");
    require(std::count(physical.blocks.begin(), physical.blocks.end(), true) >=
            static_cast<std::ptrdiff_t>((expected.size() + kBlock - 1) / kBlock),
            "mapa fisico PCM incompleto");
    return physical;
}

void expectUnchangedPcm(const Bytes &before, const Nor &after, const Bytes &data,
                        const PhysicalPcm &physical) {
    const auto now = inspectPcm(after.bytes, data);
    require(now.head == physical.head && now.blocks == physical.blocks,
            "header update realocou/copiou a cadeia PCM");
    require(after.ops.protectedPrograms == 0 && after.ops.protectedErases == 0,
            "header update programou/apagou blocos PCM");
    for (size_t block = 0; block < kBlocks; ++block) {
        if (!physical.blocks[block]) continue;
        const size_t offset = block * kBlock;
        require(std::equal(before.begin() + offset, before.begin() + offset + kBlock,
                           after.bytes.begin() + offset), "bloco PCM fisico alterado");
    }
}

Result newLayout(const Bytes &image, const Bytes &data, bool lowSpace) {
    Nor disk(image);
    Mount m(disk);
    atomicHeader(m, header(0));
    File payload(m, "/record.pcm", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL | LFS_O_APPEND);
    append(payload, data, true);
    Result result;
    result.freeAfterAppend = m.freeBytes();
    if (lowSpace) {
        require(result.freeAfterAppend >= 8 * kBlock && result.freeAfterAppend <= 128 * 1024,
                "cenario deve ter pouca folga, mas permitir commits de metadata");
    }
    disk.ops = {};
    payload.sync();
    payload.close();
    const Ops drain = disk.ops;
    const Bytes beforeHeader = disk.bytes;
    // Leitura/inspecao somente em outro lfs_t sobre COPY do disco, para nao
    // aquecer caches nem acrescentar operacoes na janela de finalizacao.
    const auto physical = inspectPcm(beforeHeader, data);
    disk.protectedBlocks = physical.blocks;
    disk.ops = {};
    atomicHeader(m, header(data.size()));
    result.headerOnly = disk.ops;
    result.final = drain + result.headerOnly;
    expectUnchangedPcm(beforeHeader, disk, data, physical);
    Nor reboot(disk.bytes);
    Mount fresh(reboot);
    expectHeader(fresh, "/record.wav", header(data.size()));
    require(!exists(fresh, "/record.wav.tmp"), "tmp permaneceu depois do rename");
    expectPrevious(fresh);
    // Limites independentes da duracao, com folga para compactacao metadata.
    require(result.headerOnly.erases <= 4 && result.headerOnly.programBytes <= 8 * 1024,
            "header separado deixou de ter custo constante baixo");
    require(result.final.erases <= 4 && result.final.programBytes <= 12 * 1024,
            "final novo fez trabalho proporcional ao PCM");
    return result;
}

std::pair<Result, Result> compare(unsigned seconds, bool lowSpace) {
    const Bytes image = baseline(seconds, lowSpace);
    const Bytes data = pcm(seconds);
    const Result old = oldLayout(image, data, lowSpace);
    const Result current = newLayout(image, data, lowSpace);
    std::cout << "  PCM=" << data.size() << " B; livres depois append antigo/novo="
              << old.freeAfterAppend << '/' << current.freeAfterAppend << " B\n";
    report(old.noSpace ? "antigo FINAL (NOSPC esperado)" : "antigo FINAL", old.final);
    report("novo FINAL (flush+close+header)", current.final);
    report("novo somente header (.tmp+sync+close+rename)", current.headerOnly);
    if (!lowSpace) {
        require(old.final.programBytes >= data.size(), "antigo nao reproduziu copia integral CTZ");
        require(old.final.erases >= data.size() / kBlock, "antigo nao reproduziu erases proporcionais");
    }
    require(old.final.programBytes > current.final.programBytes * 8 &&
            old.final.erases > current.final.erases + 8,
            "contraste de custo final nao demonstrado");
    return {old, current};
}

void expectCheckpoint(const Bytes &image, const Bytes &data, size_t committed) {
    Nor disk(image);
    Mount fresh(disk); // Novo lfs_t e caches vazios; SEM formatar.
    const auto saved = readFile(fresh, "/record.pcm");
    require(saved.size() == committed && std::equal(saved.begin(), saved.end(), data.begin()),
            "reboot perdeu/alterou checkpoint PCM");
    expectHeader(fresh, "/record.wav", header(0));
    expectPrevious(fresh);
}

void checkpointReboots(unsigned seconds) {
    Nor disk(baseline(seconds, false));
    Mount m(disk);
    atomicHeader(m, header(0));
    File payload(m, "/record.pcm", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL | LFS_O_APPEND);
    const Bytes data = pcm(seconds);
    size_t committed = 0, reboots = 0;
    for (size_t offset = 0; offset < data.size();) {
        const size_t count = std::min(kAppend, data.size() - offset);
        payload.write(data.data() + offset, count);
        offset += count;
        if (offset - committed >= kCheckpoint) {
            payload.sync();
            committed = offset;
            // O source ainda tem o arquivo aberto. So bytes do disco vao ao
            // reboot; nenhuma estrutura RAM/cache/close e reaproveitada.
            const Bytes snapshot = disk.bytes;
            expectCheckpoint(snapshot, data, committed);
            require(disk.bytes == snapshot && payload.open, "reboot modificou source vivo");
            ++reboots;
        }
    }
    require(committed < data.size(), "caso deve conter cauda sem sync");
    const Bytes crash = disk.bytes;
    const Ops beforePowerOff = disk.ops;
    m.powerOff(); // Nunca payload.close(): a cauda ainda esta nao comitada.
    expectCheckpoint(crash, data, committed);
    require(disk.bytes == crash && disk.ops.syncs == beforePowerOff.syncs &&
            disk.ops.programs == beforePowerOff.programs && disk.ops.erases == beforePowerOff.erases,
            "powerOff persistiu dados inadvertidamente");
    std::cout << "  " << reboots << " snapshots apos checkpoint + 1 crash com cauda de "
              << data.size() - committed << " B; PCM salvo=" << committed
              << " B; header original preservado; nenhum close antes do reboot\n";
}

void atomicHeaderReboots() {
    Nor disk(baseline(5, true));
    Mount m(disk);
    atomicHeader(m, header(0));
    File payload(m, "/record.pcm", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL | LFS_O_APPEND);
    const Bytes data = pcm(5);
    append(payload, data, true);
    payload.sync(); // PCM completo persistido, mas arquivo ainda aberto.
    const Bytes before = disk.bytes;
    expectCheckpoint(before, data, data.size());
    const auto physical = inspectPcm(before, data);
    disk.protectedBlocks = physical.blocks;
    disk.ops = {};
    disk.capture = true;
    writeTemporary(m, header(data.size()));
    const Bytes temporaryCommitted = disk.bytes;
    // Crash antes do rename: header original continua visivel, tmp e valido.
    expectCheckpoint(temporaryCommitted, data, data.size());
    {
        Nor reboot(temporaryCommitted);
        Mount fresh(reboot);
        expectHeader(fresh, "/record.wav.tmp", header(data.size()));
    }
    success(lfs_rename(&m.fs, "/record.wav.tmp", "/record.wav"), "rename atomico");
    disk.capture = false;
    require(!disk.snapshots.empty(), "nenhum checkpoint de dispositivo capturado");
    bool sawOld = false, sawNew = false;
    for (const auto &snapshot : disk.snapshots) {
        // Cortes depois de CADA prog/erase completo, inclusive durante
        // sync/rename. Nao simula programacao parcialmente concluida.
        Nor reboot(snapshot);
        Mount fresh(reboot);
        const auto saved = readFile(fresh, "/record.wav");
        const auto old = header(0), current = header(data.size());
        const bool isOld = saved.size() == 44 && std::equal(saved.begin(), saved.end(), old.begin());
        const bool isNew = saved.size() == 44 && std::equal(saved.begin(), saved.end(), current.begin());
        require(isOld || isNew, "power loss produziu header parcial/ausente");
        sawOld |= isOld;
        sawNew |= isNew;
        require(readFile(fresh, "/record.pcm") == data, "power loss alterou PCM");
        expectPrevious(fresh);
    }
    require(sawOld && sawNew, "snapshots nao cobriram os dois lados do rename");
    expectUnchangedPcm(before, disk, data, physical);
    const Bytes finalImage = disk.bytes;
    m.powerOff(); // Tambem nao fecha PCM neste caso.
    Nor reboot(finalImage);
    Mount fresh(reboot);
    expectHeader(fresh, "/record.wav", header(data.size()));
    require(!exists(fresh, "/record.wav.tmp"), "rename nao removeu tmp no reboot");
    std::cout << "  " << disk.snapshots.size() << " snapshots prog/erase de .tmp/rename; "
              << "header sempre antigo ou novo; PCM fisico inalterado\n";
    report("update atomico com PCM aberto", disk.ops);
}

void norRules() {
    Nor disk;
    lfs_config config{};
    config.context = &disk;
    std::array<uint8_t, kProg> zeros{}, ones{};
    ones.fill(0xff);
    success(Nor::prog(&config, 0, 0, zeros.data(), zeros.size()), "NOR 1->0");
    const Bytes programmed = disk.bytes;
    require(Nor::prog(&config, 0, 0, ones.data(), ones.size()) == LFS_ERR_IO,
            "NOR aceitou 0->1 sem erase");
    require(disk.bytes == programmed, "programacao invalida modificou disco");
    require(Nor::prog(&config, 0, 1, zeros.data(), zeros.size()) == LFS_ERR_IO,
            "NOR aceitou prog desalinhado");
    require(Nor::read(&config, 0, 1, ones.data(), kRead) == LFS_ERR_IO,
            "NOR aceitou read desalinhado");
    require(Nor::erase(&config, kBlocks) == LFS_ERR_IO, "NOR aceitou bloco fora da particao");
    success(Nor::erase(&config, 0), "NOR erase");
    success(Nor::prog(&config, 0, 0, ones.data(), ones.size()), "NOR depois de erase");
    require(disk.ops.programs == 2 && disk.ops.erases == 1 &&
            disk.ops.programBytes == 512, "contabilizacao NOR incorreta");
}
} // namespace

int main() {
    std::cout << "LittleFS upstream v2.9.3; API " << LFS_VERSION_MAJOR << '.' << LFS_VERSION_MINOR
              << "; disco " << LFS_DISK_VERSION_MAJOR << '.' << LFS_DISK_VERSION_MINOR << '\n'
              << "NOR: 4864 KiB, block=4096, read=16, prog=256, cache=512, "
              << "lookahead=128, block_cycles=512; append=8192, checkpoint=32768\n";
    unsigned passed = 0, total = 0;
    const auto test = [&](const char *name, const std::function<void()> &body) {
        ++total;
        std::cout << "TEST " << name << '\n';
        try {
            body();
            ++passed;
            std::cout << "  OK\n";
        } catch (const std::exception &error) {
            std::cout << "  FALHOU: " << error.what() << '\n';
        }
    };
    std::pair<Result, Result> five{}, sixty{};
    bool fiveOk = false, sixtyOk = false;
    test("regras NOR e contadores", norRules);
    test("5s + dois WAV anteriores de 5s", [&] { five = compare(5, false); fiveOk = true; });
    test("60s + dois WAV anteriores de 5s", [&] { sixty = compare(60, false); sixtyOk = true; });
    test("escala 5s -> 60s: antigo proporcional, novo limitado", [&] {
        require(fiveOk && sixtyOk, "comparacoes anteriores falharam");
        require(sixty.first.final.programBytes >= five.first.final.programBytes * 10 &&
                sixty.first.final.erases >= five.first.final.erases * 10,
                "custo antigo nao escalou com duracao");
        require(sixty.second.final.programBytes <= five.second.final.programBytes + 8 * 1024 &&
                sixty.second.final.erases <= five.second.final.erases + 2,
                "custo novo escalou com duracao");
    });
    test("5s com pouco espaco: antigo NOSPC, novo conclui", [&] { compare(5, true); });
    test("60s com pouco espaco: antigo NOSPC, novo conclui", [&] { compare(60, true); });
    test("reboots dos checkpoints PCM de 5s sem close", [&] { checkpointReboots(5); });
    test("reboots dos checkpoints PCM de 60s sem close", [&] { checkpointReboots(60); });
    test("power loss durante header .tmp/rename, com pouco espaco", atomicHeaderReboots);
    std::cout << "LittleFS recording: " << passed << '/' << total << " testes OK\n"
              << "Medicoes sao operacoes NOR em memoria; nao sao tempos/speedup nem testes fisicos.\n";
    return passed == total ? 0 : 1;
}
