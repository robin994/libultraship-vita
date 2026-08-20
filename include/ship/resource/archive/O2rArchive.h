#pragma once

#undef _DLL

#include <string>
#include <stdint.h>
#include <string>
#include <vector>

#include "zip.h"

#include "ship/resource/File.h"
#include "ship/resource/Resource.h"
#include "ship/resource/archive/Archive.h"

namespace Ship {
struct File;

class O2rArchive final : virtual public Archive {
  public:
    O2rArchive(const std::string& archivePath);
    ~O2rArchive();

    bool Open();
    bool Close();
    bool WriteFile(const std::string& filename, const std::vector<uint8_t>& data);

    std::shared_ptr<File> LoadFile(const std::string& filePath);
    std::shared_ptr<File> LoadFile(uint64_t hash);

  private:
    zip_t* mZipArchive;

    /* ABI/layout compatibility for the Vita incremental Makefile. This
     * vector used to own the complete archive and the two fields below
     * described its real span. The pread source no longer fills or reads
     * any of them, so they consume only their small object headers and no
     * archive storage.
     *
     * Keep them until every consumer has reliable header dependency files:
     * Archive is a virtual base, so removing these members changes its
     * offset inside O2rArchive. A stale ArchiveManager.o then constructs the
     * new layout but accesses enable_shared_from_this at the old offset,
     * interpreting bytes from the archive path as a pointer and crashing
     * before Open() is reached. */
    std::vector<uint8_t> mArchiveBuffer;

#ifdef __vita__
    uint32_t mArchiveBufferBaselineCrc = 0;
    size_t mArchiveBufferRealSize = 0;
#endif
};
} // namespace Ship
