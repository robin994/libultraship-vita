#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"

#ifdef __vita__
#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include "port_log.h"
#endif

namespace Ship {

#ifdef __vita__
#include <cstdio>

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
    port_log("SSB64: ARCHDIAG LoadFile path=%s size=%llu compsize=%llu compmethod=%u\n", filePath.c_str(),
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
    /* Same reasoning as mArchiveBuffer's padding in Open(): zlib's
     * inflate_fast() fast path (and libzip's own internal copy buffer) can
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
     * (a minimal reproduction, not a full boot chain) before redeploying. */
    std::vector<char> vitaReadScratch(zipEntryStat.size + (64 * 1024), 0);
    if (vitaReadScratch.data() == nullptr) {
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
    if (zip_fread(zipEntryFile, vitaReadScratch.data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }
    std::memcpy(fileToLoad->Buffer->data(), vitaReadScratch.data(), zipEntryStat.size);
#else
    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }
#endif

    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
    }

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

#ifdef __vita__
/* Read an entire file into `out` using raw kernel I/O.
 *
 * Real-hardware testing repeatedly produced a psp2dmp with the main thread
 * faulting inside sceIoLseek32, reached through libzip's stdio file source
 * (_zip_stdio_op_seek -> _fseeko_r -> __sseek -> _lseek_r -> sceIoLseek32),
 * always seeking to the exact same archive offset. The same binary runs
 * fine under Vita3K, whose HLE filesystem never exercises that path the
 * same way. Rather than keep chasing the syscall, drop the dependency:
 * pull the archive into RAM once with sceIoOpen/sceIoRead (no lseek, no
 * newlib stdio) and hand libzip a plain memory buffer. Random access then
 * costs nothing and never touches the filesystem again - which is also a
 * straight win on a console reading from a slow microSD.
 *
 * Size comes from sceIoGetstat rather than a seek-to-end for the same
 * reason: it avoids lseek entirely. */
static bool VitaReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    SceIoStat stat;
    sceClibMemset(&stat, 0, sizeof(stat));
    const int statRc = sceIoGetstat(path.c_str(), &stat);
    if (statRc < 0) {
        SPDLOG_ERROR("sceIoGetstat failed for \"{}\" (rc=0x{:08X})", path, (unsigned int)statRc);
        return false;
    }

    const SceOff fileSize = stat.st_size;
    if (fileSize <= 0) {
        SPDLOG_ERROR("Archive \"{}\" reports size {}", path, (long long)fileSize);
        return false;
    }

    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) {
        SPDLOG_ERROR("sceIoOpen failed for \"{}\" (rc=0x{:08X})", path, (unsigned int)fd);
        return false;
    }

    out.resize(static_cast<size_t>(fileSize));

    /* Chunked so a single huge request can't trip the driver, and so a
     * short read is detected rather than silently leaving a partial tail. */
    const size_t kChunk = 1u * 1024u * 1024u;
    size_t done = 0;
    while (done < out.size()) {
        const size_t want = std::min(kChunk, out.size() - done);
        const int got = sceIoRead(fd, out.data() + done, want);
        if (got <= 0) {
            break;
        }
        done += static_cast<size_t>(got);
    }
    sceIoClose(fd);

    if (done != out.size()) {
        SPDLOG_ERROR("Short read on \"{}\": got {} of {} bytes", path, (unsigned int)done, (unsigned int)out.size());
        out.clear();
        return false;
    }
    return true;
}
#endif

bool O2rArchive::Open() {
#ifdef __vita__
    if (!VitaReadWholeFile(GetPath(), mArchiveBuffer)) {
        SPDLOG_ERROR("Failed to read zip file into memory \"{}\"", GetPath());
        return false;
    }

    /* Real-hardware testing surfaced crashes deep inside zlib (crc32_z) and
     * in resource parsing right after this archive's decompressed data,
     * both non-deterministic in exactly which entry/resource tripped them.
     * A file-backed libzip source tolerates a decompressor reading a few
     * bytes past a logical entry's end (the OS just has more file bytes
     * behind it); a bare fixed-length memory buffer does not - the read
     * lands past mArchiveBuffer's exact allocation and faults or returns
     * garbage right at the edge. zip_source_buffer_create is told the real
     * size below, so libzip's own bounds/CRC logic is unaffected - this
     * padding only gives an accidental over-read somewhere safe to land.
     *
     * 4096 wasn't enough on its own - LoadFile()'s own scratch-buffer
     * padding (this file, above) hit the identical crash signature on a
     * different, later entry even with this padding already in place.
     * Raised alongside that one to 64 KiB as a generous stopgap. A 16-byte-
     * aligned memalign() replacement for this buffer was tried (targeting a
     * confirmed NEON alignment requirement in VitaSDK's zlib) and reverted:
     * it introduced a worse, earlier crash (a data abort from a zeroed
     * vtable, consistent with the ~12 MB memalign() overlapping live heap
     * memory) - see O2rArchive.h's mArchiveBuffer comment. */
    const size_t kRealSize = mArchiveBuffer.size();
    mArchiveBuffer.resize(kRealSize + (64 * 1024), 0);

    zip_error_t zipErr;
    zip_error_init(&zipErr);
    /* freep = 0: libzip must not take ownership of mArchiveBuffer's storage. */
    zip_source_t* source = zip_source_buffer_create(mArchiveBuffer.data(), kRealSize, 0, &zipErr);
    if (source == nullptr) {
        SPDLOG_ERROR("Failed to create in-memory zip source for \"{}\": {}", GetPath(),
                     zip_error_strerror(&zipErr));
        zip_error_fini(&zipErr);
        mArchiveBuffer.clear();
        return false;
    }

    mZipArchive = zip_open_from_source(source, ZIP_RDONLY, &zipErr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file from memory \"{}\": {}", GetPath(), zip_error_strerror(&zipErr));
        zip_source_free(source); /* only ours to free when the open failed */
        zip_error_fini(&zipErr);
        mArchiveBuffer.clear();
        return false;
    }
    zip_error_fini(&zipErr);

    SPDLOG_INFO("Loaded archive \"{}\" into memory ({} bytes)", GetPath(), kRealSize);
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
#ifdef __vita__
    /* Safe to drop only now that zip_close() has released the buffer source
     * that was pointing into it (see Open()). */
    mArchiveBuffer.clear();
    mArchiveBuffer.shrink_to_fit();
#endif
    return true;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
#ifdef __vita__
    /* Vita opens archives read-only from an in-memory buffer (see Open()),
     * so there is no on-disk handle to write back through. Nothing in the
     * boot path writes to an archive; fail loudly rather than silently
     * corrupting state or reopening a second, disk-backed handle. */
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
