#pragma once

// This module provides basic functions for operating on IPv4 addresses and
// subnets.

#include <stdbool.h>
#include <stdint.h>

// IP address stored in network order (big endian)
typedef uint32_t ip4Addr;

// Converts a string in dot-decimal notation into an IPv4 address. Returns true
// on success or false if the string did not contain a valid address.
bool ip4GetAddr(const char* str, ip4Addr* addr);

// Size of a buffer required to hold a NUL-terminated IPv4 address string
#define IP4_ADDR_BUFLEN (3*4 + 3 + 1)

// Converts an IP address to a dot-decimal notation string. buffer must be at
// least IP4_ADDR_BUFLEN long. Returns 0 on success or an error code otherwise.
int ip4AddrToString(ip4Addr addr, char* buffer);

typedef struct {
	ip4Addr addr;
	uint8_t prefixLen;
} ip4Subnet;

// Converts a string in CIDR notation into an IPv4 subnet. Returns true on
// success or false if the string did not contain a valid subnet.
bool ip4GetSubnet(const char* str, ip4Subnet* subnet);

// Returns a subnet's address mask in network order (big endian)
ip4Addr ip4SubnetMask(const ip4Subnet* subnet);

// Returns the host mask, which is the negation of the subnet mask.
ip4Addr ip4HostMask(const ip4Subnet* subnet);

// Returns the first IP address in a subnet.
ip4Addr ip4SubnetStart(const ip4Subnet* subnet);

// Returns the last IP address in a subnet.
ip4Addr ip4SubnetEnd(const ip4Subnet* subnet);

// Returns the number of IP addresses in a subnet. This does not exclude
// reserved addresses within the subnet.
uint32_t ip4SubnetSize(const ip4Subnet* subnet);

// Size of a buffer required to hold a NUL-terminated subnet (CIDR notation)
#define IP4_CIDR_BUFLEN (IP4_ADDR_BUFLEN + 1 + 2 + 1)

// Converts a subnet to a CIDR notation string. buffer must be at least
// IP4_CIDR_BUFLEN long. Returns 0 on success or an error code otherwise.
int ip4SubnetToString(const ip4Subnet* subnet, char* buffer);

typedef struct ip4Iter_s ip4Iter;

// Creates an iterator that enumerates IP addresses within a subnet. All
// addresses in the "avoidSubnets" subnets will be skipped. avoidSubnets is
// either NULL or a NULL-terminated list.
ip4Iter* ip4NewIter(const ip4Subnet* subnet, const ip4Subnet** avoidSubnets);

// Advances the iterator. Returns true if a new address was retrieved, or false
// if the subnet has been exhausted. Retrieve the address using ip4IterAddr.
bool ip4IterNext(ip4Iter* it);

// Returns the current address for the iterator. ip4IterNext must be called at
// least once before calling this function.
ip4Addr ip4IterAddr(const ip4Iter* it);

void ip4FreeIter(ip4Iter* it);

typedef struct ip4FragIter_s ip4FragIter;

// Splits a large subnet into several smaller subnets, and returns an iterator
// for these "fragments". subnet is broken into fragmentCount pieces. If
// fragmentCount is a power of 2, then each fragment will have the same size.
// Otherwise, the largest fragments will be at most twice as large as the
// smallest fragments. The function makes no guarantees about which fragments
// will be larger (in terms of index). If the subnet cannot be broken into the
// requested number of fragments, NULL is returned.
ip4FragIter* ip4FragmentSubnet(const ip4Subnet* subnet, uint32_t fragmentCount);

// Advances the iterator. Returns true if a new fragment was retrieved, or false
// if the fragments have been exhausted. Retrieve the fragment subnet using
// ip4FragIterSubnet.
bool ip4FragIterNext(ip4FragIter* it);

// Returns the subnet for the current fragment for the iterator. ip4FragIterNext
// must be called at least once before calling this function.
void ip4FragIterSubnet(const ip4FragIter* it, ip4Subnet* fragment);

void ip4FreeFragIter(ip4FragIter* it);
