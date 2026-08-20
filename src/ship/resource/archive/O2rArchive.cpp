#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"

#ifdef __vita__
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <malloc.h>
#include <new>
#include <zlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include "coroutine.h"
#include "port_log.h"
#endif

namespace Ship {

#ifdef __vita__
#include <cstdio>

/* TEMPORARY DIAGNOSTIC helper: port_log()'s normal async queue can lose its
 * last message if a crash happens before the dedicated writer thread gets
 * scheduled - confirmed repeatedly this session, right when it mattered
 * most (the message immediately preceding a crash). A direct, synchronous
 * sceClibPrintf would fix that, but is only safe to call from the real
 * thread stack, never from inside the coroutine (see
 * docs/../02-vitasdk/04-kernel-core-apis.md's "manually-swapped stacks"
 * finding - any raw kernel syscall from the coroutine can crash on real
 * hardware, and sceClibPrintf is exactly that kind of call). Archive
 * loading during initial boot (Open()/the first LoadFile() calls) happens
 * on the real thread, before the coroutine exists - port_coroutine_in_coroutine()
 * tells us which context we're in at the call site, so this only takes the
 * synchronous path when it's actually safe to. */
static void ArchDiagLog(const char* fmt, ...) {
    /* Disabled again (2026-08-19): the earlier "hang" this was re-enabled
     * to chase turned out not to be a hang at all (just this same logging
     * being quiet - see the investigation history), so it's not needed for
     * that anymore. Per-resource CRC/fingerprint logging on every single
     * archive read (hundreds of files during character-roster load) has a
     * real, measurable cost and was slowing the app down during the
     * current shader-compile-failure investigation, which doesn't need
     * it. Single point of control, as before - uncomment the body if a
     * similar buffer-corruption hunt is needed again:
     *
     * char buf[512];
     * va_list ap;
     * va_start(ap, fmt);
     * sceClibVsnprintf(buf, sizeof(buf), fmt, ap);
     * va_end(ap);
     *
     * port_log("%s", buf);
     * if (!port_coroutine_in_coroutine()) {
     *     sceClibPrintf("%s", buf);
     * }
     */
    (void)fmt;
}

/* VitaSDK's prebuilt zlib 1.3.2 compiles inflate()/inflate_fast() with real
 * ARM NEON vld1/vst1 instructions (confirmed via objdump on
 * arm-vita-eabi/lib/libz.a - this isn't a guess). A plain std::vector<char>
 * (or new[]) only guarantees whatever alignment this newlib build's default
 * allocator happens to give a byte buffer, which is not guaranteed to meet
 * whatever NEON load/store alignment the compiled code expects. Real Vita
 * hardware's Cortex-A9 NEON unit can fault on an insufficiently-aligned
 * unaligned-form vld1/vst1 in ways Vita3K's software NEON emulation does not
 * reproduce - consistent with this project's established pattern of bugs
 * that only reproduce on real hardware. A recurring, non-deterministic
 * real-hardware crash landing with PC directly inside inflate_fast (not a
 * kernel trampoline - a genuine fault while executing zlib's own code) for
 * different archive entries at different times, which persisted even after
 * generously increasing buffer *size* padding, matches an alignment fault
 * far better than a buffer-overrun: size padding can't fix a start-address
 * alignment problem. 16 bytes covers NEON's widest common quad-word (Q
 * register) access; memalign guarantees it explicitly instead of hoping the
 * default allocator happens to provide it. */
class VitaAlignedBuffer {
public:
    explicit VitaAlignedBuffer(size_t size) : mSize(size) {
        mData = static_cast<char*>(memalign(16, size));
        if (mData != nullptr) {
            memset(mData, 0, size);
        }
    }
    ~VitaAlignedBuffer() {
        if (mData != nullptr) {
            free(mData);
        }
    }
    VitaAlignedBuffer(const VitaAlignedBuffer&) = delete;
    VitaAlignedBuffer& operator=(const VitaAlignedBuffer&) = delete;

    char* data() { return mData; }
    size_t size() const { return mSize; }

private:
    char* mData;
    size_t mSize;
};
#endif

/* mZipArchive MUST start null: Close() (called from the destructor) only
 * skips zip_close() when it is null, and Open() now has early-return paths
 * that leave it untouched. Left uninitialized, a failed Open() handed
 * zip_close() a garbage pointer and took a data abort. */
O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath), mZipArchive(nullptr) {
}

O2rArchive::~O2rArchive() {
    SPDLOG_TRACE("destruct o2rarchive: {}", GetPath());
    Close();
}

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
    if (mZipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

    auto zipEntryIndex = zip_name_locate(mZipArchive, filePath.c_str(), 0);
    if (zipEntryIndex < 0) {
        SPDLOG_TRACE("Failed to find file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    struct zip_stat zipEntryStat;
    zip_stat_init(&zipEntryStat);
    if (zip_stat_index(mZipArchive, zipEntryIndex, 0, &zipEntryStat) != 0) {
        SPDLOG_TRACE("Failed to get entry information for file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        return nullptr;
    }

#ifdef __vita__
    /* Logged before any allocation below (fileToLoad->Buffer, vitaReadScratch)
     * rather than after: an earlier placement after those allocations meant
     * a crash inside the allocation itself (operator new / _malloc_r, seen
     * on real hardware) never got logged at all - the diagnostic code was
     * unreachable dead code by the time the crash happened. port_log queues
     * to an async writer thread and does no syscalls itself, so it's safe to
     * call from here regardless of what happens below. */
    ArchDiagLog("SSB64: ARCHDIAG LoadFile path=%s size=%llu compsize=%llu compmethod=%u\n", filePath.c_str(),
             (unsigned long long)zipEntryStat.size, (unsigned long long)zipEntryStat.comp_size,
             (unsigned int)zipEntryStat.comp_method);
#endif

    struct zip_file* zipEntryFile = zip_fopen_index(mZipArchive, zipEntryIndex, 0);
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

#ifdef __vita__
    /* zlib's inflate_fast() fast path (and libzip's own internal copy buffer) can
     * write in wider bursts than the exact byte count requested, especially
     * pathological for tiny entries (e.g. this archive's 5-byte "version"
     * file, deflate-compressed to 10 bytes - decompression output smaller
     * than a single wide store). Passing zip_fread a destination sized to
     * the exact entry size gave a real-hardware crash inside inflate_fast
     * every time this file's entry was loaded (confirmed via a stack-local
     * diagnostic marker matched against the coredump).
     *
     * Fix: give zip_fread a scratch buffer with slack to write into, then
     * copy exactly zipEntryStat.size bytes into fileToLoad->Buffer. Padding
     * fileToLoad->Buffer itself would be simpler but is unsafe - several
     * callers (Archive::ParseManifest's json::parse(begin,end),
     * OtrArchive's file-list split, ResourceLoader's OTR_HEADER_SIZE check)
     * trust Buffer->size() to be the exact file length, and trailing zero
     * padding would corrupt all of them.
     *
     * 4096 bytes of slack was enough to stop the crash on this archive's
     * 5-byte "version" entry, but real-hardware testing then hit the
     * identical inflate_fast/inflate/zip_source_read crash signature again
     * for some other, larger entry later in boot. That instance turned out
     * to have PC landing *directly inside inflate_fast's own compiled code*
     * (not a kernel trampoline reached via a syscall) - a genuine fault
     * while zlib's own instructions executed, which a buffer-size padding
     * fix cannot address (it only helps a decompressor that overshoots past
     * the end of the buffer, not one that faults on the buffer's start
     * address). VitaAlignedBuffer (top of file) was tried here to address
     * that (VitaSDK's zlib genuinely compiles inflate_fast with real ARM
     * NEON instructions, confirmed via objdump), but applying the same
     * memalign()-based alignment to the much larger archive buffer in
     * Open() caused a worse, earlier crash (a data abort from a zeroed
     * vtable) that was reverted. This smaller use was reverted alongside it
     * back to plain std::vector<char>, not because it was shown to cause
     * the same problem, but to isolate the variable: real-hardware testing
     * after the Open()-side revert alone still showed run-to-run crash
     * depth varying as much as it always has, so this reversion is for a
     * clean baseline to test the alignment hypothesis against, not a
     * confirmed-safe verdict either way. If revisiting, test in isolation
     * (a minimal reproduction, not a full boot chain) before redeploying.
     *
     * NOT a fresh per-call allocation anymore: a function-local static pool,
     * grown on demand and never shrunk. Real-hardware testing traced a
     * separate, still-recurring crash (crc32_z, non-deterministic exact
     * location) to newlib malloc's mmap_chunk() path, taken for any single
     * allocation big enough to cross DEFAULT_MMAP_THRESHOLD (128 KiB) -
     * unlike ordinary sbrk-backed allocations (a one-time kernel cost at
     * startup), mmap_chunk() makes a fresh kernel call on *every* qualifying
     * request (see port.cpp's mallopt comment - this project's own audio
     * assets routinely exceed 128 KiB, one wavetable blob runs ~1 MB), and
     * that kernel call crashes when made from the game coroutine's
     * manually-swapped stack, the same as every other kernel syscall from
     * that context this session found. Confirmed NOT fixable by pre-warming
     * (a fresh call is still a fresh kernel call, unlike a one-time lazy
     * lock) and the M_MMAP_THRESHOLD tunable set in port.cpp's main() is
     * itself confirmed not fully reliable on this newlib build. Reusing one
     * pool sidesteps the problem differently: once it's grown large enough
     * (which happens here on the real thread, since this function's very
     * first call - loading this archive's own "version" entry - runs
     * before any coroutine exists), later calls for equal-or-smaller data
     * never need a fresh allocation at all, regardless of which thread
     * context they run in. A function-local static's first-use
     * initialization is itself thread-safety-guarded by the compiler
     * ("magic statics") - relying on that guard already being satisfied by
     * this same real-thread "version" call, rather than re-deriving
     * safety here, since a fresh magic-static guard's own lazy lock
     * creation would otherwise be exactly one more instance of this
     * session's "first use from the coroutine" pattern. */
    static std::vector<char> sVitaReadScratchPool;
    const size_t neededSize = zipEntryStat.size + (64 * 1024);
    if (sVitaReadScratchPool.size() < neededSize) {
        sVitaReadScratchPool.resize(neededSize, 0);
    }
    char* vitaReadScratch = sVitaReadScratchPool.data();
    if (vitaReadScratch == nullptr) {
        SPDLOG_TRACE("Failed to allocate aligned read buffer for {} in zip archive {}.", filePath, GetPath());
        return nullptr;
    }
#endif

#ifdef __vita__
    /* TEMPORARY DIAGNOSTIC: identifying which specific archive entry's
     * decompression triggers a recurring real-hardware crash inside zlib
     * (inflate_fast et al. - confirmed by matching internal-state stack
     * values, not by symbol name, since addr2line keeps misattributing the
     * PC to whatever debug-info-bearing symbol happens to sit nearest
     * zlib's symbol-stripped internals). No syscalls here (pure memory
     * writes to a stack local), so this is safe to call from the game
     * coroutine's stack. A static/global buffer was tried first and
     * proved unreadable after the fact: real-hardware psp2dmp coredumps
     * only capture a handful of small memory windows (chiefly stack pages
     * near the fault), not the full data/bss segment. This local instead
     * sits in LoadFile()'s own stack frame, which - since the crash is
     * reached from further down the same call chain (LoadFile -> zip_fread
     * -> ... -> inflate) - lands within that captured window. volatile so
     * the optimizer can't treat these dead-after-use writes as eliminable
     * (nothing in this function reads them back), and a distinctive magic
     * marker so the value can be found by grepping the coredump's raw
     * bytes directly instead of computing a stack address by hand. */
    volatile char archDiagMarker[256];
    std::snprintf((char*)archDiagMarker, sizeof(archDiagMarker),
                  "ARCHDIAGv1:%s:size=%llu:compsize=%llu:compmethod=%u:END",
                  filePath.c_str(), (unsigned long long)zipEntryStat.size,
                  (unsigned long long)zipEntryStat.comp_size, (unsigned int)zipEntryStat.comp_method);
#endif

#ifdef __vita__
    if (zip_fread(zipEntryFile, vitaReadScratch, zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }
    std::memcpy(fileToLoad->Buffer->data(), vitaReadScratch, zipEntryStat.size);

    /* TEMPORARY DIAGNOSTIC: per-resource output fingerprint, for comparing
     * the same asset's decompressed content across separate runs (a
     * fingerprint that differs for the same file between two runs is
     * direct proof of non-deterministic corruption, independent of
     * whether either run actually crashed). Only reached when zip_fread
     * above didn't crash/corrupt this call outright - the input-buffer
     * check above this function catches the case where corruption already
     * happened before this call started. */
    {
        uint32_t outCrc = (uint32_t)crc32(0L, (const Bytef*)fileToLoad->Buffer->data(), (uInt)zipEntryStat.size);
        size_t headLen = std::min<size_t>(16, zipEntryStat.size);
        size_t tailLen = std::min<size_t>(16, zipEntryStat.size);
        const unsigned char* bufBytes = (const unsigned char*)fileToLoad->Buffer->data();
        char headHex[16 * 2 + 1] = {0};
        char tailHex[16 * 2 + 1] = {0};
        for (size_t i = 0; i < headLen; i++) {
            sceClibSnprintf(headHex + i * 2, 3, "%02x", bufBytes[i]);
        }
        for (size_t i = 0; i < tailLen; i++) {
            sceClibSnprintf(tailHex + i * 2, 3, "%02x", bufBytes[zipEntryStat.size - tailLen + i]);
        }
        ArchDiagLog("SSB64: ARCHDIAG BUF file=%s size=%llu crc=%08x head=%s tail=%s\n", filePath.c_str(),
                 (unsigned long long)zipEntryStat.size, (unsigned int)outCrc, headHex, tailHex);
    }
#else
    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }
#endif

#ifdef __vita__
    /* TEMPORARY DIAGNOSTIC: bisecting whether the recurring crc32_z crash
     * happens inside zip_fread's decompression (above, already confirmed
     * to complete - the BUF fingerprint log above this point is proof) or
     * inside zip_fclose() specifically - libzip verifies the entry's
     * stored CRC-32 against the just-decompressed data as part of closing
     * it, which is itself a crc32_z call, separate from our own fingerprint
     * crc32() call above. If this line is consistently missing from a run
     * that crashed in crc32_z while the BUF line above it consistently
     * isn't, that pinpoints zip_fclose()'s internal verification as the
     * culprit call site. */
    ArchDiagLog("SSB64: ARCHDIAG about to zip_fclose file=%s\n", filePath.c_str());
#endif
    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
    }
#ifdef __vita__
    ArchDiagLog("SSB64: ARCHDIAG zip_fclose done file=%s\n", filePath.c_str());
#endif

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

#ifdef __vita__
/* Seek-free libzip source for real Vita hardware.
 *
 * The stock stdio source previously crashed deterministically inside
 * sceIoLseek32. Loading the whole archive into a vector avoided that call,
 * but permanently consumed roughly 12 MiB of the already constrained
 * newlib heap. By the first gameplay shader compile the heap had only a few
 * KiB free, and SceShaccCg returned only "fatal internal error on line -1".
 *
 * Keep one raw descriptor open and implement libzip's logical seek as an
 * in-memory offset update. Reads use sceIoPread(), which takes an explicit
 * offset and never mutates or consults the descriptor's seek position. This
 * preserves the lseek workaround without retaining the archive in RAM. */
struct VitaPreadZipSource {
    SceUID fd;
    zip_uint64_t size;
    zip_uint64_t offset;
    zip_error_t error;
};

static zip_int64_t VitaPreadZipSourceCallback(void* statePtr, void* data, zip_uint64_t len,
                                              zip_source_cmd_t command) {
    VitaPreadZipSource* state = static_cast<VitaPreadZipSource*>(statePtr);

    switch (command) {
        case ZIP_SOURCE_OPEN:
            state->offset = 0;
            zip_error_set(&state->error, ZIP_ER_OK, 0);
            return 0;

        case ZIP_SOURCE_READ: {
            if (data == nullptr && len != 0) {
                zip_error_set(&state->error, ZIP_ER_INVAL, 0);
                return -1;
            }
            if (state->offset >= state->size || len == 0) {
                return 0;
            }

            const zip_uint64_t remaining = state->size - state->offset;
            /* Keep requests comfortably inside sceIoPread's SceSize/int
             * result range. libzip accepts short reads and will request the
             * remainder on its next callback. */
            const zip_uint64_t request64 = std::min<zip_uint64_t>(
                std::min<zip_uint64_t>(len, remaining), 1024u * 1024u);
            const int got = sceIoPread(state->fd, data, static_cast<SceSize>(request64),
                                       static_cast<SceOff>(state->offset));
            if (got < 0) {
                port_log("SSB64: Vita O2R pread failed offset=%llu request=%u rc=0x%08X\n",
                         (unsigned long long)state->offset, (unsigned int)request64, (unsigned int)got);
                zip_error_set(&state->error, ZIP_ER_READ, got);
                return -1;
            }
            if (got == 0) {
                port_log("SSB64: Vita O2R unexpected EOF offset=%llu request=%u\n",
                         (unsigned long long)state->offset, (unsigned int)request64);
                zip_error_set(&state->error, ZIP_ER_EOF, 0);
                return -1;
            }
            state->offset += static_cast<zip_uint64_t>(got);
            return got;
        }

        case ZIP_SOURCE_CLOSE:
            /* The descriptor belongs to the source for its whole lifetime.
             * libzip can OPEN/CLOSE it repeatedly while extracting entries;
             * only FREE releases the kernel handle. */
            return 0;

        case ZIP_SOURCE_STAT: {
            if (data == nullptr || len < sizeof(zip_stat_t)) {
                zip_error_set(&state->error, ZIP_ER_INVAL, 0);
                return -1;
            }
            zip_stat_t* stat = static_cast<zip_stat_t*>(data);
            zip_stat_init(stat);
            stat->size = state->size;
            stat->valid = ZIP_STAT_SIZE;
            return sizeof(zip_stat_t);
        }

        case ZIP_SOURCE_ERROR:
            return zip_error_to_data(&state->error, data, len);

        case ZIP_SOURCE_FREE:
            if (state->fd >= 0) {
                sceIoClose(state->fd);
                state->fd = -1;
            }
            zip_error_fini(&state->error);
            delete state;
            return 0;

        case ZIP_SOURCE_SEEK: {
            const zip_int64_t newOffset = zip_source_seek_compute_offset(
                state->offset, state->size, data, len, &state->error);
            if (newOffset < 0) {
                return -1;
            }
            state->offset = static_cast<zip_uint64_t>(newOffset);
            return 0;
        }

        case ZIP_SOURCE_TELL:
            return static_cast<zip_int64_t>(state->offset);

        case ZIP_SOURCE_SUPPORTS:
            return ZIP_SOURCE_SUPPORTS_SEEKABLE;

        default:
            zip_error_set(&state->error, ZIP_ER_OPNOTSUPP, 0);
            return -1;
    }
}

static zip_source_t* VitaCreatePreadZipSource(const std::string& path, zip_error_t* zipError) {
    SceIoStat stat;
    sceClibMemset(&stat, 0, sizeof(stat));
    const int statRc = sceIoGetstat(path.c_str(), &stat);
    if (statRc < 0) {
        SPDLOG_ERROR("sceIoGetstat failed for \"{}\" (rc=0x{:08X})", path, (unsigned int)statRc);
        zip_error_set(zipError, ZIP_ER_OPEN, statRc);
        return nullptr;
    }

    const SceOff fileSize = stat.st_size;
    if (fileSize <= 0) {
        SPDLOG_ERROR("Archive \"{}\" reports size {}", path, (long long)fileSize);
        zip_error_set(zipError, ZIP_ER_INVAL, 0);
        return nullptr;
    }

    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) {
        SPDLOG_ERROR("sceIoOpen failed for \"{}\" (rc=0x{:08X})", path, (unsigned int)fd);
        zip_error_set(zipError, ZIP_ER_OPEN, fd);
        return nullptr;
    }

    VitaPreadZipSource* state = new (std::nothrow) VitaPreadZipSource;
    if (state == nullptr) {
        sceIoClose(fd);
        zip_error_set(zipError, ZIP_ER_MEMORY, 0);
        return nullptr;
    }
    state->fd = fd;
    state->size = static_cast<zip_uint64_t>(fileSize);
    state->offset = 0;
    zip_error_init(&state->error);

    zip_source_t* source = zip_source_function_create(VitaPreadZipSourceCallback, state, zipError);
    if (source == nullptr) {
        sceIoClose(fd);
        zip_error_fini(&state->error);
        delete state;
    } else {
        port_log("SSB64: Vita O2R source=pread path=%s size=%llu\n", path.c_str(),
                 (unsigned long long)fileSize);
    }
    return source;
}
#endif

bool O2rArchive::Open() {
#ifdef __vita__
    zip_error_t zipErr;
    zip_error_init(&zipErr);
    zip_source_t* source = VitaCreatePreadZipSource(GetPath(), &zipErr);
    if (source == nullptr) {
        SPDLOG_ERROR("Failed to create pread zip source for \"{}\": {}", GetPath(),
                     zip_error_strerror(&zipErr));
        zip_error_fini(&zipErr);
        return false;
    }

    mZipArchive = zip_open_from_source(source, ZIP_RDONLY, &zipErr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file through pread source \"{}\": {}", GetPath(),
                     zip_error_strerror(&zipErr));
        zip_source_free(source); /* only ours to free when the open failed */
        zip_error_fini(&zipErr);
        return false;
    }
    zip_error_fini(&zipErr);

    SPDLOG_INFO("Loaded archive \"{}\" through seek-free Vita pread source", GetPath());
#else
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }
#endif

    auto zipNumEntries = zip_get_num_entries(mZipArchive, 0);
    for (auto i = 0; i < zipNumEntries; i++) {
        auto zipEntryName = zip_get_name(mZipArchive, i, 0);

        // It is possible for directories to have entries in a zip
        // file, we don't want those indexed as files in the archive
        if (zipEntryName[strlen(zipEntryName) - 1] == '/') {
            continue;
        }

        IndexFile(zipEntryName);
    }

    return true;
}

bool O2rArchive::Close() {
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Cannot close zip file. Zip file not loaded. \"{}\"", GetPath());
        return false;
    }

    if (zip_close(mZipArchive) == -1) {
        SPDLOG_ERROR("Failed to close zip file \"{}\"", GetPath());
        return false;
    }

    mZipArchive = nullptr;
    return true;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
#ifdef __vita__
    /* The Vita source exposes only pread-based read operations. Nothing in
     * the boot path writes to an archive; fail loudly rather than silently
     * reopening a second writable handle with different source semantics. */
    (void)data;
    SPDLOG_ERROR("Cannot write to zip \"{}\": archives are read-only on Vita (file \"{}\")", GetPath(), filePath);
    return false;
#else
    if (!mZipArchive) {
        SPDLOG_ERROR("Cannot write to zip: Archive is not open.");
        return false;
    }

    // Create a new zip source from the data buffer
    zip_source_t* source = zip_source_buffer(mZipArchive, data.data(), data.size(), 0);
    if (!source) {
        SPDLOG_ERROR("Failed to create zip source for file \"{}\"", filePath);
        return false;
    }

    // Add or replace the file in the zip archive
    if (zip_file_add(mZipArchive, filePath.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
        SPDLOG_ERROR("Failed to add file \"{}\" to ZIP", filePath);
        zip_source_free(source);
        return false;
    }

    // Save changes to disk
    if (zip_close(mZipArchive) < 0) {
        zip_error_t* error = zip_get_error(mZipArchive);
        SPDLOG_ERROR("Failed to save changes to zip archive: {} ({})", zip_error_strerror(error),
                     zip_error_code_zip(error));
        zip_discard(mZipArchive); // Close zip and discard changes
        return false;
    }

    SPDLOG_INFO("Successfully wrote file: {}", filePath);

    // Reopen the zip file so that it may continued to be used by libultraship
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to reopen zip file after writing.");
        return false;
    }

    IndexFile(filePath);

    // Success
    return true;
#endif
}

} // namespace Ship
