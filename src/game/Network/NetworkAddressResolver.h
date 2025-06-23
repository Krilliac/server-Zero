#pragma once
#include <ace/INET_Addr.h>
#include <ace/SOCK_Stream.h>
#include <array>

class NetworkAddressResolver
{
public:
    static ACE_INET_Addr GetLocalAddress(ACE_SOCK_Stream& stream);
    static ACE_INET_Addr GetPeerAddress(ACE_SOCK_Stream& stream);
    static void GetInterfaceMac(const ACE_INET_Addr& addr, uint8_t mac[6]);
    static uint16_t AllocateDynamicPort();
};
