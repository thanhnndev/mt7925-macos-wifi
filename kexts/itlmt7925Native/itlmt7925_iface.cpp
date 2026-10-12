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

#include <IOKit/IOLib.h>
#include <stdarg.h>
#include <string.h>

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
uint32_t txSubmissionAction(OSObject *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, uint32_t, void *) { return 0; }
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
    char buf[512];
    buf[0] = 0;
    if (OSString *old = OSDynamicCast(OSString, e->getProperty("itlmt_native_trace")))
        strlcpy(buf, old->getCStringNoCopy(), sizeof(buf));
    if (buf[0]) strlcat(buf, "; ", sizeof(buf));
    if (strlcat(buf, add, 480) >= 480) return;      // full: keep what is there
    e->setProperty("itlmt_native_trace", buf);
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
    return super::init();
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
    // (call [vptr+0x5b0]) then setRunningState(false) (call [vptr+0x930]). Gated by itlmt_native_publish >= 1 (default 1).
    uint32_t publish = 1;
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
    return kIOReturnUnsupported;    // no radio in stage 2
}

IOReturn itlmt7925WifiInterface::disableDatapath()
{
    return kIOReturnUnsupported;
}

uint8_t itlmt7925WifiInterface::getNumTxQueues()
{
    return kNumTx;
}

#define ITL_IFACE_DEFS
#include "itlmt7925_iface_stubs.inc"
#undef ITL_IFACE_DEFS
