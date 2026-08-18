#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"

#ifdef __vita__
#include <algorithm>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#endif

namespace Ship {
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

    struct zip_file* zipEntryFile = zip_fopen_index(mZipArchive, zipEntryIndex, 0);
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }

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

    zip_error_t zipErr;
    zip_error_init(&zipErr);
    /* freep = 0: libzip must not take ownership of mArchiveBuffer's storage. */
    zip_source_t* source = zip_source_buffer_create(mArchiveBuffer.data(), mArchiveBuffer.size(), 0, &zipErr);
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

    SPDLOG_INFO("Loaded archive \"{}\" into memory ({} bytes)", GetPath(), mArchiveBuffer.size());
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
