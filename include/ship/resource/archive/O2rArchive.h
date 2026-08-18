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

    /* Vita only: the whole archive, read into RAM up front via raw sceIo*
     * calls so libzip never seeks/reads through newlib stdio. libzip's
     * buffer source does NOT own or copy this, so it must outlive
     * mZipArchive - see O2rArchive::Open(). Empty on every other platform. */
    std::vector<uint8_t> mArchiveBuffer;
};
} // namespace Ship
