#include "route.hpp"

#ifdef __linux__
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>
#ifndef ANDROID
#include <sys/socket.h>
#include <linux/rtnetlink.h>
#endif
#include <net/net.hpp>
#include <exception>
#include <charconv>
#endif
#ifdef __APPLE__
#include <net/net.hpp>
#include <util/str.hpp>
#endif
#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <strsafe.h>
#include <locale>
#include <codecvt>
#endif

#include <sstream>
#include <util/logging/logger.hpp>
#include <util/str.hpp>

namespace llarp::net
{
#ifndef __linux__
  void
  Execute(std::string cmd)
  {
    LogDebug(cmd);
#ifdef _WIN32
    system(cmd.c_str());
#else
    std::vector<std::string> parts_str;
    std::vector<const char*> parts_raw;
    std::stringstream in(cmd);
    for (std::string part; std::getline(in, part, ' ');)
    {
      if (part.empty())
        continue;
      parts_str.push_back(part);
    }
    for (const auto& part : parts_str)
    {
      parts_raw.push_back(part.c_str());
    }
    parts_raw.push_back(nullptr);
    const auto pid = fork();
    if (pid == -1)
    {
      throw std::runtime_error("failed to fork");
    }
    else if (pid == 0)
    {
      char* const* args = (char* const*)parts_raw.data();
      const auto result = execv(parts_raw[0], args);
      if (result)
      {
        std::cout << "failed: " << result << std::endl;
      }
      exit(result);
    }
    else
    {
      waitpid(pid, 0, 0);
    }
#endif
  }
#endif

#ifdef __linux__
#ifndef ANDROID
  struct NLSocket
  {
    NLSocket() : fd(socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE))
    {
      if (fd == -1)
        throw std::runtime_error("failed to make netlink socket");
    }

    ~NLSocket()
    {
      if (fd != -1)
        close(fd);
    }

    const int fd;
  };

  /* Helper structure for ip address data and attributes */
  typedef struct
  {
    unsigned char family;
    unsigned char bitlen;
    unsigned char data[sizeof(struct in6_addr)];
  } _inet_addr;

  /* */

#define NLMSG_TAIL(nmsg) ((struct rtattr*)(((intptr_t)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

  /* Add new data to rtattr */
  int
  rtattr_add(struct nlmsghdr* n, unsigned int maxlen, int type, const void* data, int alen)
  {
    int len = RTA_LENGTH(alen);
    struct rtattr* rta;

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
    {
      fprintf(stderr, "rtattr_add error: message exceeded bound of %d\n", maxlen);
      return -1;
    }

    rta = NLMSG_TAIL(n);
    rta->rta_type = type;
    rta->rta_len = len;

    if (alen)
    {
      memcpy(RTA_DATA(rta), data, alen);
    }

    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);

    return 0;
  }

  int
  do_route(int sock, int cmd, int flags, _inet_addr* dst, _inet_addr* gw, int def_gw, int if_idx)
  {
    struct
    {
      struct nlmsghdr n;
      struct rtmsg r;
      char buf[4096];
    } nl_request{};

    /* Initialize request structure */
    nl_request.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nl_request.n.nlmsg_flags = NLM_F_REQUEST | flags;
    nl_request.n.nlmsg_type = cmd;
    nl_request.n.nlmsg_pid = getpid();
    nl_request.r.rtm_family = dst->family;
    nl_request.r.rtm_table = RT_TABLE_MAIN;
    if (if_idx)
    {
      nl_request.r.rtm_scope = RT_SCOPE_LINK;
    }
    else
    {
      nl_request.r.rtm_scope = RT_SCOPE_NOWHERE;
    }
    /* Set additional flags if NOT deleting route */
    if (cmd != RTM_DELROUTE)
    {
      nl_request.r.rtm_protocol = RTPROT_BOOT;
      nl_request.r.rtm_type = RTN_UNICAST;
    }

    nl_request.r.rtm_family = dst->family;
    nl_request.r.rtm_dst_len = dst->bitlen;

    /* Select scope, for simplicity we supports here only IPv6 and IPv4 */
    if (nl_request.r.rtm_family == AF_INET6)
    {
      nl_request.r.rtm_scope = RT_SCOPE_UNIVERSE;
    }
    else
    {
      nl_request.r.rtm_scope = RT_SCOPE_LINK;
    }

    /* Set gateway */
    if (gw->bitlen != 0)
    {
      rtattr_add(&nl_request.n, sizeof(nl_request), RTA_GATEWAY, &gw->data, gw->bitlen / 8);
      nl_request.r.rtm_scope = 0;
      nl_request.r.rtm_family = gw->family;
    }

    /* Don't set destination and interface in case of default gateways */
    if (!def_gw)
    {
      /* Set destination network */
      rtattr_add(
          &nl_request.n, sizeof(nl_request), /*RTA_NEWDST*/ RTA_DST, &dst->data, dst->bitlen / 8);

      /* Set interface */
      rtattr_add(&nl_request.n, sizeof(nl_request), RTA_OIF, &if_idx, sizeof(int));
    }

    /* Send message to the netlink */
    return send(sock, &nl_request, sizeof(nl_request), 0);
  }

  int
  read_addr(const char* addr, _inet_addr* res)
  {
    if (strchr(addr, ':'))
    {
      res->family = AF_INET6;
      res->bitlen = 128;
    }
    else
    {
      res->family = AF_INET;
      res->bitlen = 32;
    }
    return inet_pton(res->family, addr, res->data);
  }

#endif
#endif

#ifdef _WIN32

  std::wstring
  get_win_sys_path()
  {
    wchar_t win_sys_path[MAX_PATH] = {0};
    const wchar_t* default_sys_path = L"C:\\Windows\\system32";

    if (!GetSystemDirectoryW(win_sys_path, _countof(win_sys_path)))
    {
      wcsncpy(win_sys_path, default_sys_path, _countof(win_sys_path));
      win_sys_path[_countof(win_sys_path) - 1] = L'\0';
    }
    return win_sys_path;
  }

  std::string
  RouteCommand()
  {
    std::wstring wcmd = get_win_sys_path() + L"\\route.exe";

    using convert_type = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_type, wchar_t> converter;
    return converter.to_bytes(wcmd);
  }

  template <typename Visit>
  void
  ForEachWIN32Interface(Visit visit)
  {
#define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
#define FREE(x) HeapFree(GetProcessHeap(), 0, (x))
    MIB_IPFORWARDTABLE* pIpForwardTable;
    DWORD dwSize = 0;
    DWORD dwRetVal = 0;

    pIpForwardTable = (MIB_IPFORWARDTABLE*)MALLOC(sizeof(MIB_IPFORWARDTABLE));
    if (pIpForwardTable == nullptr)
      return;

    if (GetIpForwardTable(pIpForwardTable, &dwSize, 0) == ERROR_INSUFFICIENT_BUFFER)
    {
      FREE(pIpForwardTable);
      pIpForwardTable = (MIB_IPFORWARDTABLE*)MALLOC(dwSize);
      if (pIpForwardTable == nullptr)
      {
        return;
      }
    }

    if ((dwRetVal = GetIpForwardTable(pIpForwardTable, &dwSize, 0)) == NO_ERROR)
    {
      for (int i = 0; i < (int)pIpForwardTable->dwNumEntries; i++)
      {
        visit(&pIpForwardTable->table[i]);
      }
    }
    FREE(pIpForwardTable);
#undef MALLOC
#undef FREE
  }
#endif

  void
  AddRoute(std::string ip, std::string gateway)
  {
    LogInfo("Add route: ", ip, " via ", gateway);
#ifdef __linux__
#ifndef ANDROID
    NLSocket sock;
    int default_gw = 0;
    int if_idx = 0;
    _inet_addr to_addr{};
    _inet_addr gw_addr{};
    int nl_cmd = RTM_NEWROUTE;
    int nl_flags = NLM_F_CREATE | NLM_F_EXCL;
    read_addr(gateway.c_str(), &gw_addr);
    read_addr(ip.c_str(), &to_addr);
    do_route(sock.fd, nl_cmd, nl_flags, &to_addr, &gw_addr, default_gw, if_idx);
#endif
#else
    std::stringstream ss;
#if _WIN32
    ss << RouteCommand() << " ADD " << ip << " MASK 255.255.255.255 " << gateway << " METRIC 2";
#elif __APPLE__
    ss << "/sbin/route -n add -host " << ip << " " << gateway;
#else
#error unsupported platform
#endif
    Execute(ss.str());
#endif
  }

  void
  DelRoute(std::string ip, std::string gateway)
  {
    LogInfo("Delete route: ", ip, " via ", gateway);
#ifdef __linux__
#ifndef ANDROID
    NLSocket sock;
    int default_gw = 0;
    int if_idx = 0;
    _inet_addr to_addr{};
    _inet_addr gw_addr{};
    int nl_cmd = RTM_DELROUTE;
    int nl_flags = 0;
    read_addr(gateway.c_str(), &gw_addr);
    read_addr(ip.c_str(), &to_addr);
    do_route(sock.fd, nl_cmd, nl_flags, &to_addr, &gw_addr, default_gw, if_idx);
#endif
#else
    std::stringstream ss;
#if _WIN32
    ss << RouteCommand() << " DELETE " << ip << " MASK 255.255.255.255 " << gateway << " METRIC 2";
#elif __APPLE__
    ss << "/sbin/route -n delete -host " << ip << " " << gateway;
#else
#error unsupported platform
#endif
    Execute(ss.str());
#endif
  }

  void
  AddDefaultRouteViaInterface(std::string ifname)
  {
    LogInfo("Add default route via ", ifname);
#ifdef __linux__
#ifndef ANDROID
    NLSocket sock;
    int default_gw = 1;
    int if_idx = if_nametoindex(ifname.c_str());
    _inet_addr to_addr{};
    _inet_addr gw_addr{};

    const auto maybe = GetIFAddr(ifname);
    if (not maybe.has_value())
      throw std::runtime_error("we dont have our own net interface?");
    int nl_cmd = RTM_NEWROUTE;
    int nl_flags = NLM_F_CREATE | NLM_F_EXCL;
    read_addr(maybe->toHost().c_str(), &gw_addr);
    do_route(sock.fd, nl_cmd, nl_flags, &to_addr, &gw_addr, default_gw, if_idx);
#endif
#elif _WIN32
    ifname.back()++;
    int ifindex = 0;
    // find interface index for address
    ForEachWIN32Interface([&ifindex, ifname = ifname](auto w32interface) {
      in_addr interface_addr;
      interface_addr.S_un.S_addr = (u_long)w32interface->dwForwardNextHop;
      std::array<char, 128> interface_str{};
      StringCchCopy(interface_str.data(), interface_str.size(), inet_ntoa(interface_addr));
      std::string interface_name{interface_str.data()};
      if (interface_name == ifname)
      {
        ifindex = w32interface->dwForwardIfIndex;
      }
    });
    Execute(
        RouteCommand() + " ADD 0.0.0.0 MASK 128.0.0.0 " + ifname + " IF "
        + std::to_string(ifindex));
    Execute(
        RouteCommand() + " ADD 128.0.0.0 MASK 128.0.0.0 " + ifname + " IF "
        + std::to_string(ifindex));
#elif __APPLE__
    Execute("/sbin/route -n add -cloning -net 0.0.0.0 -netmask 128.0.0.0 -interface " + ifname);
    Execute("/sbin/route -n add -cloning -net 128.0.0.0 -netmask 128.0.0.0 -interface " + ifname);
#else
#error unsupported platform
#endif
  }

  void
  DelDefaultRouteViaInterface(std::string ifname)
  {
    LogInfo("Remove default route via ", ifname);
#ifdef __linux__
#ifndef ANDROID
    NLSocket sock;
    int default_gw = 1;
    int if_idx = if_nametoindex(ifname.c_str());
    _inet_addr to_addr{};
    _inet_addr gw_addr{};
    const auto maybe = GetIFAddr(ifname);

    if (not maybe.has_value())
      throw std::runtime_error("we dont have our own net interface?");
    int nl_cmd = RTM_DELROUTE;
    int nl_flags = 0;
    read_addr(maybe->toHost().c_str(), &gw_addr);
    do_route(sock.fd, nl_cmd, nl_flags, &to_addr, &gw_addr, default_gw, if_idx);
#endif
#elif _WIN32
    ifname.back()++;
    Execute(RouteCommand() + " DELETE 0.0.0.0 MASK 128.0.0.0 " + ifname);
    Execute(RouteCommand() + " DELETE 128.0.0.0 MASK 128.0.0.0 " + ifname);
#elif __APPLE__
    Execute("/sbin/route -n delete -cloning -net 0.0.0.0 -netmask 128.0.0.0 -interface " + ifname);
    Execute(
        "/sbin/route -n delete -cloning -net 128.0.0.0 -netmask 128.0.0.0 -interface " + ifname);
#else
#error unsupported platform
#endif
  }

  std::vector<std::string>
  GetGatewaysNotOnInterface(std::string ifname)
  {
    std::vector<std::string> gateways;
#ifdef __linux__
    std::ifstream inf("/proc/net/route");
    for (std::string line; std::getline(inf, line);)
    {
      const auto parts = split(line, '\t');
      if (parts[1].find_first_not_of('0') == std::string::npos and parts[0] != ifname)
      {
        const auto& ip = parts[2];
        if ((ip.size() == sizeof(uint32_t) * 2) and lokimq::is_hex(ip))
        {
          huint32_t x{};
          lokimq::from_hex(ip.begin(), ip.end(), reinterpret_cast<char*>(&x.h));
          gateways.emplace_back(x.ToString());
        }
      }
    }

    return gateways;
#elif _WIN32
    ForEachWIN32Interface([&](auto w32interface) {
      struct in_addr gateway, interface_addr;
      gateway.S_un.S_addr = (u_long)w32interface->dwForwardDest;
      interface_addr.S_un.S_addr = (u_long)w32interface->dwForwardNextHop;
      std::array<char, 128> interface_str{};
      StringCchCopy(interface_str.data(), interface_str.size(), inet_ntoa(interface_addr));
      std::string interface_name{interface_str.data()};
      if ((!gateway.S_un.S_addr) and interface_name != ifname)
      {
        gateways.push_back(std::move(interface_name));
      }
    });
    return gateways;
#elif __APPLE__
    LogDebug("get gateways not on ", ifname);
    const auto maybe = GetIFAddr(ifname);
    if (not maybe.has_value())
      return gateways;
    const auto interface = maybe->toString();
    // mac os is so godawful man
    FILE* p = popen("/usr/sbin/netstat -rn -f inet", "r");
    if (p == nullptr)
    {
      return gateways;
    }
    char* line = nullptr;
    size_t len = 0;
    ssize_t read = 0;
    while ((read = getline(&line, &len, p)) != -1)
    {
      const std::string line_str(line, len);
      const auto parts = llarp::split_any(line_str, " "sv, true);
      if (parts[0] == "default" and parts[3] != ifname)
      {
        gateways.emplace_back(parts[1]);
      }
    }
    pclose(p);
    return gateways;
#else
#error unsupported platform
#endif
  }

}  // namespace llarp::net