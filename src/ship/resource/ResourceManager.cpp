#include "ship/resource/ResourceManager.h"
#include <spdlog/spdlog.h>
#include "ship/resource/File.h"
#include "ship/resource/archive/Archive.h"
#include <algorithm>
#include <thread>
#include "ship/utils/StringHelper.h"
#include "ship/utils/Utils.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/Context.h"

namespace Ship {

#ifdef __vita__
/* Real-hardware Vita testing traced a deterministic crash - always at the
 * very first resource load, always the same kernel object UID, no ASLR on
 * this platform to make it look different run to run - to pte_osMutexLock
 * inside a std::condition_variable's destruction (see docs/bugs/). A
 * std::promise/std::future pair's shared state always allocates a real
 * mutex + condition_variable, even when resolved purely synchronously with
 * no cross-thread waiting ever happening (LoadResourceAsync's Vita branch
 * below does exactly that): the crash is in DESTROYING that shared state
 * from the game coroutine's manually swapped stack, not in using it.
 * Removing the (unused-on-Vita) BS::thread_pool and spdlog's async logger
 * thread pool earlier this session didn't touch this - this is a separate
 * instance of the same "syscall from that stack faults" bug, from a
 * std::promise/future created fresh on literally every single resource
 * load throughout the game's lifetime, not just boot.
 *
 * There is no way to get a std::shared_future without a promise/future
 * shared state somewhere underneath it, so the fix here is to never let
 * that state's destructor run on the coroutine's stack: keep every
 * completed promise alive indefinitely instead of letting it go out of
 * scope. This is a deliberate, unbounded leak (~100-200 bytes per resource
 * load) - real but slow relative to Vita's 256MiB heap, and clearing it
 * safely would mean draining this list from the main thread's real stack
 * on a frame boundary, which is a larger change than justified while this
 * is still the first-order blocker to booting at all. Revisit if a long
 * play session's leaked total becomes an actual problem. */
static std::vector<std::shared_ptr<void>> sVitaLeakedResourcePromises;
#endif

ResourceFilter::ResourceFilter(const std::list<std::string>& includeMasks, const std::list<std::string>& excludeMasks,
                               const uintptr_t owner, const std::shared_ptr<Archive> parent)
    : IncludeMasks(includeMasks), ExcludeMasks(excludeMasks), Owner(owner), Parent(parent) {
}

size_t ResourceIdentifier::GetHash() const {
    return mHash;
}

ResourceIdentifier::ResourceIdentifier(const std::string& path, const uintptr_t owner,
                                       const std::shared_ptr<Archive> parent)
    : Path(path), Owner(owner), Parent(parent) {
    mHash = CalculateHash();
}

ResourceIdentifier::ResourceIdentifier(std::string&& path, const uintptr_t owner, const std::shared_ptr<Archive> parent)
    : Path(std::move(path)), Owner(owner), Parent(parent) {
    mHash = CalculateHash();
}

bool ResourceIdentifier::operator==(const ResourceIdentifier& rhs) const {
    return Owner == rhs.Owner && Path == rhs.Path && Parent == rhs.Parent;
}

size_t ResourceIdentifier::CalculateHash() {
    size_t hash = Math::HashCombine(std::hash<std::string>{}(Path), std::hash<std::uintptr_t>{}(Owner));
    if (Parent != nullptr) {
        hash = Math::HashCombine(hash, std::hash<std::string>{}(Parent->GetPath()));
    }
    return hash;
}

size_t ResourceIdentifierHash::operator()(const ResourceIdentifier& rcd) const {
    return rcd.GetHash();
}

ResourceManager::ResourceManager() {
}

void ResourceManager::Init(const std::vector<std::string>& archivePaths,
                           const std::unordered_set<uint32_t>& validHashes, int32_t reservedThreadCount) {
    mResourceLoader = std::make_shared<ResourceLoader>();
    mArchiveManager = std::make_shared<ArchiveManager>();
    GetArchiveManager()->Init(archivePaths, validHashes);

#ifdef __vita__
    // Real-hardware testing this session repeatedly showed the main thread
    // parked in a generic kernel wait (at a point that moved around between
    // runs) while this pool's own worker thread(s) sat "Running" in a
    // coredump doing real file I/O (sceIoWrite/sceIoLseek32) - capping the
    // pool to a single worker didn't resolve it either. Rinnegatamante
    // (author of the Vita-patched rendering layer this port's libultraship
    // fork is merged from) confirmed he removed ResourceManager's thread
    // pool entirely as one of several Vita-specific optimizations in his own
    // fork - every LoadResourceAsync/LoadResourcesAsync/DirtyResources/
    // UnloadResourcesAsync call site below now runs synchronously on Vita
    // instead of submitting to mThreadPool, so the pool itself is never
    // used there and isn't created at all.
    (void)reservedThreadCount;
    /* Real-hardware testing traced a crash to pte_mutex_check_need_init
     * (VitaSDK's pthread-embedded mutex, lazily creating its underlying
     * kernel object on first lock()) inside CheckCache()'s mMutex lock -
     * the same "syscall from the game coroutine's manually swapped stack
     * faults on real hardware" bug as everywhere else in docs/bugs/. A
     * pre-warm-on-main-thread workaround was tried here first; confirmed
     * with the original author of this port's Vita rendering layer
     * (Rinnegatamante) that his own working fork doesn't use a mutex in
     * ResourceManager at all - Vita resource loading is single-threaded
     * through the game coroutine, so there's no real concurrent access to
     * protect against in the first place. mMutex's three lock sites
     * (CheckCache, LoadResourceProcess, UnloadResource) are now skipped
     * entirely on Vita instead of pre-warmed. */
#else
    // the extra `- 1` is because we reserve an extra thread for spdlog
    size_t threadCount = std::max(1, (int32_t)(std::thread::hardware_concurrency() - reservedThreadCount - 1));
    mThreadPool = std::make_shared<BS::thread_pool>(threadCount);

    if (!IsLoaded()) {
        // Nothing ever unpauses the thread pool since nothing will ever try to load the archive again.
        mThreadPool->pause();
    }
#endif
}

ResourceManager::~ResourceManager() {
    SPDLOG_INFO("destruct ResourceManager");
}

bool ResourceManager::IsLoaded() {
    return mArchiveManager != nullptr && mArchiveManager->IsLoaded();
}

std::shared_ptr<File> ResourceManager::LoadFileProcess(const std::string& filePath) {
    auto file = mArchiveManager->LoadFile(filePath);
    if (file != nullptr) {
        SPDLOG_TRACE("Loaded File {} on ResourceManager", filePath);
    } else {
        SPDLOG_TRACE("Could not load File {} in ResourceManager", filePath);
    }
    return file;
}

std::shared_ptr<File> ResourceManager::LoadFileProcess(const ResourceIdentifier& identifier) {
    if (identifier.Parent == nullptr) {
        return LoadFileProcess(identifier.Path);
    }
    auto archive = identifier.Parent;
    auto file = archive->LoadFile(identifier.Path);
    if (file != nullptr) {
        SPDLOG_TRACE("Loaded File {} on ResourceManager", identifier.Path);
    } else {
        SPDLOG_TRACE("Could not load File {} in ResourceManager", identifier.Path);
    }
    return file;
}

std::shared_ptr<IResource> ResourceManager::LoadResourceProcess(const ResourceIdentifier& identifier, bool loadExact,
                                                                std::shared_ptr<ResourceInitData> initData) {
    // Check for and remove the OTR signature
    if (OtrSignatureCheck(identifier.Path.c_str())) {
        const auto newFilePath = identifier.Path.substr(7);
        return LoadResourceProcess({ newFilePath, identifier.Owner, identifier.Parent }, false, initData);
    }

    // Cache the starts_with check to avoid repeated string comparisons
    const bool isAltPath = identifier.Path.starts_with(IResource::gAltAssetPrefix);
    const bool shouldCheckAlt = !loadExact && mAltAssetsEnabled && !isAltPath;

    // Attempt to load the alternate version of the asset, if we fail then we continue trying to load the standard
    // asset.
    if (shouldCheckAlt) {
        std::string altPath = IResource::gAltAssetPrefix;
        altPath += identifier.Path;
        auto altResource =
            LoadResourceProcess({ std::move(altPath), identifier.Owner, identifier.Parent }, loadExact, initData);

        if (altResource != nullptr) {
            return altResource;
        }
    }

    // While waiting in the queue, another thread could have loaded the resource.
    // In a last attempt to avoid doing work that will be discarded, let's check if the cached version exists.
    auto cacheLine = CheckCache(identifier, loadExact);
    auto cachedResource = GetCachedResource(cacheLine);
    if (cachedResource != nullptr) {
        return cachedResource;
    }

    // Check for resource load errors which can indicate an alternate asset.
    // If we are attempting to load an alternate asset, we can return null
    if (!loadExact && mAltAssetsEnabled && isAltPath) {
        if (std::holds_alternative<ResourceLoadError>(cacheLine)) {
            try {
                // If we have attempted to cache an alternate asset, but failed, we return nullptr and rely on the
                // calling function to return a regular asset. If we have NOT attempted load already, attempt the load.
                auto loadError = std::get<ResourceLoadError>(cacheLine);
                if (loadError != ResourceLoadError::NotCached) {
                    return nullptr;
                }
            } catch (std::bad_variant_access const& e) {
                // Ignore the exception. This should never happen. The last check should've returned the resource.
            }
        }
    }

    // Get the file from the OTR
    auto file = LoadFileProcess(identifier.Path);
    if (file == nullptr) {
        SPDLOG_TRACE("Failed to load resource file at path {}", identifier.Path);
        mResourceCache[identifier] = ResourceLoadError::NotFound;
        return nullptr;
    }

    // Transform the raw data into a resource
    auto resource = GetResourceLoader()->LoadResource(identifier.Path, file, initData);

    // Another thread could have loaded the resource while we were processing, so we want to check before setting to
    // the cache.
    cachedResource = GetCachedResource(identifier, true);

    {
#ifndef __vita__
        const std::lock_guard<std::mutex> lock(mMutex);
#endif

        if (cachedResource != nullptr) {
            // If another thread has already loaded this resource, discard the work we already did and return from
            // cache.
            resource = cachedResource;
        }

        // Set the cache to the loaded resource
        if (resource != nullptr) {
            mResourceCache[identifier] = resource;
        } else {
            mResourceCache[identifier] = ResourceLoadError::NotFound;
        }
    }

    if (resource != nullptr) {
        SPDLOG_TRACE("Loaded Resource {} on ResourceManager", identifier.Path);
    } else {
        SPDLOG_TRACE("Resource load FAILED {} on ResourceManager", identifier.Path);
    }

    return resource;
}

std::shared_ptr<IResource> ResourceManager::LoadResourceProcess(const std::string& filePath, bool loadExact,
                                                                std::shared_ptr<ResourceInitData> initData) {
    return LoadResourceProcess({ filePath, mDefaultCacheOwner, mDefaultCacheArchive }, loadExact, initData);
}

std::shared_future<std::shared_ptr<IResource>>
ResourceManager::LoadResourceAsync(const ResourceIdentifier& identifier, bool loadExact, BS::priority_t priority,
                                   std::shared_ptr<ResourceInitData> initData) {
    // Check for and remove the OTR signature
    if (OtrSignatureCheck(identifier.Path.c_str())) {
        auto newFilePath = identifier.Path.substr(7);
        return LoadResourceAsync({ newFilePath, identifier.Owner, identifier.Parent }, loadExact, priority);
    }

    // Check the cache before queueing the job.
    auto cacheCheck = GetCachedResource(identifier, loadExact);
    if (cacheCheck) {
        auto promise = std::make_shared<std::promise<std::shared_ptr<IResource>>>();
        promise->set_value(cacheCheck);
        auto future = promise->get_future().share();
#ifdef __vita__
        sVitaLeakedResourcePromises.push_back(promise);
#endif
        return future;
    }

#ifdef __vita__
    // Rinnegatamante (author of the Vita-patched rendering layer this port's
    // libultraship fork is merged from) removed ResourceManager's thread
    // pool entirely as one of several Vita-specific optimizations. This
    // session's own real-hardware testing independently converged on the
    // same conclusion from the other direction: a psp2dmp coredump showed
    // the main thread genuinely stalled with worker-pool threads "Running"
    // concurrently inside real file I/O (sceIoWrite/sceIoLseek32) - dropping
    // the pool to a single worker thread didn't resolve it either. Load
    // synchronously on the calling thread instead - still wrapped in the
    // same shared_future the caller already expects, so nothing downstream
    // needs to change.
    auto promise = std::make_shared<std::promise<std::shared_ptr<IResource>>>();
    promise->set_value(LoadResourceProcess(identifier, loadExact, initData));
    auto future = promise->get_future().share();
    sVitaLeakedResourcePromises.push_back(promise);
    return future;
#else
    return mThreadPool->submit_task(
        [this, identifier, loadExact, initData]() -> std::shared_ptr<IResource> {
            return LoadResourceProcess(identifier, loadExact, initData);
        },
        priority);
#endif
}

std::shared_future<std::shared_ptr<IResource>>
ResourceManager::LoadResourceAsync(const std::string& filePath, bool loadExact, BS::priority_t priority,
                                   std::shared_ptr<ResourceInitData> initData) {
    return LoadResourceAsync({ filePath, mDefaultCacheOwner, mDefaultCacheArchive }, loadExact, priority, initData);
}

std::shared_ptr<IResource> ResourceManager::LoadResource(const ResourceIdentifier& identifier, bool loadExact,
                                                         std::shared_ptr<ResourceInitData> initData) {
    auto resource = LoadResourceAsync(identifier, loadExact, BS::pr::highest, initData).get();
    if (resource == nullptr) {
        SPDLOG_TRACE("Failed to load resource file at path {}", identifier.Path);
    }
    return resource;
}

std::shared_ptr<IResource> ResourceManager::LoadResource(const std::string& filePath, bool loadExact,
                                                         std::shared_ptr<ResourceInitData> initData) {
    return LoadResource({ filePath, mDefaultCacheOwner, mDefaultCacheArchive }, loadExact, initData);
}

std::shared_ptr<IResource> ResourceManager::LoadResource(uint64_t crc, bool loadExact,
                                                         std::shared_ptr<ResourceInitData> initData) {
    const std::string* hashStr = GetArchiveManager()->HashToString(crc);
    if (hashStr == nullptr || hashStr->length() == 0) {
        SPDLOG_TRACE("ResourceLoad: Unknown crc {}\n", crc);
        return nullptr;
    }

    return LoadResource(*hashStr, loadExact, initData);
}

std::variant<ResourceManager::ResourceLoadError, std::shared_ptr<IResource>>
ResourceManager::CheckCache(const ResourceIdentifier& identifier, bool loadExact) {
    if (!loadExact && mAltAssetsEnabled && !identifier.Path.starts_with(IResource::gAltAssetPrefix)) {
        const auto altPath = IResource::gAltAssetPrefix + identifier.Path;
        auto altCacheResult = CheckCache({ altPath, identifier.Owner, identifier.Parent }, loadExact);

        // If the type held at this cache index is a resource, then we return it.
        // Else we attempt to load standard definition assets.
        if (std::holds_alternative<std::shared_ptr<IResource>>(altCacheResult)) {
            return altCacheResult;
        }
    }

#ifndef __vita__
    const std::lock_guard<std::mutex> lock(mMutex);
#endif

    auto cacheFind = mResourceCache.find(identifier);
    if (cacheFind == mResourceCache.end()) {
        return ResourceLoadError::NotCached;
    }

    return cacheFind->second;
}

std::variant<ResourceManager::ResourceLoadError, std::shared_ptr<IResource>>
ResourceManager::CheckCache(const std::string& filePath, bool loadExact) {
    return CheckCache({ filePath, mDefaultCacheOwner, mDefaultCacheArchive }, loadExact);
}

std::shared_ptr<IResource> ResourceManager::GetCachedResource(const ResourceIdentifier& identifier, bool loadExact) {
    // Gets the cached resource based on filePath.
    return GetCachedResource(CheckCache(identifier, loadExact));
}

std::shared_ptr<IResource> ResourceManager::GetCachedResource(const std::string& filePath, bool loadExact) {
    // Gets the cached resource based on filePath.
    return GetCachedResource({ filePath, mDefaultCacheOwner, mDefaultCacheArchive }, loadExact);
}

std::shared_ptr<IResource>
ResourceManager::GetCachedResource(std::variant<ResourceLoadError, std::shared_ptr<IResource>> cacheLine) {
    // Gets the cached resource based on a cache line std::variant from the cache map.
    if (std::holds_alternative<std::shared_ptr<IResource>>(cacheLine)) {
        try {
            auto resource = std::get<std::shared_ptr<IResource>>(cacheLine);

            if (resource.use_count() <= 0) {
                return nullptr;
            }

            if (resource->IsDirty()) {
                return nullptr;
            }

            return resource;
        } catch (std::bad_variant_access const& e) {
            // Ignore the exception
        }
    }

    return nullptr;
}

std::shared_ptr<std::vector<std::shared_ptr<IResource>>>
ResourceManager::LoadResourcesProcess(const ResourceFilter& filter) {
    auto loadedList = std::make_shared<std::vector<std::shared_ptr<IResource>>>();
    auto fileList = GetArchiveManager()->ListFiles(filter.IncludeMasks, filter.ExcludeMasks);
    loadedList->reserve(fileList->size());

    for (size_t i = 0; i < fileList->size(); i++) {
        auto fileName = std::string(fileList->operator[](i));
        auto resource = LoadResource({ fileName, filter.Owner, filter.Parent });
        loadedList->push_back(resource);
    }

    return loadedList;
}

std::shared_future<std::shared_ptr<std::vector<std::shared_ptr<IResource>>>>
ResourceManager::LoadResourcesAsync(const ResourceFilter& filter, BS::priority_t priority) {
#ifdef __vita__
    // See LoadResourceAsync's comment above - no thread pool on Vita.
    auto promise = std::make_shared<std::promise<std::shared_ptr<std::vector<std::shared_ptr<IResource>>>>>();
    promise->set_value(LoadResourcesProcess(filter));
    auto future = promise->get_future().share();
    sVitaLeakedResourcePromises.push_back(promise);
    return future;
#else
    return mThreadPool->submit_task(
        [this, filter]() -> std::shared_ptr<std::vector<std::shared_ptr<IResource>>> {
            return LoadResourcesProcess(filter);
        },
        priority);
#endif
}

std::shared_future<std::shared_ptr<std::vector<std::shared_ptr<IResource>>>>
ResourceManager::LoadResourcesAsync(const std::string& searchMask, BS::priority_t priority) {
    return LoadResourcesAsync({ { searchMask }, {}, mDefaultCacheOwner, mDefaultCacheArchive }, priority);
}

std::shared_ptr<std::vector<std::shared_ptr<IResource>>> ResourceManager::LoadResources(const std::string& searchMask) {
    return LoadResources({ { searchMask }, {}, mDefaultCacheOwner, mDefaultCacheArchive });
}

std::shared_ptr<std::vector<std::shared_ptr<IResource>>> ResourceManager::LoadResources(const ResourceFilter& filter) {
    return LoadResourcesAsync(filter, BS::pr::highest).get();
}

void ResourceManager::DirtyResources(const ResourceFilter& filter) {
    auto doDirty = [this, filter]() -> void {
        auto list = GetArchiveManager()->ListFiles(filter.IncludeMasks, filter.ExcludeMasks);

        for (const auto& key : *list.get()) {
            auto resource = GetCachedResource({ key, filter.Owner, filter.Parent });
            // If it's a resource, we will set the dirty flag, else we will just unload it.
            if (resource != nullptr) {
                resource->Dirty();
            } else {
                UnloadResource({ key, filter.Owner, filter.Parent });
            }
        }
    };
#ifdef __vita__
    // See LoadResourceAsync's comment above - no thread pool on Vita.
    doDirty();
#else
    mThreadPool->submit_task(doDirty);
#endif
}

void ResourceManager::DirtyResources(const std::string& searchMask) {
    DirtyResources({ { searchMask }, {}, mDefaultCacheOwner, mDefaultCacheArchive });
}

void ResourceManager::UnloadResourcesAsync(const std::string& searchMask, BS::priority_t priority) {
    UnloadResourcesAsync({ { searchMask }, {}, mDefaultCacheOwner, mDefaultCacheArchive }, priority);
}

void ResourceManager::UnloadResourcesAsync(const ResourceFilter& filter, BS::priority_t priority) {
#ifdef __vita__
    // See LoadResourceAsync's comment above - no thread pool on Vita.
    UnloadResourcesProcess(filter);
#else
    mThreadPool->submit_task([this, filter]() -> void { UnloadResourcesProcess(filter); }, priority);
#endif
}

void ResourceManager::UnloadResources(const std::string& searchMask) {
    UnloadResources({ { searchMask }, {}, mDefaultCacheOwner, mDefaultCacheArchive });
}

void ResourceManager::UnloadResources(const ResourceFilter& filter) {
    UnloadResourcesProcess(filter);
}

void ResourceManager::UnloadResourcesProcess(const ResourceFilter& filter) {
    auto list = GetArchiveManager()->ListFiles(filter.IncludeMasks, filter.ExcludeMasks);

    for (const auto& key : *list.get()) {
        UnloadResource({ key, mDefaultCacheOwner, mDefaultCacheArchive });
    }
}

std::shared_ptr<ArchiveManager> ResourceManager::GetArchiveManager() {
    return mArchiveManager;
}

std::shared_ptr<ResourceLoader> ResourceManager::GetResourceLoader() {
    return mResourceLoader;
}

size_t ResourceManager::UnloadResource(const ResourceIdentifier& identifier) {
    // Store a shared pointer here so that erase doesn't destruct the resource.
    // The resource will attempt to load other resources on the destructor, and this will fail because we already hold
    // the mutex.
    std::variant<ResourceLoadError, std::shared_ptr<IResource>> value = nullptr;
    size_t ret = 0;
    // We can only erase the resource if we have any resources for that owner.
    if (mResourceCache.contains(identifier)) {
#ifndef __vita__
        const std::lock_guard<std::mutex> lock(mMutex);
#endif
        mResourceCache.erase(identifier);
    }

    return ret;
}

size_t ResourceManager::UnloadResource(const std::string& filePath) {
    return UnloadResource({ filePath, mDefaultCacheOwner, mDefaultCacheArchive });
}

bool ResourceManager::WriteResource(const ResourceIdentifier& identifier, const std::vector<uint8_t>& data,
                                    bool unloadFile) {
    std::shared_ptr<Archive> archive = identifier.Parent;

    if (!archive) {
        archive = mArchiveManager->GetArchiveFromFile(identifier.Path);
    }

    if (!archive) {
        return false;
    }

    if (!mArchiveManager->WriteFile(archive, identifier.Path, data)) {
        return false;
    }

    if (unloadFile) {
        UnloadResource(identifier);
    }

    return true;
}

bool ResourceManager::OtrSignatureCheck(const char* fileName) {
    static const char* sOtrSignature = "__OTR__";
    return strncmp(fileName, sOtrSignature, strlen(sOtrSignature)) == 0;
}

bool ResourceManager::IsAltAssetsEnabled() {
    return mAltAssetsEnabled;
}

void ResourceManager::SetAltAssetsEnabled(bool isEnabled) {
    mAltAssetsEnabled = isEnabled;
}

size_t ResourceManager::GetResourceSize(std::shared_ptr<IResource> resource) {
    if (resource == nullptr) {
        return 0;
    }

    return resource->GetPointerSize();
}

size_t ResourceManager::GetResourceSize(const char* name) {
    auto resource = LoadResource(name);

    return GetResourceSize(resource);
}

size_t ResourceManager::GetResourceSize(uint64_t crc) {
    auto resource = LoadResource(crc);

    return GetResourceSize(resource);
}

bool ResourceManager::GetResourceIsCustom(std::shared_ptr<IResource> resource) {
    if (resource == nullptr) {
        return false;
    }

    return resource->GetInitData()->IsCustom;
}

bool ResourceManager::GetResourceIsCustom(const char* name) {
    auto resource = LoadResource(name);

    return GetResourceIsCustom(resource);
}

bool ResourceManager::GetResourceIsCustom(uint64_t crc) {
    auto resource = LoadResource(crc);

    return GetResourceIsCustom(resource);
}

void* ResourceManager::GetResourceRawPointer(std::shared_ptr<IResource> resource) {
    if (resource == nullptr) {
        return nullptr;
    }

    return resource->GetRawPointer();
}

void* ResourceManager::GetResourceRawPointer(const char* name) {
    auto resource = LoadResource(name);

    return GetResourceRawPointer(resource);
}

void* ResourceManager::GetResourceRawPointer(uint64_t crc) {
    auto resource = LoadResource(crc);

    return GetResourceRawPointer(resource);
}

} // namespace Ship
