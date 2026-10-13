// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// itlmt7925WifiInterface, stage 2 (see itlmt7925_iface.h and docs/IO80211-ABI-2.md "Native stub, stage 2").
//
// Order of AppleBCMWLANSkywalkInterface::start(core, RegistrationInfo*) (0xffffff800156a576), which this follows:
//   IO80211InfraInterface::start(core) -> work queue of the interface -> (bus layer creates pool + queues) -> every Tx
//   submission queue, then the Tx completion and Rx completion queue go on the work queue as event sources -> headroom
//   (info+0x3c) is subtracted from info+0x48 / +0x4c -> registerInfraEthernetInterface(info, [Tx x n, TxC, RxC], n+2, pool, pool)
//   (result 0 = success). The pool and the queues are created here directly with the IOSkywalk factories.
#include "itlmt7925_iface.h"
#include "ether_checksum.h"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <stdarg.h>
#include <string.h>
#include <pexpert/pexpert.h>

#define super IO80211InfraProtocol
OSDefineMetaClassAndStructors(itlmt7925WifiInterface, IO80211InfraProtocol)

namespace {

// Sizes Apple's PCIe layer uses with its state words at 0 (ABI-2 section 2): TxSubmission/TxCompletion `a`, RxSubmission/
// RxCompletion `c`, pool `d` packets of `e` bytes.
const uint32_t kTxCapacity = 0x200;
const uint32_t kRxCapacity = 0x400;
const uint32_t kPoolPackets = 0x1300;
const uint32_t kPoolBufferSize = 0x800;

// Dequeue / completion callbacks. What a callback must do with the packets it is handed (ownership, completion) is NOT
// determined (stage 2 has no radio and never enables the queues); they take nothing and report no work.
uint32_t txSubmissionAction(OSObject *owner, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *packets, uint32_t count, void *)
{ return ((itlmt7925WifiInterface *)owner)->submitPackets(packets, count); }
uint32_t rxSubmissionAction(OSObject *, IOSkywalkRxSubmissionQueue *, IOSkywalkPacket *const *, uint32_t, void *) { return 0; }
uint32_t txCompletionAction(OSObject *, IOSkywalkTxCompletionQueue *, IOSkywalkPacket **, uint32_t, void *) { return 0; }
uint32_t rxCompletionAction(OSObject *, IOSkywalkRxCompletionQueue *, IOSkywalkPacket **, uint32_t, void *) { return 0; }

// RegistrationInfo is an opaque 0x130-byte block (ABI-2 section 2): only the words the registration path edits are touched.
template <typename T>
inline T &field(IOSkywalkEthernetInterface::RegistrationInfo *info, size_t off)
{
    return *(T *)((uint8_t *)info + off);
}

inline void rel(OSObject *o)
{
    if (o) o->release();
}

}

void itlmtTrace(IORegistryEntry *e, const char *fmt, ...)
{
    if (!e) return;
    char add[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(add, sizeof(add), fmt, ap);
    va_end(ap);
    IOLog("itlmt-native: %s\n", add); // also emit every milestone to the kernel log before registry work
    e->setProperty("itlmt_native_last", add);
    char buf[1536];
    buf[0] = 0;
    OSObject *snapshot = e->copyProperty("itlmt_native_trace");
    if (OSString *old = OSDynamicCast(OSString, snapshot))
        strlcpy(buf, old->getCStringNoCopy(), sizeof(buf));
    if (snapshot) snapshot->release();
    if (buf[0]) strlcat(buf, "; ", sizeof(buf));
    bool full = strlcat(buf, add, 1500) >= 1500;
    if (full) {
        // Preserve recent diagnostic entries after the startup prefix fills up.
        e->setProperty("itlmt_native_trace_tail", add);
    }
    if (!full) e->setProperty("itlmt_native_trace", buf);
    uint32_t nvtrace = 0;
    if (PE_parse_boot_argn("itlmt_native_nvtrace", &nvtrace, sizeof(nvtrace)) && nvtrace) {
        // Low-frequency lifecycle checkpoints only, no packet bytes or network identifiers.
        // Best effort: a panic before NVRAM flush can still lose the latest checkpoint.
        IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (options) {
            OSData *data = OSData::withBytes(full ? add : buf, (unsigned)strlen(full ? add : buf));
            if (data) {
                const char *key = full ? "itlmt-native-last" : "itlmt-native-trace";
                options->setProperty(key, data);
                data->release();
                options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", key);
            }
            options->release();
        }
    }
}

itlmt7925WifiInterface *itlmt7925WifiInterface::withController(IO80211Controller *controller, uint32_t ifId, uint32_t role)
{
    // Same steps as AppleBCMWLANSkywalkInterface::initWithProviderAndParameters: init(), setInterfaceRole, setInterfaceId, attach
    itlmt7925WifiInterface *i = OSTypeAlloc(itlmt7925WifiInterface);
    if (!i) return nullptr;
    if (!i->init()) {
        i->release();
        return nullptr;
    }
    i->setInterfaceRole(role);
    i->setInterfaceId(ifId);
    i->ctl = controller;
    if (!i->attach(controller)) {
        i->release();
        return nullptr;
    }
    return i;
}

bool itlmt7925WifiInterface::init()
{
    if (!super::init()) return false;
    rxLock = IOLockAlloc();
    return rxLock != nullptr;
}

bool itlmt7925WifiInterface::createPoolAndQueues()
{
    IOSkywalkPacketBufferPool::PoolOptions po;
    bzero(&po, sizeof(po));
    po.packets0 = po.packets1 = kPoolPackets;
    po.bufferSize = kPoolBufferSize;
    po.one = 1;
    po.zero = 0;
    po.flags = 1;
    pool = IOSkywalkPacketBufferPool::withName("itlmt7925WifiPool", this, 1, &po);
    itlmtTrace(ctl, "pool=%s", pool ? "ok" : "null");
    if (!pool) return false;

    // Apple creates the common Rx submission queue before any interface (queueId 0, capacity c); where it is registered is not
    // visible in the interface's start (only Tx x n, TxCompletion, RxCompletion are in the array): kept here, not registered.
    rxSub = IOSkywalkRxSubmissionQueue::withPool(pool, kRxCapacity, 0, 0, this, rxSubmissionAction, this, 0);
    for (uint32_t i = 0; i < kNumTx; i++) {
        // Service class of each queue (Apple: table {0, 0x00100080, 0x00380100, 0x00400180} by AC) is NOT set: the plain factory
        // has no parameter for it, the BCM subclass passes it to IOSkywalkTxSubmissionQueue::initWithPool.
        txSub[i] = IOSkywalkTxSubmissionQueue::withPool(pool, kTxCapacity, i, this, txSubmissionAction, this, 0);
        if (!txSub[i]) {
            itlmtTrace(ctl, "txSub[%u]=null", i);
            return false;
        }
    }
    txComp = IOSkywalkTxCompletionQueue::withPool(pool, kTxCapacity, kNumTx, this, txCompletionAction, this, 0);
    rxComp = IOSkywalkRxCompletionQueue::withPool(pool, kRxCapacity, kNumTx + 1, this, rxCompletionAction, this, 0);
    itlmtTrace(ctl, "queues rxSub=%d txComp=%d rxComp=%d", rxSub != nullptr, txComp != nullptr, rxComp != nullptr);
    return rxSub && txComp && rxComp;
}

bool itlmt7925WifiInterface::startWithInfo(IO80211Controller *controller, IOSkywalkEthernetInterface::RegistrationInfo *info)
{
    ctl = controller;
    if (!info || field<uint32_t>(info, 0) != 1 || field<uint32_t>(info, 4) != 0x130) {
        itlmtTrace(ctl, "sw:bad info");
        return false;
    }
    // AppleBCMWLANSkywalkInterface::start: for role 1 it calls deferBSDAttach(true) before IO80211InfraInterface::start
    if (getInterfaceRole() == 1) {
        IOReturn dr = deferBSDAttach(true);
        itlmtTrace(ctl, "sw:deferBSDAttach=%x", dr);
    }
    bool sup = super::start(controller);       // IO80211InfraInterface::start(IOService *)
    itlmtTrace(ctl, "sw:IO80211InfraInterface::start=%d", sup);
    if (!sup) return false;

    IO80211WorkQueue *wq = getWorkQueue();
    itlmtTrace(ctl, "sw:getWorkQueue=%s", wq ? "ok" : "null");
    if (!wq) return false;
    bool made = createPoolAndQueues();
    itlmtTrace(ctl, "sw:pool+queues=%d", made);
    if (!made) {
        destroyPoolAndQueues();
        return false;
    }

    IOSkywalkPacketQueue *queues[kNumTx + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < kNumTx; i++) queues[n++] = txSub[i];
    queues[n++] = txComp;
    queues[n++] = rxComp;
    wq->retain();
    queued = wq;
    for (uint32_t i = 0; i < n; i++) {
        IOReturn r = wq->addEventSource(queues[i]);
        if (r != kIOReturnSuccess) {
            itlmtTrace(ctl, "sw:addEventSource[%u]=%x", i, r);
            // remove the ones already on the loop: destroyPoolAndQueues removes every non-null queue, a failed add is harmless
            destroyPoolAndQueues();
            return false;
        }
    }
    itlmtTrace(ctl, "sw:addEventSource x%u ok", n);
    uint32_t dp = 0;
    if (PE_parse_boot_argn("itlmt_native_dp", &dp, sizeof(dp)) && dp) {
        core = controller->getProvider();
        rxRing = (RxFrame *)IOMalloc(sizeof(RxFrame) * 64);
        dpTimer = IOTimerEventSource::timerEventSource(this, datapathTick);
        if (!core || !rxRing || !dpTimer || queued->addEventSource(dpTimer) != kIOReturnSuccess) {
            itlmtTrace(ctl, "dp:timer/ring setup failed");
            destroyPoolAndQueues();
            return false;
        }
        ItlNativeSink sink = {1, this, receiveFromCore, linkFromCore};
        IOReturn br = core->callPlatformFunction(ITL_NATIVE_BIND, false, &sink, nullptr, nullptr, nullptr);
        itlmtTrace(ctl, "dp:bind=%x", br);
        if (br != kIOReturnSuccess) { destroyPoolAndQueues(); return false; }
        bound = true;
        dpTimer->setTimeoutMS(10);
    }

    headroom = field<uint16_t>(info, 0x3c);
    field<uint32_t>(info, 0x48) -= headroom;
    field<uint32_t>(info, 0x4c) -= headroom;

    IOReturn rr = registerInfraEthernetInterface(info, queues, n, pool, pool);
    itlmtTrace(ctl, "sw:registerInfraEthernetInterface=%x", rr);
    if (rr != kIOReturnSuccess) {
        destroyPoolAndQueues();
        return false;
    }
    // AppleBCMWLANSkywalkInterface::start, after a successful registerInfraEthernetInterface: registerService(kIOServiceAsynchronous)
    // (call [vptr+0x5b0]) then setRunningState(false) (call [vptr+0x930]). Gated by itlmt_native_publish >= 1 (default 2).
    uint32_t publish = 2;
    PE_parse_boot_argn("itlmt_native_publish", &publish, sizeof(publish));
    if (getInterfaceRole() == 1 && publish >= 1) {
        registerService(kIOServiceAsynchronous);
        itlmtTrace(ctl, "sw:registerService(8) done");
        setRunningState(false);
        itlmtTrace(ctl, "sw:setRunningState(false) done");
    }
    return true;
}

// Reverse of start; Apple's stop does for each queue: disable (IOEventSource::disable, IOSkywalkPacketQueue overrides it),
// removeEventSource, release.
void itlmt7925WifiInterface::destroyPoolAndQueues()
{
    if (bound && core) {
        itlmtTrace(ctl, "dp:unbind begin");
        core->callPlatformFunction(ITL_NATIVE_UNBIND, false, this, nullptr, nullptr, nullptr);
        bound = false;
    }
    dpEnabled = false;
    if (dpTimer) {
        dpTimer->cancelTimeout();
        if (queued) queued->removeEventSource(dpTimer);
        dpTimer->release(); dpTimer = nullptr;
    }
    // Reclaim submitted packets while the completion queue is still enabled.
    while (pendingCount && txComp) {
        IOReturn r = txComp->enqueuePackets(pendingTx, 1, 0);
        if (r != kIOReturnSuccess) {
            // Submission packet completion must notify its source queue before freeing.
            itlmtTrace(ctl, "dp:teardown completion blocked=%x pending=%u", r, pendingCount);
            break;
        }
        memmove(pendingTx, pendingTx + 1, (--pendingCount) * sizeof(pendingTx[0]));
    }
    if (rxRing) { IOFree(rxRing, sizeof(RxFrame) * 64); rxRing = nullptr; }
    core = nullptr;
    IOSkywalkPacketQueue *all[kNumTx + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < kNumTx; i++) {
        all[n++] = txSub[i];
        txSub[i] = nullptr;
    }
    all[n++] = txComp;
    all[n++] = rxComp;
    txComp = nullptr;
    rxComp = nullptr;
    for (uint32_t i = 0; i < n; i++) {
        if (!all[i]) continue;
        all[i]->disable();
        if (queued) queued->removeEventSource(all[i]);
        all[i]->release();
    }
    rel(rxSub);
    rxSub = nullptr;
    rel(queued);
    queued = nullptr;
    rel(pool);
    pool = nullptr;
}

void itlmt7925WifiInterface::stop(IOService *provider)
{
    destroyPoolAndQueues();
    super::stop(provider);
}

void itlmt7925WifiInterface::free()
{
    destroyPoolAndQueues();
    if (rxLock) { IOLockFree(rxLock); rxLock = nullptr; }
    super::free();
}

CCLogStream *itlmt7925WifiInterface::getLogger() const
{
    return ctl ? ctl->getLogger() : nullptr;
}

IO80211Controller *itlmt7925WifiInterface::getController()
{
    return ctl;
}

uint32_t itlmt7925WifiInterface::getTxQueueDepth()
{
    return kTxCapacity;
}

uint32_t itlmt7925WifiInterface::getRxQueueCapacity()
{
    return kRxCapacity;
}

IOSkywalkPacketQueue *itlmt7925WifiInterface::getMultiCastQueue()
{
    return nullptr;     // optional in Apple's driver (state flag at +0xa2); not created here
}

uint16_t itlmt7925WifiInterface::getTxHeadroom()
{
    return headroom;
}

IOSkywalkRxCompletionQueue *itlmt7925WifiInterface::getRxCompQueue()
{
    return rxComp;
}

IOSkywalkTxCompletionQueue *itlmt7925WifiInterface::getTxCompQueue()
{
    return txComp;
}

IOSkywalkTxSubmissionQueue *itlmt7925WifiInterface::getTxSubQueue(apple80211_wme_ac ac)
{
    return (uint32_t)ac < kNumTx ? txSub[(uint32_t)ac] : nullptr;
}

IOSkywalkPacketBufferPool *itlmt7925WifiInterface::getTxPacketPool()
{
    return pool;
}

IOSkywalkPacketBufferPool *itlmt7925WifiInterface::getRxPacketPool()
{
    return pool;
}

IOReturn itlmt7925WifiInterface::enableDatapath()
{
    if (!bound || !core || !dpTimer) return kIOReturnNotReady;
    if (dpEnabled) return kIOReturnSuccess;
    itlmtTrace(ctl, "dp:enable queues begin");
    itlmtTrace(ctl, "dp:TX completion enable"); txComp->enable();
    itlmtTrace(ctl, "dp:RX completion enable"); rxComp->enable();
    for (uint32_t i = 0; i < kNumTx; i++) {
        itlmtTrace(ctl, "dp:TX submission %u enable", i);
        txSub[i]->enable();
    }
    dpEnabled = true;
    setRunningState(true);
    setProperty("itlmt_datapath_enabled", true);
    dpTimer->setTimeoutMS(10);
    itlmtTrace(ctl, "dp:enabled");
    return kIOReturnSuccess;
}

IOReturn itlmt7925WifiInterface::disableDatapath()
{
    if (!dpEnabled) return kIOReturnSuccess;
    itlmtTrace(ctl, "dp:disable queues");
    dpEnabled = false;
    setRunningState(false);
    for (uint32_t i = 0; i < kNumTx; i++) if (txSub[i]) txSub[i]->disable();
    // Completion queues remain active until outstanding TX ownership is returned.
    setProperty("itlmt_datapath_enabled", false);
    return kIOReturnSuccess;
}

IOReturn itlmt7925WifiInterface::enable(uint32_t options)
{
    itlmtTrace(ctl, "if:enable begin options=%x", options);
    IOReturn result = IO80211InfraProtocol::enable(options);
    if (result == kIOReturnSuccess && bound) result = enableDatapath();
    itlmtTrace(ctl, "if:enable result=%x", result);
    return result;
}

IOReturn itlmt7925WifiInterface::disable(uint32_t options)
{
    itlmtTrace(ctl, "if:disable begin options=%x", options);
    disableDatapath();
    IOReturn result = IO80211InfraProtocol::disable(options);
    itlmtTrace(ctl, "if:disable result=%x", result);
    return result;
}

IOReturn itlmt7925WifiInterface::processBSDCommand(__ifnet *netif, uint32_t command, void *data)
{
    bool media = command == 0xc02c6938 || command == 0xc0286938;
    uint32_t index = command == 0xc0286938 ? 1 : 0;
    bool firstMedia = media && !mediaTraced[index];
    if (firstMedia || command == 0x80206910)
        itlmtTrace(ctl, "if:bsd cmd=%x begin", command);
    IOReturn result = IO80211InfraProtocol::processBSDCommand(netif, command, data);
    // Record only media queries and interface flags; never dump ioctl payloads.
    if (firstMedia || command == 0x80206910 || (media && result != lastMediaResult[index]))
        itlmtTrace(ctl, "if:bsd cmd=%x result=%x", command, result);
    if (media) {
        mediaTraced[index] = true;
        lastMediaResult[index] = result;
        if ((++mediaQueryCount & 63) == 1)
            setProperty("itlmt_media_query_count", (uint64_t)mediaQueryCount, 64);
    }
    return result;
}

uint32_t itlmt7925WifiInterface::linkState()
{
    if (!bound || !rxLock) return IO80211InfraProtocol::linkState();
    IOLockLock(rxLock);
    bool up = radioUp;
    IOLockUnlock(rxLock);
    // SDK enum and round-12 medium-status branches: Undefined=0, Down=1, Up=2.
    return up ? 2 : 1;
}

uint32_t itlmt7925WifiInterface::getHardwareAssists()
{
    // Core TX accepts complete Ethernet frames. It has no checksum/TSO offload.
    return 0;
}

IOReturn itlmt7925WifiInterface::setMacAddress(ether_addr &address)
{
    IOEthernetAddress current = {};
    if (!ctl || ctl->getHardwareAddress(&current) != kIOReturnSuccess) return kIOReturnNotReady;
    bool same = memcmp(&address, &current, 6) == 0;
    itlmtTrace(ctl, "if:setMacAddress %s", same ? "station identity unchanged" : "change unsupported");
    // Initial MAC is already scheduled into core firmware setup. Do not falsely
    // accept a new address without rebuilding firmware device/BSS configuration.
    return same ? kIOReturnSuccess : kIOReturnUnsupported;
}

void itlmt7925WifiInterface::setInterfaceEnable(bool enabled)
{
    itlmtTrace(ctl, "if:setInterfaceEnable=%u begin", enabled);
    IO80211InfraProtocol::setInterfaceEnable(enabled);
    itlmtTrace(ctl, "if:setInterfaceEnable=%u done", enabled);
}

uint8_t itlmt7925WifiInterface::getNumTxQueues()
{
    return kNumTx;
}

uint32_t itlmt7925WifiInterface::submitPackets(IOSkywalkPacket *const *packets, uint32_t count)
{
    if (!dpEnabled || !core || !packets) return 0;
    IOLockLock(rxLock);
    bool up = radioUp;
    IOLockUnlock(rxLock);
    if (!up) { txStalled++; return 0; }
    bool first = !firstTxTrace;
    if (first) { firstTxTrace = true; itlmtTrace(ctl, "dp:TX callback begin"); }
    uint32_t used = 0;
    for (; used < count && pendingCount < 512; used++) {
        IOSkywalkPacket *p = packets[used];
        if (!p) break;
        bool single = p->getPacketBufferCount() == 1;
        uint32_t len = p->getDataLength(), size = single ? p->getBufferSize() : 0;
        uint16_t offset = single ? p->getDataOffset() : 0;
        void *base = single ? p->getDataVirtualAddress() : nullptr;
        int result = -1;
        if (base && len >= 14 && len <= 1600 && offset <= size && len <= size - offset) {
            memcpy(txScratch, (const uint8_t *)base + offset, len);
            if (first && used == 0)
                itlmtTrace(ctl, "dp:TX len=%u offset=%u type=%x", len, offset, itl_checksum::read16(txScratch + 12));
            if (itl_checksum::ethernet(txScratch, len)) {
                ItlNativeTx tx = {txScratch, len, -1};
                IOReturn r = core->callPlatformFunction(ITL_NATIVE_TX, false, &tx, nullptr, nullptr, nullptr);
                if (r == kIOReturnSuccess) result = tx.result;
            } else txChecksumFailures++;
        }
        if (result > 0) { txStalled++; break; } // callback takes no ownership of this packet; retry on next tick
        if (result == 0) txSent++; else txDropped++;
        // Complete only after submission callback returns and the queue records consumption.
        pendingTx[pendingCount++] = p;
    }
    if (used && txSent + txDropped <= used) itlmtTrace(ctl, "dp:first TX consumed=%u", used);
    return used;
}

void itlmt7925WifiInterface::receiveFromCore(OSObject *owner, const uint8_t *bytes, uint32_t length)
{
    auto *i = (itlmt7925WifiInterface *)owner;
    if (!i->rxLock) return;
    IOLockLock(i->rxLock);
    uint32_t next = (i->rxHead + 1) % 64;
    if (!i->rxRing || !bytes || length < 14 || length > 1600 || next == i->rxTail) i->rxDropped++;
    else {
        RxFrame &f = i->rxRing[i->rxHead];
        f.length = length; memcpy(f.bytes, bytes, length);
        i->rxHead = next; i->rxReceived++;
    }
    IOLockUnlock(i->rxLock);
}

void itlmt7925WifiInterface::linkFromCore(OSObject *owner, bool up, uint64_t speed)
{
    auto *i = (itlmt7925WifiInterface *)owner;
    if (!i->rxLock) return;
    IOLockLock(i->rxLock);
    i->radioUp = up; i->radioSpeed = speed; i->linkDirty = true;
    IOLockUnlock(i->rxLock);
}

void itlmt7925WifiInterface::datapathTick(OSObject *owner, IOTimerEventSource *)
{
    ((itlmt7925WifiInterface *)owner)->drainDatapath();
}

void itlmt7925WifiInterface::drainDatapath()
{
    if (!bound || !core || !dpTimer) return;
    IOLockLock(rxLock);
    bool dirty = linkDirty, up = radioUp;
    uint64_t speed = radioSpeed;
    linkDirty = false;
    IOLockUnlock(rxLock);
    if (dirty) {
        if (!linkTraceValid || up != linkTraceUp) {
            linkTraceValid = true; linkTraceUp = up;
            itlmtTrace(ctl, "dp:radio link=%u", up);
        }
        ctl->setLinkStatus(kIONetworkLinkValid | (up ? kIONetworkLinkActive : 0),
                           up ? ctl->getSelectedMedium() : nullptr, up ? speed : 0);
        // Classic controller broadcast skips Skywalk (not IONetworkInterface).
        // The verified Skywalk nonvirtual method updates media and posts BSD link events.
        IOReturn reported = itlmtReportSkywalkLink(this,
            kIONetworkLinkValid | (up ? kIONetworkLinkActive : 0), kIOMediumIEEE80211Auto);
        if (reported != kIOReturnSuccess) {
            IOLockLock(rxLock); linkDirty = true; IOLockUnlock(rxLock);
            if (++linkReportFailures <= 3) itlmtTrace(ctl, "dp:BSD link report=%x", reported);
        } else linkReportFailures = 0;
        setProperty("itlmt_radio_link", up);
    }
    // TX staging has already copied packet bytes. Return Skywalk ownership exactly once.
    while (pendingCount) {
        if (!firstCompletionTrace) {
            firstCompletionTrace = true; itlmtTrace(ctl, "dp:TX completion begin");
        }
        IOReturn r = txComp->enqueuePackets(pendingTx, 1, 0);
        if (r != kIOReturnSuccess) {
            if (ticks == 0 || ticks == 100 || ticks == 200)
                itlmtTrace(ctl, "dp:TX completion=%x pending=%u", r, pendingCount);
            break;
        }
        memmove(pendingTx, pendingTx + 1, (--pendingCount) * sizeof(pendingTx[0]));
        txCompleted++;
    }
    if (dpEnabled) {
        // RX copies are moved onto the native gate; core worker never acquires this gate.
        for (uint32_t batch = 0; batch < 32; batch++) {
            RxFrame f;
            IOLockLock(rxLock);
            bool ready = rxHead != rxTail;
            if (ready) { f = rxRing[rxTail]; rxTail = (rxTail + 1) % 64; }
            IOLockUnlock(rxLock);
            if (!ready) break;
            if (!firstRxTrace) { firstRxTrace = true; itlmtTrace(ctl, "dp:RX allocation/enqueue begin"); }
            IOSkywalkPacket *p = nullptr;
            IOReturn r = pool->allocatePacket(1, &p, 1); // no-sleep allocation on the native work loop
            if (r == kIOReturnSuccess && p) {
                bool single = p->getPacketBufferCount() == 1;
                void *data = single ? p->getDataVirtualAddress() : nullptr;
                if (!data || p->getBufferSize() < f.length)
                    r = kIOReturnNoSpace;
                else {
                    memcpy(data, f.bytes, f.length);
                    r = p->setDataOffsetAndLength(0, f.length);
                    if (r == kIOReturnSuccess) r = rxComp->enqueuePackets(&p, 1, 0);
                }
                if (r != kIOReturnSuccess) pool->deallocatePacket(p);
            }
            if (r == kIOReturnSuccess && p) rxDelivered++;
            else { IOLockLock(rxLock); rxDropped++; IOLockUnlock(rxLock); }
            if (r == kIOReturnSuccess && p && rxDelivered == 1) itlmtTrace(ctl, "dp:first RX delivered");
        }
        for (uint32_t i = 0; i < kNumTx; i++) txSub[i]->requestDequeue(nullptr, 1);
    }
    if ((++ticks % 100) == 0) {
        IOLockLock(rxLock);
        uint32_t received = rxReceived, dropped = rxDropped;
        IOLockUnlock(rxLock);
        setProperty("itlmt_tx_sent", (uint64_t)txSent, 64);
        setProperty("itlmt_tx_drop", (uint64_t)txDropped, 64);
        setProperty("itlmt_tx_stall", (uint64_t)txStalled, 64);
        setProperty("itlmt_tx_completed", (uint64_t)txCompleted, 64);
        setProperty("itlmt_rx_received", (uint64_t)received, 64);
        setProperty("itlmt_rx_delivered", (uint64_t)rxDelivered, 64);
        setProperty("itlmt_rx_drop", (uint64_t)dropped, 64);
        setProperty("itlmt_tx_pending", (uint64_t)pendingCount, 32);
        setProperty("itlmt_tx_checksum_failed", (uint64_t)txChecksumFailures, 64);
    }
    dpTimer->setTimeoutMS(10);
}

#define ITL_IFACE_DEFS
#include "itlmt7925_iface_stubs.inc"
#undef ITL_IFACE_DEFS
