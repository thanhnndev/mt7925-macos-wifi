// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include <IOKit/IOService.h>
// Internal versioned ABI, kernel pointers only; no userspace entry point.
struct ItlNativeSink {
    uint32_t version;
    OSObject *owner;
    OSObject *lifetime; // retained with each out-of-lock link callback
    void (*receive)(OSObject *, const uint8_t *, uint32_t);
    void (*link)(OSObject *, bool, uint64_t);
};
struct ItlNativeTx { const uint8_t *bytes; uint32_t length; int result; };
#define ITL_NATIVE_BIND "itlmt-native-bind-v2"
#define ITL_NATIVE_UNBIND "itlmt-native-unbind-v2"
#define ITL_NATIVE_TX "itlmt-native-tx-v1"
#define ITL_NATIVE_MAC "itlmt-native-mac-v1"
