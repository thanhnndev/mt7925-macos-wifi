// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// itlmt7925WifiInterface - native Wi-Fi interface (macOS 15.8.1 / 24H32 ABI), stage 2: interface + queues + pool, no radio.
//
// Shape taken from Apple's AppleBCMWLANSkywalkInterface (docs/IO80211-ABI-2.md, "Native stub, stage 2"): a subclass of the
// Family's IO80211InfraProtocol (0x120 bytes, 190 pure get/set handlers + setMacAddress), created by the controller, attached
// to it, started with an initialised RegistrationInfo, which registers [TxSubmission x4, TxCompletion, RxCompletion] and the
// packet pool with registerInfraEthernetInterface.
#pragma once
#include "../itlmt7925/io80211/IO80211Controller.h"
#include "../itlmt7925/io80211/IO80211WorkQueue.h"
#include "../itlmt7925/io80211/IO80211InfraProtocol.h"
#include "skywalk_min.h"
#include "../itlmt7925/itl_native_bridge.h"
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>


// Appends "; <text>" to the registry property "itlmt_native_trace" of `e` (cap 480 bytes). IOLog output of third-party kexts was
// not found in the collected logs of stage A/B, a registry property survives and is read with `ioreg -l`.
void itlmtTrace(IORegistryEntry *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

class itlmt7925WifiInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(itlmt7925WifiInterface)

public:
    static const uint32_t kNumTx = 4;       // AppleBCMWLANSkywalkInterface::withProvider fixes numTx = 4 (ACs 0..3)

    // Creates the object: alloc, init(), role and id (initWithProviderAndParameters), attach(controller). Not started yet.
    static itlmt7925WifiInterface *withController(IO80211Controller *controller, uint32_t ifId, uint32_t role);

    // The equivalent of AppleBCMWLANSkywalkInterface::start(core, RegistrationInfo*) (slot 657 there, not a Family virtual).
    bool startWithInfo(IO80211Controller *controller, IOSkywalkEthernetInterface::RegistrationInfo *info);

    bool init() override;
    void free() override;
    void stop(IOService *provider) override;
    IOReturn enable(uint32_t options) override;
    IOReturn disable(uint32_t options) override;
    void setInterfaceEnable(bool enabled) override;
    IOReturn prepareBSDInterface(__ifnet *, uint32_t) override;
    IOReturn getHardwareAddress(ether_addr *) override;
    uint32_t linkState() override;
    uint32_t getHardwareAssists() override;
    IOReturn processBSDCommand(__ifnet *netif, uint32_t command, void *data) override;

    // Accessors the Family calls on its interface (IO80211SkywalkInterface slots 418-439; AppleBCMWLANSkywalkInterface overrides them)
    CCLogStream *getLogger() const override;
    IO80211Controller *getController() override;
    uint32_t getTxQueueDepth() override;
    uint32_t getRxQueueCapacity() override;
    IOSkywalkPacketQueue *getMultiCastQueue() override;
    uint16_t getTxHeadroom() override;
    IOSkywalkRxCompletionQueue *getRxCompQueue() override;
    IOSkywalkTxCompletionQueue *getTxCompQueue() override;
    IOSkywalkTxSubmissionQueue *getTxSubQueue(apple80211_wme_ac ac) override;
    IOSkywalkPacketBufferPool *getTxPacketPool() override;
    IOSkywalkPacketBufferPool *getRxPacketPool() override;
    IOReturn enableDatapath() override;
    IOReturn disableDatapath() override;
    uint8_t getNumTxQueues() override;
    uint32_t submitPackets(IOSkywalkPacket *const *, uint32_t);
    static void receiveFromCore(OSObject *, const uint8_t *, uint32_t);
    static void linkFromCore(OSObject *, bool, uint64_t);
    static void datapathTick(OSObject *, IOTimerEventSource *);

#define ITL_IFACE_DECLS
#include "itlmt7925_iface_stubs.inc"
#undef ITL_IFACE_DECLS

private:
    void drainDatapath();
    IOService *core = nullptr;
    IOTimerEventSource *dpTimer = nullptr;
    IOLock *rxLock = nullptr;
    struct RxFrame { uint32_t length; uint8_t bytes[1600]; };
    RxFrame *rxRing = nullptr;
    uint32_t rxHead = 0, rxTail = 0;
    IOSkywalkPacket *pendingTx[512] = {};
    uint32_t pendingCount = 0;
    bool dpEnabled = false, bound = false, radioUp = false, linkDirty = false;
    bool firstTxTrace = false, firstRxTrace = false, firstCompletionTrace = false;
    bool linkTraceValid = false, linkTraceUp = false;
    uint32_t linkReportFailures = 0;
    bool mediaTraced[2] = {};
    IOReturn lastMediaResult[2] = {kIOReturnSuccess, kIOReturnSuccess};
    uint32_t mediaQueryCount = 0;
    uint8_t txScratch[1600] = {};
    uint32_t txChecksumFailures = 0;
    uint64_t radioSpeed = 0;
    uint32_t txSent = 0, txDropped = 0, txStalled = 0, txCompleted = 0;
    uint32_t rxReceived = 0, rxDropped = 0, rxDelivered = 0, ticks = 0;
    bool createPoolAndQueues();
    void destroyPoolAndQueues();

    IO80211Controller *ctl = nullptr;           // not retained: the controller owns the interface's lifetime
    IOSkywalkPacketBufferPool *pool = nullptr;
    IOSkywalkTxSubmissionQueue *txSub[kNumTx] = {};
    IOSkywalkTxCompletionQueue *txComp = nullptr;
    IOSkywalkRxCompletionQueue *rxComp = nullptr;
    IOSkywalkRxSubmissionQueue *rxSub = nullptr;   // common Rx submission queue (Apple creates it before any interface)
    IOWorkLoop *queued = nullptr;           // work queue the event sources were added to (retained)
    uint16_t headroom = 0;
};
