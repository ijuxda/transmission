/*
Copyright (c) 2010 by David Artoise Ijux

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <assert.h>
#include <string.h>

#include <event.h>

#include "transmission.h"
#include "session.h"
#include "peer-io.h"
#include "peer-proxy.h"
#include "utils.h"


static void
writeProxyRequestHTTP( tr_peerIo * io )
{
    static const int http_minor_version = 1;
    const tr_session * session = tr_peerIoGetSession( io );
    char buf[1024], host_hdr[256], auth_hdr[256], peer[64];
    const tr_address * peerAddr;
    tr_port peerPort;
    int len;

    if( http_minor_version > 0 )
    {
        const char *proxy = tr_sessionGetPeerProxy( session );
        int proxyPort = tr_sessionGetPeerProxyPort( session );
        tr_snprintf( host_hdr, sizeof( host_hdr ), "Host: %s:%d\015\012",
                     proxy, proxyPort );
    }
    else
        host_hdr[0] = '\0';

    if( tr_sessionIsPeerProxyAuthEnabled( session ) )
    {
        char auth[128], *enc;
        tr_snprintf( auth, sizeof( auth ), "%s:%s",
                     tr_sessionGetPeerProxyUsername( session ),
                     tr_sessionGetPeerProxyPassword( session ) );
        enc = tr_base64_encode( auth, -1, NULL );
        tr_snprintf( auth_hdr, sizeof( auth_hdr ),
                     "Proxy-Authorization: Basic %s\015\012",
                     enc );
        tr_free( enc );
    }
    else
        auth_hdr[0] = '\0';

    peerAddr = tr_peerIoGetAddress( io, &peerPort );
    tr_ntop( peerAddr, peer, sizeof( peer ) );

    len = tr_snprintf( buf, sizeof( buf ),
                       "CONNECT %s:%d HTTP/1.%d\015\012%s%s\015\012",
                       peer, peerPort, http_minor_version,
                       host_hdr, auth_hdr );

    tr_peerIoWrite( io, buf, len, FALSE );
    io->proxyStatus = PEER_PROXY_CONNECT;
}

static void
writeProxyRequestSOCKS4( tr_peerIo * io )
{
    const tr_session * session = tr_peerIoGetSession( io );
    uint8_t version, command, null;
    tr_port port;
    const tr_address * addr;

    version = 4;
    command = 1;
    addr = tr_peerIoGetAddress( io, &port );
    null = 0;

    assert( addr->type == TR_AF_INET );

    tr_peerIoWrite( io, &version, 1, FALSE );
    tr_peerIoWrite( io, &command, 1, FALSE );
    tr_peerIoWrite( io, &port, 2, FALSE );
    tr_peerIoWrite( io, &addr->addr.addr4.s_addr, 4, FALSE );

    if( tr_sessionIsPeerProxyAuthEnabled( session ) )
    {
        const char * username = tr_sessionGetPeerProxyUsername( session );
        size_t len = strlen( username );
        tr_peerIoWrite( io, username, len, FALSE );
    }

    tr_peerIoWrite( io, &null, 1, FALSE );
    io->proxyStatus = PEER_PROXY_CONNECT;
}

static void
writeProxyRequestSOCKS5( tr_peerIo * io )
{
    const tr_session * session = tr_peerIoGetSession( io );

    if ( tr_sessionIsPeerProxyAuthEnabled( session ) )
    {
        uint8_t packet[4] = { 5, 2, 0x00, 0x02 };
        tr_peerIoWrite( io, packet, sizeof(packet), FALSE );
    }
    else
    {
        uint8_t packet[3] = { 5, 1, 0x00 };
        tr_peerIoWrite( io, packet, sizeof(packet), FALSE );
    }

    io->proxyStatus = PEER_PROXY_INIT;
}

void
tr_peerIoWriteProxyRequest( tr_peerIo * io )
{
    assert( io->isIncoming == FALSE );
    assert( tr_peerIoIsProxied( io ) == TRUE );
    assert( io->session != NULL );
    assert( io->encryptionMode == PEER_ENCRYPTION_NONE );

    switch( tr_sessionGetPeerProxyType( io->session ) )
    {
        case TR_PROXY_HTTP:
            writeProxyRequestHTTP( io );
            break;
        case TR_PROXY_SOCKS4:
            writeProxyRequestSOCKS4( io );
            break;
        case TR_PROXY_SOCKS5:
            writeProxyRequestSOCKS5( io );
            break;
    }
}

static int
readProxyResponseHTTP( tr_peerIo * io, struct evbuffer * inbuf )
{
    const void * data = EVBUFFER_DATA( inbuf );
    size_t datalen = EVBUFFER_LENGTH( inbuf );
    const char * eom = tr_memmem( data, datalen, "\015\012\015\012", 4 );
    char * line;
    tr_bool success;

    if( eom == NULL )
        return READ_LATER;

    line = evbuffer_readline( inbuf );
    if( line == NULL )
        return READ_ERR;
    success = ( strstr( line, " 200 " ) != NULL );
    tr_free( line );
    evbuffer_drain( inbuf, EVBUFFER_LENGTH( inbuf ) );

    if (success)
    {
        io->proxyStatus = PEER_PROXY_ESTABLISHED;
        return READ_NOW;
    }

    return READ_ERR;
}

static int
readProxyResponseSOCKS4( tr_peerIo * io, struct evbuffer * inbuf )
{
    if( EVBUFFER_LENGTH( inbuf ) < 8 )
        return READ_LATER;
    if( EVBUFFER_DATA( inbuf )[1] != 90 )
        return READ_ERR;
    evbuffer_drain( inbuf, 8 );
    io->proxyStatus = PEER_PROXY_ESTABLISHED;
    return READ_NOW;
}

static void
writeSOCKS5ConnectCommand( tr_peerIo * io )
{
    const tr_address * addr;
    tr_port port;
    uint8_t version, command, reserved, address_type;

    addr = tr_peerIoGetAddress( io, &port );

    version = 5;
    command = 1;
    reserved = 0;
    tr_peerIoWrite( io, &version, 1, FALSE );
    tr_peerIoWrite( io, &command, 1, FALSE );
    tr_peerIoWrite( io, &reserved, 1, FALSE );

    if( addr->type == TR_AF_INET6 )
    {
        address_type = 4;
        tr_peerIoWrite( io, &address_type, 1, FALSE );
        tr_peerIoWrite( io, &addr->addr.addr6, 16, FALSE );
    }
    else
    {
        assert( addr->type == TR_AF_INET );
        address_type = 1;
        tr_peerIoWrite( io, &address_type, 1, FALSE );
        tr_peerIoWrite( io, &addr->addr.addr4.s_addr, 4, FALSE );
    }
    tr_peerIoWrite( io, &port, 2, FALSE );

    io->proxyStatus = PEER_PROXY_CONNECT;
}

static int
processSOCKS5Greeting( tr_peerIo * io, struct evbuffer * inbuf )
{
    const tr_session * session = tr_peerIoGetSession( io );
    const tr_bool auth_enabled = tr_sessionIsPeerProxyAuthEnabled( session );
    uint8_t auth_method;

    if( EVBUFFER_LENGTH( inbuf ) < 2 )
        return READ_LATER;

    auth_method = EVBUFFER_DATA( inbuf )[1];
    evbuffer_drain( inbuf, 2 );

    if( auth_method != 0x00 && auth_method != 0x02 )
        return READ_ERR;
    if( auth_method == 0x02 && !auth_enabled )
        return READ_ERR;

    if( auth_method == 0x02 )
    {
        uint8_t version, length;
        const char *username = tr_sessionGetPeerProxyUsername( session );
        const char *password = tr_sessionGetPeerProxyPassword( session );
        version = 5;
        tr_peerIoWrite( io, &version, 1, FALSE );
        length = MAX( strlen( username ), 255 );
        tr_peerIoWrite( io, &length, 1, FALSE );
        tr_peerIoWrite( io, username, length, FALSE );
        length = MAX( strlen( password ), 255 );
        tr_peerIoWrite( io, &length, 1, FALSE );
        tr_peerIoWrite( io, password, length, FALSE );

        io->proxyStatus = PEER_PROXY_AUTH;
        return READ_LATER;
    }

    writeSOCKS5ConnectCommand( io );
    return READ_LATER;
}

static int
processSOCKS5AuthResponse( tr_peerIo * io, struct evbuffer * inbuf )
{
    uint8_t status;
    if( EVBUFFER_LENGTH( inbuf ) < 2 )
        return READ_LATER;

    status = EVBUFFER_DATA( inbuf )[1];
    evbuffer_drain( inbuf, 2 );

    if( status != 0 )
        return READ_ERR;

    writeSOCKS5ConnectCommand( io );
    return READ_LATER;
}

static int
processSOCKS5CmdResponse( tr_peerIo * io, struct evbuffer * inbuf )
{
    uint8_t status, address_type;

    if( EVBUFFER_LENGTH( inbuf ) < 4 )
        return READ_LATER;

    status = EVBUFFER_DATA( inbuf )[1];
    address_type = EVBUFFER_DATA( inbuf )[3];
    evbuffer_drain( inbuf, 4 );

    if( status != 0 )
        return READ_ERR;

    if( address_type == 1 )
        evbuffer_drain( inbuf, 4 + 2 );
    else if( address_type == 4 )
        evbuffer_drain( inbuf, 16 + 2 );
    else
        return READ_ERR;

    io->proxyStatus = PEER_PROXY_ESTABLISHED;
    return READ_NOW;
}

static int
readProxyResponseSOCKS5( tr_peerIo * io, struct evbuffer * inbuf )
{
    switch( io->proxyStatus )
    {
        case PEER_PROXY_INIT:
            return processSOCKS5Greeting( io, inbuf );
        case PEER_PROXY_AUTH:
            return processSOCKS5AuthResponse( io, inbuf );
        case PEER_PROXY_CONNECT:
            return processSOCKS5CmdResponse( io, inbuf );
        default:
            break;
    }
    return READ_ERR;
}

/**
 * @brief Reads and removes the proxy response from the buffer
 * @return Returns READ_NOW if the proxy request succeeded and
 * and the connection is now ready to be used for peer communication,
 * READ_LATER if the buffer does not yet contain the complete
 * response, or READ_ERR if an error occured.
 * @note The proxy's complete response is removed from the buffer.
 */
int
tr_peerIoReadProxyResponse( tr_peerIo * io, struct evbuffer * inbuf )
{
    assert( io->isIncoming == FALSE );
    assert( tr_peerIoIsProxied( io ) == TRUE );
    assert( io->session != NULL );
    assert( io->encryptionMode == PEER_ENCRYPTION_NONE );

    switch( tr_sessionGetPeerProxyType( io->session ) )
    {
        case TR_PROXY_HTTP:
            return readProxyResponseHTTP( io, inbuf );
        case TR_PROXY_SOCKS4:
            return readProxyResponseSOCKS4( io, inbuf );
        case TR_PROXY_SOCKS5:
            return readProxyResponseSOCKS5( io, inbuf );
    }

    return READ_ERR;
}
