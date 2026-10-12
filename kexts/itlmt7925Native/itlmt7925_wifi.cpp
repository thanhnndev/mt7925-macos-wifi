// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// itlmt7925Wifi - native IO80211Controller front end for the MT7925 (macOS 15.8.1 / 24H32 ABI), stage 1: controller only.
//
// Matches the core driver (`itlmt7925`) as its provider and does nothing unless boot-arg itlmt_native=1 is set. What it does
// today is exactly what IO80211Controller::start() demands of a subclass (docs/IO80211-ABI-2.md, "Controller startup" and
// "Native stub"): a work queue before start, a real CCLogStream from getLogger(), a non-null fault reporter from
// getFaultReporterFromDriver() (a null one panics in findAndAttachToFaultReporter). It creates no interface, no queues, and
// answers every request as prohibited; the data path stays on the Ethernet front end in the core kext.
//
// Stage 2 (second boot-arg itlmt_native_if=1): after the base start it also creates the native interface
// (itlmt7925_iface.cpp), in the order of AppleBCMWLANCore::createSkywalkInterface: object -> attachInterface -> RegistrationInfo
// -> interface start (which registers pool + queues) -> detachInterface on failure. Still no radio.
#include "../itlmt7925/io80211/IO80211Controller.h"
#include "../itlmt7925/io80211/IO80211WorkQueue.h"
#include "corecapture_min.h"
#include "itlmt7925_iface.h"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <string.h>

#define LOGT "itlmt7925Wifi: "

class itlmt7925Wifi : public IO80211Controller {
    OSDeclareDefaultStructors(itlmt7925Wifi)

public:
    bool init(OSDictionary *properties = nullptr) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    // IO80211Controller virtuals the base class needs from a subclass (see ABI-2: slots 397, 399 and the pure ones)
    bool createWorkQueue() override;
    IO80211WorkQueue *getWorkQueue() const override;
    IOWorkLoop *getWorkLoop() const override;
    CCLogStream *getLogger() const override;
    IO80211FaultReporter *getFaultReporterFromDriver() override;
    IOReturn isCommandProhibited(int cmd) override;
    IOReturn handleCardSpecific(IO80211SkywalkInterface *i, unsigned long cmd, void *data, bool set) override;
    IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *i, apple80211_version_data *d) override;
    IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *i, apple80211_version_data *d) override;
    IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *i, apple80211_capability_data *d) override;
    IOReturn getPOWER(IO80211SkywalkInterface *i, apple80211_power_data *d) override;
    IOReturn setPOWER(IO80211SkywalkInterface *i, apple80211_power_data *d) override;
    IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *i, apple80211_country_code_data *d) override;
    IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *i, apple80211_country_code_data *d) override;
    IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *i, apple80211_debug_command *d) override;

private:
    bool makeLogger(IOService *owner);
    bool makeFaultReporter(IOService *owner);
    bool makeInterface(IOService *provider);
    void dropInterface();

    IO80211WorkQueue *wq = nullptr;
    bool started = false;                   // IO80211Controller::start returned (getWorkLoop mode 1 switches to wq after it)
    uint32_t wlMode = 0;                    // itlmt_native_wl
    IOWorkLoop *loop = nullptr;             // fallback IOWorkLoop when the provider has none (a real thread, unlike wq)
    CCPipe *logPipe = nullptr;
    CCStream *logStream = nullptr;          // a CCLogStream (options type 0)
    CCPipe *dataPipe = nullptr;
    CCStream *dataStream = nullptr;         // a CCDataStream (options type 1)
    CCFaultReporter *ccFault = nullptr;
    IO80211FaultReporter *fault = nullptr;
    itlmt7925WifiInterface *iface = nullptr;
};

#define super IO80211Controller
OSDefineMetaClassAndStructors(itlmt7925Wifi, IO80211Controller)

namespace {

const char kOwnerId[] = "com.itlmt7925.native";

template <typename T>
inline void put(uint8_t *block, size_t off, T v)
{
    memcpy(block + off, &v, sizeof(v));
}

inline void rel(void *o)
{
    if (o) ((OSObject *)o)->release();
}

// errno-style value Apple's BCM driver returns from isCommandProhibited() when it rejects commands (ENOTSUP)
const IOReturn kProhibited = 0x3a;

}

bool itlmt7925Wifi::init(OSDictionary *properties)
{
    return super::init(properties);
}

// CCPipeOptions / CCStreamOptions as Apple fills them (ABI-2 section 1; keys of the CoreCapture registry entry in brackets).
// Pipe options (zeroed, 0x350 bytes): +0 [PipeType], +0x10 [PipeSize], +0x18 [MinLogSizeToNotify], +0x20 [NotifyThreshold],
// +0x24 [Filename], +0x124 [LogIdentifier], +0x224 [FileSize], +0x228 [NumberOfFiles], +0x244 [DirectoryName].
bool itlmt7925Wifi::makeLogger(IOService *owner)
{
    uint8_t *po = (uint8_t *)IOMalloc(kCCPipeOptionsSize);
    uint8_t *so = (uint8_t *)IOMalloc(kCCStreamOptionsSize);
    bool ok = false;
    if (po && so) {
        bzero(po, kCCPipeOptionsSize);
        bzero(so, kCCStreamOptionsSize);
        // The values of Apple's ControlPath pipe (IO80211Controller::setupControlPathLogging)
        put<uint32_t>(po, 0x000, 0);                    // CCLogPipe
        put<uint64_t>(po, 0x010, 0x10000);
        put<uint64_t>(po, 0x018, 0x6666);
        put<uint32_t>(po, 0x020, 0x3e8);
        strlcpy((char *)po + 0x024, "itlmt7925Wifi", 0x100);
        strlcpy((char *)po + 0x124, "itlmt7925Wifi", 0x100);
        put<uint32_t>(po, 0x224, 0x200000);
        put<uint32_t>(po, 0x228, 2);
        strlcpy((char *)po + 0x244, "WiFi", 0x100);
        // Stream: class CCLogStream, level -1 (Apple's default, 5 only when io80211isDebuggable())
        put<uint32_t>(so, 0x000, 0);
        put<uint32_t>(so, 0x008, 0xffffffffu);
        put<uint32_t>(so, 0x00c, 0xffffffffu);
        strlcpy((char *)so + 0x058, "itlmt7925Wifi", 0xf0);

        logPipe = CCPipe::withOwnerNameCapacity(owner, kOwnerId, "itlmt7925Wifi", (const CCPipeOptions *)po);
        // CCLogStream::initWithPipeAndName casts the pipe to CCLogPipe without a null check: never pass a null pipe
        if (logPipe)
            logStream = CCStream::withPipeAndName(logPipe, "itlmt7925Wifi", (const CCStreamOptions *)so);
        ok = logPipe && logStream;
    }
    if (po) IOFree(po, kCCPipeOptionsSize);
    if (so) IOFree(so, kCCStreamOptionsSize);
    return ok;
}

// Fault reporter: IO80211FaultReporter(CCFaultReporter(CCDataStream(CCDataPipe))). The data pipe/stream take the option
// values Apple uses for its crash-tracer log in AppleBCMWLANBusInterfacePCIe::createCrashTracerLogger (pipe type 1,
// +4 = 2 [LogType], +8 = 6 [LogDataType], +0x10 = 0x40 [PipeSize], +0x230 = the owner; stream type 1, +8 = -1 as a qword).
bool itlmt7925Wifi::makeFaultReporter(IOService *owner)
{
    uint8_t *po = (uint8_t *)IOMalloc(kCCPipeOptionsSize);
    uint8_t *so = (uint8_t *)IOMalloc(kCCStreamOptionsSize);
    bool ok = false;
    if (po && so && wq) {
        bzero(po, kCCPipeOptionsSize);
        bzero(so, kCCStreamOptionsSize);
        put<uint32_t>(po, 0x000, 1);                    // CCDataPipe
        put<uint32_t>(po, 0x004, 2);
        put<uint64_t>(po, 0x008, 6);
        put<uint64_t>(po, 0x010, 0x40);
        strlcpy((char *)po + 0x024, "itlmt7925WifiFault", 0x100);
        strlcpy((char *)po + 0x124, "itlmt7925WifiFault", 0x100);
        strlcpy((char *)po + 0x244, "WiFi", 0x100);
        put<uint64_t>(po, 0x230, (uint64_t)(uintptr_t)owner);
        put<uint32_t>(so, 0x000, 1);                    // CCDataStream
        put<uint64_t>(so, 0x008, ~0ull);

        dataPipe = CCPipe::withOwnerNameCapacity(owner, kOwnerId, "itlmt7925WifiFault", (const CCPipeOptions *)po);
        if (dataPipe)
            dataStream = CCStream::withPipeAndName(dataPipe, "itlmt7925WifiFault", (const CCStreamOptions *)so);
        if (dataStream)
            ccFault = CCFaultReporter::withStreamWorkloop((CCDataStream *)dataStream, (IOWorkLoop *)wq);
        if (ccFault)
            fault = IO80211FaultReporter::allocWithParams(ccFault);
        ok = fault != nullptr;
    }
    if (po) IOFree(po, kCCPipeOptionsSize);
    if (so) IOFree(so, kCCStreamOptionsSize);
    return ok;
}

bool itlmt7925Wifi::start(IOService *provider)
{
    uint32_t on = 0;
    if (!PE_parse_boot_argn("itlmt_native", &on, sizeof(on)) || !on) {
        IOLog(LOGT "idle (boot-arg itlmt_native not set)\n");
        return false;
    }
    if (!createWorkQueue()) {
        IOLog(LOGT "no work queue\n");
        return false;
    }
    if (!makeLogger(provider)) {
        IOLog(LOGT "no logger\n");
        return false;
    }
    if (!makeFaultReporter(provider)) {
        IOLog(LOGT "no fault reporter\n");
        return false;
    }
    if (!super::start(provider)) {
        IOLog(LOGT "IO80211Controller::start failed\n");
        return false;
    }
    PE_parse_boot_argn("itlmt_native_wl", &wlMode, sizeof(wlMode));
    started = true;
    registerService();
    uint32_t withIf = 0;
    if (PE_parse_boot_argn("itlmt_native_if", &withIf, sizeof(withIf)) && withIf) {
        if (!makeInterface(provider)) {
            // the controller stays up without an interface; nothing else depends on it
            IOLog(LOGT "interface creation failed, controller only\n");
            return true;
        }
        IOLog(LOGT "controller and interface started (no radio)\n");
        return true;
    }
    IOLog(LOGT "controller started (no interface: itlmt_native_if not set)\n");
    return true;
}

// The core driver's Ethernet front end publishes IOMACAddress on itself (IOEthernetController::start); it is a child of the core.
static bool findMac(IOService *provider, uint8_t out[6])
{
    bool found = false;
    if (IORegistryIterator *it = IORegistryIterator::iterateOver(provider, gIOServicePlane)) {
        while (!found) {
            IORegistryEntry *e = it->getNextObject();
            if (!e) break;
            if (OSData *d = OSDynamicCast(OSData, e->getProperty("IOMACAddress"))) {
                if (d->getLength() == 6) {
                    memcpy(out, d->getBytesNoCopy(), 6);
                    found = true;
                }
            }
        }
        it->release();
    }
    return found;
}

// Mirrors AppleBCMWLANCore::createSkywalkInterface (0xffffff800163aaf8, ABI-2 "Native stub, stage 2"). Role 2 (LowLatency) is the
// only role that gets a BSD name prefix + unit in the RegistrationInfo; role 1 (primary) sets [info+0x10] = 1 and nothing else.
bool itlmt7925Wifi::makeInterface(IOService *provider)
{
    uint8_t mac[6];
    if (!findMac(provider, mac)) {
        // locally administered placeholder: the Ethernet front end has not published a MAC (or is not loaded)
        static const uint8_t fallback[6] = {0x02, 0x00, 0x79, 0x25, 0x00, 0x01};
        memcpy(mac, fallback, 6);
        IOLog(LOGT "no IOMACAddress under the core driver, using a placeholder\n");
    }
    // Role 1 is the primary station interface. Role 2 (LowLatency) is a secondary one: IO80211SkywalkInterface::start needs
    // ivars->parentInterface for it (it logs a failure at IO80211SkywalkInterface.cpp:0x1f8 otherwise), createSkywalkInterface only
    // makes it from a bus flag and passes the parent as its 4th argument. itlmt_native_role=2 keeps the old path for comparison.
    uint32_t role = 1;
    PE_parse_boot_argn("itlmt_native_role", &role, sizeof(role));
    // Role 1: createSkywalkInterface leaves the BSD name to the family. An explicit name/unit is only used when itlmt_native_unit is
    // given (or for role 2, where Apple always passes one).
    uint32_t unit = 9;
    bool explicitName = PE_parse_boot_argn("itlmt_native_unit", &unit, sizeof(unit)) || role == 2;

    itlmtTrace(this, "mk:start role=%u unit=%u explicit=%d", role, unit, explicitName);
    itlmt7925WifiInterface *ifc = itlmt7925WifiInterface::withController(this, 0, role);
    itlmtTrace(this, "mk:withController=%s", ifc ? "ok" : "null");
    if (!ifc) return false;
    bool att = attachInterface(ifc, this);
    itlmtTrace(this, "mk:attachInterface=%d", att);
    if (!att) {
        ifc->release();
        return false;
    }
    uint8_t block[0x130];
    bzero(block, sizeof(block));
    IOSkywalkEthernetInterface::RegistrationInfo *info = (IOSkywalkEthernetInterface::RegistrationInfo *)block;
    ifc->initRegistrationInfo(info, 1, 0x130);
    // The fields createSkywalkInterface sets after initRegistrationInfo (offsets from its disassembly; meaning of +0x14, +0x24,
    // +0x28 and +0xc is only partly known): +0x14 = 2, MAC at +0x108, +0x24 = +0x28 = 0x80, +0xc = 3, +0x3c (u16) = 0x40,
    // +0x40 flags = 4 for role 2 (| 2 when the provider has IOPMResetPowerStateOnWake: not replicated), BSD prefix at +0x30,
    // unit at +0x38.
    static const char kPrefix[] = "en";
    put<uint32_t>(block, 0x14, 2);
    memcpy(block + 0x108, mac, 6);
    put<uint32_t>(block, 0x24, 0x80);
    put<uint32_t>(block, 0x28, 0x80);
    put<uint32_t>(block, 0x0c, 3);
    put<uint16_t>(block, 0x3c, 0x40);
    if (role == 1) put<uint32_t>(block, 0x10, 1);       // createSkywalkInterface: [info+0x10] = 1 for role 1 (meaning chưa xác định)
    // Role 1: initRegistrationInfo leaves +0x38 = 0xFFFFFFFF (unit not assigned yet) and registerNetworkInterface only sets
    // IOInterfaceUnit for unit >= 0, so without it IOSkywalkNetworkBSDClient::start never finds IOInterfaceUnit and cannot attach.
    // The prefix stays the Ethernet default ("en"). Role 2 keeps the explicit prefix/flags Apple passes.
    put<uint32_t>(block, 0x38, unit);
    if (role == 2 || explicitName) {
        put<uint32_t>(block, 0x40, 4);
        put<uint64_t>(block, 0x30, (uint64_t)(uintptr_t)kPrefix);
    }

    if (!ifc->startWithInfo(this, info)) {
        itlmtTrace(this, "mk:startWithInfo failed");
        detachInterface(ifc, false);
        ifc->release();
        return false;
    }
    itlmtTrace(this, "mk:startWithInfo ok");
    iface = ifc;
    // AppleBCMWLANCore::setupDriver clears the deferral on its interface with deferBSDAttach(false) (removeProperty
    // IODeferBSDAttach + registerService(0)), which lets IOSkywalkNetworkBSDClient attach. itlmt_native_publish >= 2 does it here.
    uint32_t publish = 1;
    PE_parse_boot_argn("itlmt_native_publish", &publish, sizeof(publish));
    {
        // Evidence for the IO80211Glue.cpp:417 panic: which work loop each layer sees and the gate/thread state right now.
        IOWorkLoop *pl = getProvider() ? getProvider()->getWorkLoop() : nullptr;
        IOWorkLoop *cl = getWorkLoop();
        IOWorkLoop *il = ifc->getWorkLoop();
        IOWorkLoop *w = (IOWorkLoop *)wq;
        itlmtTrace(this, "mk:loops wq=%p provider=%p ctl=%p iface=%p wq.inGate=%d wq.onThread=%d", w, pl, cl, il,
                   w ? w->inGate() : -1, w ? w->onThread() : -1);
    }
    if (role == 1 && publish >= 2) {
        IOReturn dr = ifc->deferBSDAttach(false);
        itlmtTrace(this, "mk:deferBSDAttach(false)=%x", dr);
    }
    return true;
}

void itlmt7925Wifi::dropInterface()
{
    if (!iface) return;
    itlmt7925WifiInterface *ifc = iface;
    iface = nullptr;
    detachInterface(ifc, false);
    ifc->release();
}

void itlmt7925Wifi::stop(IOService *provider)
{
    dropInterface();
    super::stop(provider);
}

void itlmt7925Wifi::free()
{
    // reverse order of creation; the reporter chain holds the stream and the work queue
    dropInterface();
    rel(fault);       fault = nullptr;
    rel(ccFault);     ccFault = nullptr;
    rel(dataStream);  dataStream = nullptr;
    rel(dataPipe);    dataPipe = nullptr;
    rel(logStream);   logStream = nullptr;
    rel(logPipe);     logPipe = nullptr;
    rel(wq);          wq = nullptr;
    rel(loop);        loop = nullptr;
    super::free();
}

// The base class never calls createWorkQueue() from start(): getWorkQueue() must already return a queue (ABI-2 round 2).
// Apple's own BCM driver takes IO80211WorkQueue::workQueue() when boot-arg wlan.workQueue.create is set.
bool itlmt7925Wifi::createWorkQueue()
{
    if (!wq) wq = IO80211WorkQueue::workQueue();
    return wq != nullptr;
}

IO80211WorkQueue *itlmt7925Wifi::getWorkQueue() const
{
    return wq;
}

// IONetworkController::start (24H32, +0x1d4) compares this->getWorkLoop() with provider->getWorkLoop() and, when they
// differ, calls ml_thread_policy(workLoop->getThread(), ...). IO80211WorkQueue::getThread is an 8-byte stub returning
// NULL, so returning the queue (what IO80211Controller::getWorkLoop evidently does) panicked at 0.47 s in stage A.
// Return the provider's real work loop, so the compare matches and the call is skipped; fall back to an own IOWorkLoop
// (real thread) so the call would be harmless if it were reached.
// itlmt_native_wl=1 (experiment): once start has returned, answer with the IO80211WorkQueue like Apple's controller does. The
// BSD client's command gate and IO80211Glue's inGate()/onThread() check (panic IO80211Glue.cpp:417) are suspected to use the loop
// reached through getWorkLoop(); chưa xác định (round 10 dumps decide).
IOWorkLoop *itlmt7925Wifi::getWorkLoop() const
{
    if (started && wlMode == 1 && wq) return (IOWorkLoop *)wq;
    IOService *p = getProvider();
    IOWorkLoop *l = p ? p->getWorkLoop() : nullptr;
    if (l) return l;
    itlmt7925Wifi *self = const_cast<itlmt7925Wifi *>(this);
    if (!self->loop) self->loop = IOWorkLoop::workLoop();
    return self->loop;
}

CCLogStream *itlmt7925Wifi::getLogger() const
{
    return (CCLogStream *)logStream;
}

IO80211FaultReporter *itlmt7925Wifi::getFaultReporterFromDriver()
{
    return fault;
}

// Every request is refused until an interface exists.
IOReturn itlmt7925Wifi::isCommandProhibited(int)
{
    return kProhibited;
}

IOReturn itlmt7925Wifi::handleCardSpecific(IO80211SkywalkInterface *, unsigned long, void *, bool)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::getDRIVER_VERSION(IO80211SkywalkInterface *, apple80211_version_data *)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::getHARDWARE_VERSION(IO80211SkywalkInterface *, apple80211_version_data *)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::getCARD_CAPABILITIES(IO80211SkywalkInterface *, apple80211_capability_data *)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::getPOWER(IO80211SkywalkInterface *, apple80211_power_data *)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::setPOWER(IO80211SkywalkInterface *, apple80211_power_data *)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::getCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::setCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *)
{
    return kIOReturnUnsupported;
}

IOReturn itlmt7925Wifi::setGET_DEBUG_INFO(IO80211SkywalkInterface *, apple80211_debug_command *)
{
    return kIOReturnUnsupported;
}
