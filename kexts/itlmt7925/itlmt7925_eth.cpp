// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// itlmt7925 - Ethernet interface on top of the associated MT7925 link (the "network card" macOS sees).
// Created by itlmt7925::m3Link() after the 4-way handshake; frames to send arrive in outputPacket(), frames received from the AP are
// pushed in with rxPacket(). The 802.11 <-> Ethernet conversion is done in itlmt7925_m3.cpp.
#include "itlmt7925.hpp"

#include <IOKit/IOWorkLoop.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>
#include <sys/kpi_mbuf.h>
#include <string.h>

class itlmt7925Eth : public IOEthernetController {
    OSDeclareDefaultStructors(itlmt7925Eth)

public:
    bool setup(itlmt7925 *d, const uint8_t *m);
    void rxPacket(const uint8_t *e, uint32_t len);
    void setLink(bool up);

    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    bool createWorkLoop() override;
    IOWorkLoop *getWorkLoop() const override;
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    IOReturn enable(IONetworkInterface *netif) override;
    IOReturn disable(IONetworkInterface *netif) override;
    UInt32 outputPacket(mbuf_t m, void *param) override;
    IOReturn setPromiscuousMode(bool active) override;
    IOReturn setMulticastMode(bool active) override;
    IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;
    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;

private:
    itlmt7925 *drv = nullptr;
    uint8_t addr[6] = {0};
    IOEthernetInterface *netif = nullptr;
    IOWorkLoop *wl = nullptr;
    IONetworkMedium *medium = nullptr;
    uint8_t txb[1600];
};

#define super IOEthernetController
OSDefineMetaClassAndStructors(itlmt7925Eth, IOEthernetController)

bool itlmt7925Eth::setup(itlmt7925 *d, const uint8_t *m)
{
    drv = d;
    memcpy(addr, m, 6);
    return true;
}

bool itlmt7925Eth::createWorkLoop()
{
    if (!wl) wl = IOWorkLoop::workLoop();
    return wl != nullptr;
}

IOWorkLoop *itlmt7925Eth::getWorkLoop() const
{
    return wl;
}

bool itlmt7925Eth::start(IOService *provider)
{
    if (!super::start(provider)) return false;

    UInt64 speed = drv ? drv->linkSpeedBps() : 54ULL * 1000000ULL;      // estimated PHY rate (54 Mb/s unless HT/VHT is in use)
    OSDictionary *media = OSDictionary::withCapacity(1);
    IONetworkMedium *med = IONetworkMedium::medium(kIOMediumEthernetAuto, speed);
    if (!media || !med) {
        if (med) med->release();
        if (media) media->release();
        return false;
    }
    IONetworkMedium::addMedium(media, med);
    bool ok = publishMediumDictionary(media) && setCurrentMedium(med) && setSelectedMedium(med);
    if (ok) setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, med, speed);
    medium = med;                                                          // kept for setLink()
    medium->retain();
    med->release();
    media->release();
    if (!ok) return false;

    if (!attachInterface((IONetworkInterface **)&netif, true)) return false;
    registerService();
    return true;
}

void itlmt7925Eth::stop(IOService *provider)
{
    if (netif) { detachInterface(netif, true); netif = nullptr; }
    super::stop(provider);
}

void itlmt7925Eth::free()
{
    if (medium) { medium->release(); medium = nullptr; }
    if (wl) { wl->release(); wl = nullptr; }
    super::free();
}

IOReturn itlmt7925Eth::getHardwareAddress(IOEthernetAddress *a)
{
    memcpy(a->bytes, addr, 6);
    return kIOReturnSuccess;
}

IOReturn itlmt7925Eth::enable(IONetworkInterface *)
{
    return kIOReturnSuccess;
}

IOReturn itlmt7925Eth::disable(IONetworkInterface *)
{
    return kIOReturnSuccess;
}

IOReturn itlmt7925Eth::setPromiscuousMode(bool)
{
    return kIOReturnSuccess;
}

IOReturn itlmt7925Eth::setMulticastMode(bool)
{
    return kIOReturnSuccess;
}

IOReturn itlmt7925Eth::setMulticastList(IOEthernetAddress *, UInt32)
{
    return kIOReturnSuccess;
}

IOReturn itlmt7925Eth::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    if (group == gIONetworkFilterGroup) {
        *filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast | kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
        return kIOReturnSuccess;
    }
    return super::getPacketFilters(group, filters);
}

UInt32 itlmt7925Eth::outputPacket(mbuf_t m, void *)
{
    if (!m) return kIOReturnOutputDropped;
    size_t len = mbuf_pkthdr_len(m);
    if (len >= 14 && len <= sizeof(txb) && drv) {
        mbuf_copydata(m, 0, len, txb);
        drv->ethTx(txb, (uint32_t)len);
    }
    freePacket(m);
    return kIOReturnOutputSuccess;
}

// Link state for macOS: down while the driver is between networks, so DHCP and the routes follow the Wi-Fi link.
void itlmt7925Eth::setLink(bool up)
{
    if (!medium) return;
    UInt64 speed = drv ? drv->linkSpeedBps() : 54ULL * 1000000ULL;
    setLinkStatus(kIONetworkLinkValid | (up ? kIONetworkLinkActive : 0), up ? medium : nullptr, up ? speed : 0);
}

void itlmt7925Eth::rxPacket(const uint8_t *e, uint32_t len)
{
    if (!netif || len < 14) return;
    mbuf_t m = allocatePacket(len);
    if (!m) return;
    memcpy(mbuf_data(m), e, len);
    netif->inputPacket(m, len, IONetworkInterface::kInputOptionQueuePacket);
    netif->flushInputQueue();
}

// ---- driver side ----------------------------------------------------------------------------------------------------------------
bool itlmt7925::ethCreate()
{
    itlmt7925Eth *e = OSTypeAlloc(itlmt7925Eth);
    if (!e) return false;
    if (!e->init(nullptr) || !e->setup(this, mac)) { e->release(); return false; }
    if (!e->attach(this)) { e->release(); return false; }
    if (!e->start(this)) { e->detach(this); e->release(); return false; }
    eth = e;
    return true;
}

void itlmt7925::ethRx(const uint8_t *frame, uint32_t len)
{
    if (eth) eth->rxPacket(frame, len);
}

void itlmt7925::ethLink(bool up)
{
    if (eth) eth->setLink(up);
}
