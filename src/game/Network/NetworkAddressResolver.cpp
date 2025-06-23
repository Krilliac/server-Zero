#include "NetworkAddressResolver.h"
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>

ACE_INET_Addr NetworkAddressResolver::GetLocalAddress(ACE_SOCK_Stream& stream)
{
    ACE_INET_Addr addr;
    stream.get_local_addr(addr);
    return addr;
}

ACE_INET_Addr NetworkAddressResolver::GetPeerAddress(ACE_SOCK_Stream& stream)
{
    ACE_INET_Addr addr;
    stream.get_remote_addr(addr);
    return addr;
}

void NetworkAddressResolver::GetInterfaceMac(const ACE_INET_Addr& addr, uint8_t mac[6])
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    // Get interface name from IP
    ifr.ifr_addr.sa_family = AF_INET;
    reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr = 
        *reinterpret_cast<const in_addr*>(addr.get_addr());
    
    ioctl(fd, SIOCGIFNAME, &ifr);
    ioctl(fd, SIOCGIFHWADDR, &ifr);
    std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    
    close(fd);
}

uint16_t NetworkAddressResolver::AllocateDynamicPort()
{
    ACE_SOCK_Dgram sock;
    ACE_INET_Addr addr(0); // Let OS choose port
    sock.open(addr);
    sock.get_local_addr(addr);
    return addr.get_port_number();
}
