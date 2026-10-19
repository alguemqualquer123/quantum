// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// Connections.cpp - see Connections.h.

#include "Connections.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#ifdef _WIN32
    // Same include order the serving loop uses: WIN32_LEAN_AND_MEAN is defined
    // for this target, and winsock2.h must precede windows.h so <winsock.h>
    // cannot redefine sockaddr (see tools/resource-server/ResourceServer.cpp).
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <iphlpapi.h>
    #include <psapi.h>
#endif

namespace doriax::rs {

unsigned long long ConnectionLog::add(ConnectionRecord record) {
    record.id = nextId_++;
    if (record.unixTime == 0) record.unixTime = static_cast<long long>(std::time(nullptr));
    lastId_ = record.id;
    ++accepted_;
    if (!record.ok) ++failures_;
    lastRequestUnix_ = record.unixTime;

    records_.push_back(std::move(record));
    if (records_.size() > kCapacity) records_.erase(records_.begin());
    return records_.back().id;
}

void ConnectionLog::clear() {
    records_.clear();
    accepted_ = 0;
    failures_ = 0;
    lastRequestUnix_ = 0;
}

std::vector<ClientGroup> ConnectionLog::clients() const {
    // Keyed by pid + peer host + self-reported client, so two processes on the
    // same loopback port range stay separate and an anonymous client still gets
    // its own row when the owner lookup fails.
    std::map<std::string, ClientGroup> groups;
    for (const ConnectionRecord& r : records_) {
        const std::string host = r.peer.substr(0, r.peer.find(':'));
        const std::string pidText = r.ownerPid ? std::to_string(r.ownerPid) : std::string();
        const std::string key = pidText + "|" + host + "|" + r.client + "|" + r.processName;

        ClientGroup& g = groups[key];
        if (g.requests == 0) {
            g.peerHost = host;
            g.pidText = pidText;
            g.processName = r.processName;
            g.client = r.client;
            g.firstSeenUnix = r.unixTime;
        }
        ++g.requests;
        if (!r.ok) ++g.failures;
        if (r.unixTime >= g.lastSeenUnix) {
            g.lastSeenUnix = r.unixTime;
            g.lastCommand = r.command;
        }
        // The owner lookup can succeed for a later request from the same peer.
        if (g.processName.empty() && !r.processName.empty()) g.processName = r.processName;
        if (g.client.empty() && !r.client.empty()) g.client = r.client;
    }

    std::vector<ClientGroup> out;
    out.reserve(groups.size());
    for (auto& kv : groups) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const ClientGroup& a, const ClientGroup& b) {
        return a.lastSeenUnix > b.lastSeenUnix;
    });
    return out;
}

#ifdef _WIN32

namespace {

bool peerPortAndAddress(const std::string& peer, int& portOut, unsigned long& addrOut) {
    const size_t colon = peer.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    const std::string host = peer.substr(0, colon);
    const std::string port = peer.substr(colon + 1);
    if (port.size() > 5) return false;
    for (char c : port) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    // Only IPv4 loopback is ever served, so a textual match is enough; parsing
    // the address keeps the comparison exact rather than cosmetic.
    in_addr parsed{};
    if (inet_pton(AF_INET, host.c_str(), &parsed) != 1) return false;
    addrOut = parsed.s_addr;
    portOut = std::atoi(port.c_str());
    return portOut > 0 && portOut <= 65535;
}

std::string fileNameFromPath(const std::string& path) {
    size_t start = 0;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '\\' || path[i] == '/') start = i + 1;
    }
    return path.substr(start);
}

}

bool queryProcessName(unsigned long long pid, std::string& nameOut) {
    nameOut.clear();
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process) return false;
    // QueryFullProcessImageNameA fails on paths that are not representable in
    // the ANSI code page, so the wide call plus an explicit conversion is used.
    wchar_t wide[MAX_PATH] = {0};
    DWORD wideSize = DWORD(MAX_PATH);
    const bool ok = QueryFullProcessImageNameW(process, 0, wide, &wideSize) != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide, int(wideSize), nullptr, 0, nullptr,
                                         nullptr);
    if (need <= 0) return false;
    std::string full(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, int(wideSize), full.data(), need, nullptr, nullptr);
    nameOut = fileNameFromPath(full);
    return !nameOut.empty();
}

bool queryConnectionOwner(const std::string& peer, int localPort,
                          unsigned long long& pidOut, std::string& nameOut) {
    pidOut = 0;
    nameOut.clear();
    int peerPort = 0;
    unsigned long peerAddr = 0;
    if (!peerPortAndAddress(peer, peerPort, peerAddr)) return false;

    DWORD family = AF_INET;
    DWORD size = 0;
    // The sizing call reports ERROR_INSUFFICIENT_BUFFER (not NO_ERROR) and
    // leaves the needed size behind; a second call reads the real table.
    (void)GetExtendedTcpTable(nullptr, &size, TRUE, family, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size < sizeof(MIB_TCPTABLE_OWNER_PID)) return false;
    std::vector<unsigned char> table(size);
    if (GetExtendedTcpTable(table.data(), &size, TRUE, family, TCP_TABLE_OWNER_PID_ALL, 0) !=
        NO_ERROR) {
        return false;
    }
    const auto* rows = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(table.data());
    const DWORD count = rows->dwNumEntries;
    // Both endpoints are 127.0.0.1, so only the ports tell the two halves of
    // the same connection apart: the client's own row has the peer port as its
    // LOCAL port and this server's port as its REMOTE port. Matching the other
    // way round would report the server process as its own client.
    const u_short wantLocal = htons(static_cast<unsigned short>(peerPort));
    const u_short wantRemote = htons(static_cast<unsigned short>(localPort));
    for (DWORD i = 0; i < count; ++i) {
        const MIB_TCPROW_OWNER_PID& row = rows->table[i];
        // Ports in this table are already in network byte order.
        if (row.dwLocalAddr == peerAddr && row.dwLocalPort == wantLocal &&
            row.dwRemotePort == wantRemote) {
            pidOut = row.dwOwningPid;
            queryProcessName(pidOut, nameOut);
            return true;
        }
    }
    return false;
}

#else

bool queryProcessName(unsigned long long, std::string&) { return false; }

bool queryConnectionOwner(const std::string&, int, unsigned long long&, std::string&) {
    return false;
}

#endif

}
