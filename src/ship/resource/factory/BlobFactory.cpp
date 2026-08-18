#include "ship/resource/factory/BlobFactory.h"
#include "ship/resource/type/Blob.h"
#include "spdlog/spdlog.h"

namespace Ship {
std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryBlobV0::ReadResource(std::shared_ptr<Ship::File> file,
                                          std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto blob = std::make_shared<Blob>(initData);
    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);

    uint32_t dataSize = reader->ReadUInt32();

    // Small zero-pad for N64 code that overreads by a few bytes
    // (e.g. compressed MIDI parser). Large audio DMA overreads are handled by
    // AudioDma_Clamp in osPiStartDma instead.
    constexpr uint32_t kBlobPadding = 16;
#ifdef __vita__
    /* Real-hardware testing hit a crash reading this loop one byte at a
     * time via ReadUByte()/push_back() - PC landed inside trivial pointer
     * accessors (shared_ptr::get(), vector::size()) on MemoryStream's
     * underlying buffer, which can only fault if that object itself is a
     * wild/dangling pointer - confirmed independent of the earlier spdlog
     * async-logger crash this session also found and fixed (see
     * docs/bugs/). A single bulk Read() call instead of dataSize separate
     * ReadUByte() calls avoids whatever specific interaction triggers it,
     * and is also the correct fix on its own merits. */
    blob->Data.resize(dataSize + kBlobPadding, 0);
    if (dataSize > 0) {
        reader->Read(blob->Data.data(), (int32_t)dataSize);
    }
#else
    blob->Data.reserve(dataSize + kBlobPadding);

    for (uint32_t i = 0; i < dataSize; i++) {
        blob->Data.push_back(reader->ReadUByte());
    }

    blob->Data.resize(dataSize + kBlobPadding, 0);
#endif

    return blob;
}
} // namespace Ship
