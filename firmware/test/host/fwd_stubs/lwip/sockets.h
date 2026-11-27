#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

// lwIP's reentrant form, which glibc doesn't have.
static inline char *inet_ntoa_r(struct in_addr a, char *buf, int len)
{
    return (char *)inet_ntop(AF_INET, &a, buf, len);
}
