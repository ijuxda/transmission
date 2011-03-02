/******************************************************************************
 *
 * $Id$
 *
 * Copyright (c) Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include "net-interfaces.h"
#include "utils.h"
#include "list.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#if defined(HAVE_GETIFADDRS)
    #include <ifaddrs.h>
#endif

#include <errno.h>

#if defined(HAVE_GETIFADDRS)
    static tr_interface ** tr_net_getinterfaces(void);
#else
    static tr_interface ** tr_net_dummy_interfaces(void);
#endif

tr_interface * tr_FindInterfaceByName(tr_interface **interfaces, char * device)
{
    tr_interface * found = NULL;
    if (interfaces)
    {
        int entry;
        for(entry = 0; interfaces[entry]; entry++)
        {
            tr_interface * test = interfaces[entry];
            if( 0 == strcasecmp( test->name, device ) )
            {
                found = test;
                break;
            }
        }
    }
    return found;
}

void tr_interfacesFree( tr_interface ** interfaces )
{
    if (interfaces)
    {
        int entry;
        for(entry = 0; interfaces[entry]; entry++)
        {
            tr_free(interfaces[entry]);
        }
    }
    tr_free(interfaces);
}

tr_interface ** tr_net_interfaces()
{
#if defined(HAVE_GETIFADDRS)
    return tr_net_getinterfaces();
#else
    return tr_net_dummy_interfaces();
#endif
}

#if defined(HAVE_GETIFADDRS)
static void tr_MergeOrAppendToInterfaces(tr_interface **interfaces, struct ifaddrs * ifa);

static void tr_MergeOrAppendToInterfaces(tr_interface **interfaces, struct ifaddrs * ifa)
{
    if (interfaces)
    {
        tr_interface * merge = tr_FindInterfaceByName(interfaces, ifa->ifa_name);

        if (merge == NULL)
        {
            int entry;
            for(entry = 0; interfaces[entry]; entry++)
            {
            }
            interfaces[entry] = tr_new0(tr_interface, 1);
            merge = interfaces[entry];
            // Name.
            tr_strlcpy(merge->name, ifa->ifa_name, sizeof(merge->name));
        }

        if (merge)
        {
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in * s4 = (struct sockaddr_in *)(ifa->ifa_addr);

                merge->af4 = ifa->ifa_addr->sa_family;
                merge->ipv4.type = TR_AF_INET;
                merge->ipv4.addr.addr4 = s4->sin_addr;
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6 * s6 = (struct sockaddr_in6 *)(ifa->ifa_addr);

                merge->af6 = ifa->ifa_addr->sa_family;
                merge->ipv6.type = TR_AF_INET6;
                merge->ipv6.addr.addr6 = s6->sin6_addr;
            }
        }
    }
}

static tr_interface ** tr_net_getinterfaces(void)
{
	tr_interface ** interfaces = NULL;
	int ifcount = 0;

    struct ifaddrs *myaddrs, *ifa;
    int status;

    status = getifaddrs(&myaddrs);
    if (status != 0)
    {
        tr_err( _( "getifaddrs error: \'%s\' (%d)" ), tr_strerror(errno), errno );
    }

    for (ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (  (ifa->ifa_addr != NULL)    // has address
           && (ifa->ifa_flags & IFF_UP)) // iface is up.
        {
            ifcount++;
        }
    }

	if (ifcount > 0)
	{
	    // treat as a null terminated array of interfaces
        interfaces = tr_new0( tr_interface *, ifcount + 1 );
        for (ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (  (ifa->ifa_addr != NULL)    // has address
               && (ifa->ifa_flags & IFF_UP)) // iface is up.
            {
                tr_MergeOrAppendToInterfaces(interfaces, ifa);
            }
        }
	}
    freeifaddrs(myaddrs);

    return interfaces;
}

#else // HAVE_GETIFADDRS

tr_interface ** tr_net_dummy_interfaces(void)
{
    // Is there a port of getifaddrs for win32?
    return NULL;
}

#endif
