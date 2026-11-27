// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Rmx1901WindowPublisher: publish the SHNGROOT OS/SystemOS windows of the
// flashed userdata image as read-only IOMedia, so XNU's /chosen/root-matching
// (IOProviderClass=IOMedia, UUID=<root volume>) can select the root volume.
//
// The transport is the device-qualified BSD UFS driver in
// patches/xnu/sweet-ufs-root.patch. This file never touches the UFS registers:
// it consumes two exported C primitives from that driver and uses
// sweet_ufs_rootdev() to make sure the controller is ready first:
//
//   int  sweet_ufs_rootdev(dev_t *dev);
//   int  rmx1901_ufs_read(uint64_t lba, uint32_t count, void *out);
//   void rmx1901_ufs_geometry(uint64_t *first, uint64_t *nblocks,
//                             uint32_t *block_bytes);
//
// Both read/geometry calls use 4096-byte device blocks on LUN 0. SHNGROOT
// window offsets are byte offsets from the start of userdata; each window is
// itself a 512-byte-sector disk containing an APFS container, so each window
// is exposed as a 512-byte IOBlockStorageDevice (the layout APFS expects).
//
// Start is gated by `rmx1901-root-windows=1`. The unverified-root hooks are
// separately gated by `rmx1901-allow-unverified=1`. With the first argument
// off, the personality does not match and nothing is registered.

#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <sys/param.h>
#include <sys/types.h>
#include <string.h>

#include <libkern/crypto/sha2.h>

extern "C" int sweet_ufs_rootdev(dev_t *dev);
extern "C" int rmx1901_ufs_read(uint64_t lba, uint32_t count, void *out);
extern "C" void rmx1901_ufs_geometry(uint64_t *first, uint64_t *nblocks,
    uint32_t *block_bytes);
extern "C" volatile uint32_t native_ufs_blocks_published;

namespace {
constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kBlockBytes = 4096;
constexpr uint32_t kTransferSectors = 128;  // 64 KiB per I/O
constexpr uint32_t kTransferBytes = kTransferSectors * kSectorBytes;
constexpr uint64_t kManifestBytes = 4096;
constexpr uint64_t kDigestOffset = 4064;
constexpr uint64_t kHeaderBytes = 24;
constexpr uint64_t kEntryBytes = 128;
constexpr unsigned kMaxWindows = 4;

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
  return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

struct Window {
  char name[32];
  uint64_t firstSector;   // absolute 512-byte sector, relative to LUN 0
  uint64_t sectorCount;
};

bool allowUnverifiedWindow() {
  unsigned allow = 0;
  PE_parse_boot_argn("rmx1901-allow-unverified", &allow, sizeof(allow));
  return allow != 0;
}
}  // namespace

class Rmx1901WindowBlock;

// Lightweight board-started service. It owns the transfer buffer and parses
// the manifest, then publishes one read-only block device per SHNGROOT window.
class Rmx1901WindowPublisher : public IOService {
  OSDeclareDefaultStructors(Rmx1901WindowPublisher)

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  void free() override;

  bool readWindow(uint64_t sector, uint32_t count, IOMemoryDescriptor *buffer,
      uint64_t offset);
  void publishWindows();

 private:
  bool readManifest();

  IOBufferMemoryDescriptor *transfer = nullptr;
  IOLock *gate = nullptr;
  uint8_t *bytes = nullptr;
  uint64_t userdataFirstBlock = 0;  // in 4096-byte device blocks
  uint64_t userdataBlocks = 0;
  Window windows[kMaxWindows] = {};
  uint32_t windowCount = 0;
};

// One read-only APFS window, exposed as a 512-byte IOBlockStorageDevice.
class Rmx1901WindowBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(Rmx1901WindowBlock)
  Rmx1901WindowPublisher *parent = nullptr;
  uint64_t firstSector = 0;
  uint64_t sectorCount = 0;
  char label[32] = {};

 public:
  bool configure(Rmx1901WindowPublisher *owner, uint64_t first, uint64_t count,
      const char *name) {
    if (!owner || !count || !name || first > UINT64_MAX - count) return false;
    parent = owner;
    firstSector = first;
    sectorCount = count;
    strlcpy(label, name, sizeof(label));
    setProperty("SHNGROOTWindow", label);
    return true;
  }
  bool start(IOService *provider) override {
    if (!parent || !IOBlockStorageDevice::start(provider)) return false;
    registerService();
    __atomic_fetch_add(&native_ufs_blocks_published, 1, __ATOMIC_RELAXED);
    IOLog("Rmx1901Window: published %s (%llu sectors at 0x%llx)\n", label,
        (unsigned long long)sectorCount, (unsigned long long)firstSector);
    return true;
  }
  void stop(IOService *provider) override { IOBlockStorageDevice::stop(provider); }
  void free() override { parent = nullptr; IOBlockStorageDevice::free(); }

  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("UniXNU"); }
  char *getProductString() override { return const_cast<char *>(label); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override {
    return const_cast<char *>("SHNGROOT window, UFS");
  }
  IOReturn reportBlockSize(UInt64 *value) override {
    *value = kSectorBytes;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability(bool *value) override {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock(UInt64 *value) override {
    *value = sectorCount ? sectorCount - 1 : 0;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = true;
    if (changed) *changed = false;
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportWriteProtection(bool *value) override {
    *value = true;
    return kIOReturnSuccess;
  }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override {
    return kIOReturnSuccess;
  }
  IOReturn getWriteCacheState(bool *value) override {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }

  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
      UInt64 count, IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!buffer || !completion || !completion->action || !count ||
        block >= sectorCount || count > sectorCount - block ||
        count > UINT64_MAX / kSectorBytes ||
        buffer->getLength() < count * kSectorBytes) {
      return kIOReturnBadArgument;
    }
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    uint64_t done = 0;
    IOReturn result = kIOReturnSuccess;
    while (done < count) {
      uint32_t chunk = count - done < kTransferSectors
          ? static_cast<uint32_t>(count - done) : kTransferSectors;
      if (!parent->readWindow(firstSector + block + done, chunk, buffer,
              done * kSectorBytes)) {
        result = kIOReturnIOError;
        break;
      }
      done += chunk;
    }
    completion->action(completion->target, completion->parameter, result,
        done * kSectorBytes);
    return kIOReturnSuccess;
  }
};

OSDefineMetaClassAndStructors(Rmx1901WindowPublisher, IOService)
OSDefineMetaClassAndStructors(Rmx1901WindowBlock, IOBlockStorageDevice)

IOService *Rmx1901WindowPublisher::probe(IOService *provider, SInt32 *score) {
  unsigned enable = 0;
  if (!PE_parse_boot_argn("rmx1901-root-windows", &enable, sizeof(enable)) ||
      enable != 1) {
    return nullptr;
  }
  // The personality matches every IOPlatformExpertDevice candidate. Only the
  // first one should own the fixed UFS transport.
  static volatile int claimed = 0;
  if (__atomic_exchange_n(&claimed, 1, __ATOMIC_SEQ_CST)) return nullptr;
  return IOService::probe(provider, score);
}

bool Rmx1901WindowPublisher::readManifest() {
  if (!bytes || !userdataFirstBlock) return false;
  IOLockLock(gate);
  int error = rmx1901_ufs_read(userdataFirstBlock, 1, bytes);
  IOLockUnlock(gate);
  if (error) return false;
  const uint8_t *raw = bytes;

  if (memcmp(raw, "SHNGROOT", 8)) return false;
  uint32_t version = le32(raw + 8);
  uint32_t count = le32(raw + 12);
  if (!((version == 1 && count >= 1 && count <= kMaxWindows) ||
        (version == 2 && count >= 1 && count <= 8))) {
    return false;
  }
  SHA256_CTX context;
  uint8_t digest[32];
  SHA256_Init(&context);
  SHA256_Update(&context, raw, static_cast<size_t>(kDigestOffset));
  SHA256_Final(digest, &context);
  if (memcmp(digest, raw + kDigestOffset, sizeof(digest))) return false;

  const uint64_t limit = userdataBlocks * kBlockBytes;
  for (uint32_t i = 0; i < count && windowCount < kMaxWindows; i++) {
    const uint8_t *entry = raw + kHeaderBytes + kEntryBytes * i;
    uint64_t offset = le64(entry + 32);
    uint64_t size = le64(entry + 40);
    uint32_t blockSize = le32(entry + 48);
    uint32_t flags = le32(entry + 52);
    if (blockSize != kSectorBytes || flags != 1) continue;
    if (!size || (offset & (kSectorBytes - 1)) ||
        (size & (kSectorBytes - 1)) || offset < kManifestBytes ||
        offset >= limit || size > limit - offset) {
      return false;
    }
    unsigned length = 0;
    while (length < sizeof(windows[0].name) - 1 &&
        entry[length] >= 0x20 && entry[length] <= 0x7e) {
      length++;
    }
    if (!length || entry[length] != 0) return false;

    Window &w = windows[windowCount];
    bzero(&w, sizeof(w));
    strlcpy(w.name, reinterpret_cast<const char *>(entry), sizeof(w.name));
    w.firstSector = userdataFirstBlock * (kBlockBytes / kSectorBytes) +
        offset / kSectorBytes;
    w.sectorCount = size / kSectorBytes;
    windowCount++;
  }
  if (!windowCount) return false;
  IOLog("Rmx1901Window: SHNGROOT v%u with %u window(s)\n", version, count);
  return true;
}

static void rmx1901_publish_windows(void *argument, wait_result_t) {
  auto *owner = static_cast<Rmx1901WindowPublisher *>(argument);
  owner->publishWindows();
  owner->release();
}

bool Rmx1901WindowPublisher::start(IOService *provider) {
  if (!IOService::start(provider)) return false;

  // The IOKit personality can start before the BSD root probe. Bring the UFS
  // transport up here; sweet_ufs_rootdev() is idempotent and the later
  // vfs_mountroot hook reuses the same state.
  dev_t ignored = NODEV;
  if (sweet_ufs_rootdev(&ignored)) return false;

  uint64_t first = 0, nblocks = 0;
  uint32_t blockBytes = 0;
  rmx1901_ufs_geometry(&first, &nblocks, &blockBytes);
  if (!first || !nblocks || blockBytes != kBlockBytes) return false;
  userdataFirstBlock = first;
  userdataBlocks = nblocks;

  transfer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
      static_cast<IOOptionBits>(kIODirectionIn), kTransferBytes + kBlockBytes,
      0x1000);
  gate = IOLockAlloc();
  if (!transfer || !gate) return false;
  bytes = static_cast<uint8_t *>(transfer->getBytesNoCopy());
  if (!bytes || !readManifest()) return false;

  registerService();
  // Child start() is deferred to a kernel thread, the same ownership order
  // NativeDwMmc uses: APFS sees the windows only after this service is live.
  retain();
  thread_t thread = nullptr;
  if (kernel_thread_start(rmx1901_publish_windows, this, &thread) ==
      KERN_SUCCESS) {
    thread_deallocate(thread);
  } else {
    release();
  }
  return true;
}

void Rmx1901WindowPublisher::publishWindows() {
  static const char *kOrder[] = {"SystemOS", "OS"};
  for (unsigned pass = 0; pass < 2; pass++) {
    for (uint32_t i = 0; i < windowCount; i++) {
      if (strcmp(windows[i].name, kOrder[pass]) != 0) continue;
      auto *window = OSTypeAlloc(Rmx1901WindowBlock);
      if (!window) continue;
      if (!window->init(nullptr) ||
          !window->configure(this, windows[i].firstSector,
              windows[i].sectorCount, windows[i].name) ||
          !window->attach(this) || !window->start(this)) {
        window->release();
      }
    }
  }
}

bool Rmx1901WindowPublisher::readWindow(uint64_t sector, uint32_t count,
    IOMemoryDescriptor *buffer, uint64_t offset) {
  if (!count || !buffer || !bytes || !gate) return false;
  const uint64_t bytePos = sector * kSectorBytes;
  const uint32_t skip = (uint32_t)(bytePos % kBlockBytes);
  const uint64_t devBlock = bytePos / kBlockBytes;
  const uint64_t totalBytes = (uint64_t)count * kSectorBytes;
  if (skip % kSectorBytes) return false;

  const uint32_t blocks = (uint32_t)((skip + totalBytes + kBlockBytes - 1) /
      kBlockBytes);
  if (!blocks || blocks * kBlockBytes > kTransferBytes + kBlockBytes) {
    return false;
  }

  IOLockLock(gate);
  int error = rmx1901_ufs_read(devBlock, blocks, bytes);
  ssize_t written = error ? -1 : buffer->writeBytes(offset, bytes + skip,
      totalBytes);
  IOLockUnlock(gate);
  return written == (ssize_t)totalBytes;
}

void Rmx1901WindowPublisher::stop(IOService *provider) {
  IOService::stop(provider);
}

void Rmx1901WindowPublisher::free() {
  if (transfer) {
    transfer->release();
    transfer = nullptr;
  }
  if (gate) {
    IOLockFree(gate);
    gate = nullptr;
  }
  bytes = nullptr;
  IOService::free();
}

// The unverified-root bring-up path. The signed OS/SystemOS policies are not
// built for this board; these three hooks answer XNU's block-device questions
// from the two read-only SHNGROOT windows. The boot argument is deliberately
// explicit: without it every hook refuses. The implementation follows the
// gtaxllte NativeDwMmc hooks, including per-device (not global) matching.
namespace {

bool mediaHasWindow(IOService *media, Rmx1901WindowBlock **window = nullptr) {
  unsigned depth = 0;
  for (IOService *cursor = media; cursor && depth++ < 64;
       cursor = cursor->getProvider()) {
    Rmx1901WindowBlock *candidate = OSDynamicCast(Rmx1901WindowBlock, cursor);
    if (candidate) {
      if (window) *window = candidate;
      return true;
    }
  }
  return false;
}

IOService *copySingleMedia(OSDictionary *matching) {
  OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
  if (!iterator) return nullptr;
  IOService *found = nullptr;
  unsigned matches = 0;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) {
      found = nullptr;
      break;
    }
    found = media;
    found->retain();
  }
  iterator->release();
  return found;
}

IOService *copyMediaForDevice(dev_t device) {
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(2);
  OSNumber *mjr = OSNumber::withNumber(static_cast<unsigned>(major(device)), 32);
  OSNumber *mnr = OSNumber::withNumber(static_cast<unsigned>(minor(device)), 32);
  if (!matching || !properties || !mjr || !mnr ||
      !properties->setObject(kIOBSDMajorKey, mjr) ||
      !properties->setObject(kIOBSDMinorKey, mnr) ||
      !matching->setObject("IOPropertyMatch", properties)) {
    OSSafeReleaseNULL(mnr);
    OSSafeReleaseNULL(mjr);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(matching);
    return nullptr;
  }
  mjr->release();
  mnr->release();
  properties->release();
  IOService *media = copySingleMedia(matching);
  matching->release();
  return media;
}

IOService *copyMediaForUUID(const char *uuid) {
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  if (!matching || !properties || !wanted ||
      !properties->setObject("UUID", wanted) ||
      !matching->setObject("IOPropertyMatch", properties)) {
    OSSafeReleaseNULL(wanted);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(matching);
    return nullptr;
  }
  wanted->release();
  properties->release();
  IOService *media = copySingleMedia(matching);
  matching->release();
  return media;
}

}  // namespace

extern "C" int native_verified_block_root_authorized(dev_t device) {
  if (!allowUnverifiedWindow() || device == NODEV) return 0;
  IOService *media = copyMediaForDevice(device);
  bool authorized = media && mediaHasWindow(media);
  if (media) media->release();
  return authorized ? 1 : 0;
}

extern "C" int native_verified_block_volume_device(const char *uuid,
    dev_t *device, char *path, size_t size) {
  if (!allowUnverifiedWindow() || !uuid || strlen(uuid) != 36 || !device ||
      !path || size < 6) {
    return 0;
  }
  *device = NODEV;
  path[0] = 0;
  for (unsigned attempt = 0; attempt < 50; attempt++) {
    IOService *media = copyMediaForUUID(uuid);
    if (media && mediaHasWindow(media)) {
      OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
      OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
      OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
      if (name && mjr && mnr && name->getLength() &&
          name->getLength() < size - 6 &&
          mjr->unsigned64BitValue() <= 255 &&
          mnr->unsigned64BitValue() <= 0xffffff) {
        *device = static_cast<dev_t>(
            makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
        snprintf(path, size, "/dev/%s", name->getCStringNoCopy());
        media->release();
        return 1;
      }
    }
    if (media) media->release();
    IOSleep(100);
  }
  return 0;
}

extern "C" int native_verified_block_manifest_sha384(dev_t device,
    const char *uuid, uint8_t digest[48]) {
  if (!allowUnverifiedWindow() || !digest || !uuid || strlen(uuid) != 36) {
    return 0;
  }
  IOService *media = copyMediaForDevice(device);
  bool authorized = media && mediaHasWindow(media);
  if (media) media->release();
  if (!authorized) return 0;
  static const char domain[] = "rmx1901.unverified.window.v1";
  SHA384_CTX context;
  SHA384_Init(&context);
  SHA384_Update(&context, domain, sizeof(domain));
  SHA384_Update(&context, uuid, 36);
  SHA384_Final(digest, &context);
  return 1;
}
