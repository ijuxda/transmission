/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#ifdef HAVE_LSEEK64
 #define _LARGEFILE64_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <stdlib.h> /* realloc */
#include <string.h> /* memcmp */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "transmission.h"
#include "cache.h"
#include "crypto.h"
#include "fdlimit.h"
#include "inout.h"
#include "peer-common.h" /* MAX_BLOCK_SIZE */
#include "platform.h"
#include "stats.h"
#include "torrent.h"
#include "utils.h"

/****
*****  Low-level IO functions
****/

#ifdef WIN32
 #if defined(read)
  #undef read
 #endif
 #define read  _read

 #if defined(write)
  #undef write
 #endif
 #define write _write
#endif

enum { TR_IO_READ, TR_IO_PREFETCH,
       /* Any operations that require write access must follow TR_IO_WRITE. */
       TR_IO_WRITE
};

/**
 * Set a torrent error if the operation given by @a mode
 * requires that the file exists.
 *
 * @return Zero on success otherwise an errno value.
 *
 * @note This function assumes that it has already been
 *       determined that the file does not exist in the
 *       filesystem.
 */
static int
checkOperation( tr_torrent * tor, const tr_file * file,
                const char * path, int mode )
{
    const char * fmt = _( "Expected file not found: %s" );

    if( mode == TR_IO_READ )
    {
        tr_torrentSetLocalError( tor, fmt, path );
        return ENOENT;
    }

    if( mode == TR_IO_PREFETCH )
    {
        /* Allow prefetch to fail silently. */
        return ENOENT;
    }

    assert( mode == TR_IO_WRITE );

    if( file->exists )
    {
        tr_torrentSetLocalError( tor, fmt, path );
        return ENOENT;
    }

    return 0;
}

/**
 * @return 0 on success, or an errno on failure.
 *
 * @note If @a tor->info.files[fileIndex]->usept is TRUE the IO
 *       operations will be carried out on a temporary piece file
 *       instead of the actual file. If you change this behavior,
 *       you also need to change code in setFileDND().
 *
 * @see setFileDND()
 */
static int
readOrWriteBytes( tr_session       * session,
                  tr_torrent       * tor,
                  int                ioMode,
                  tr_piece_index_t   pieceIndex,
                  uint32_t           pieceOffset,
                  tr_file_index_t    fileIndex,
                  uint64_t           fileOffset,
                  void             * buf,
                  size_t             buflen )
{
    const tr_info * info = &tor->info;
    const tr_file * file = &info->files[fileIndex];
    const tr_bool doWrite = ioMode >= TR_IO_WRITE;
    uint64_t offset;
    uint64_t desiredSize;
    uint32_t indexNum;
    tr_fd_index_type indexType;
    int fd = -1;
    int err = 0;

//if( doWrite )
//    fprintf( stderr, "in file %s at offset %zu, writing %zu bytes; file length is %zu\n", file->name, (size_t)fileOffset, buflen, (size_t)file->length );

    assert( fileIndex < info->fileCount );
    assert( !file->length || ( fileOffset < file->length ) );
    assert( fileOffset + buflen <= file->length );

    if( !file->length )
        return 0;

    if( file->usept )
    {
        offset = pieceOffset;
        desiredSize = tr_torPieceCountBytes( tor, pieceIndex );
        indexNum = pieceIndex;
        indexType = TR_FD_INDEX_PIECE;
    }
    else
    {
        offset = fileOffset;
        desiredSize = file->length;
        indexNum = fileIndex;
        indexType = TR_FD_INDEX_FILE;
    }

    fd = tr_fdFileGetCached( session, tr_torrentId( tor ),
                             indexNum, indexType, doWrite );

    if( fd < 0 )
    {
        /* the fd cache doesn't have this file...
         * we'll need to open it and maybe create it */
        char * subpath, * filename = NULL;
        const char * base;
        tr_bool fileExists;
        tr_preallocation_mode preallocationMode;

        if( file->usept )
        {
            fileExists = tr_torrentFindPieceTemp2( tor, pieceIndex,
                                                   &base, &subpath );
        }
        else
        {
            fileExists = tr_torrentFindFile2( tor, fileIndex,
                                              &base, &subpath );

            if( !fileExists )
            {
                base = tr_torrentGetCurrentDir( tor );

                if( tr_sessionIsIncompleteFileNamingEnabled( tor->session ) )
                    subpath = tr_torrentBuildPartial( tor, fileIndex );
                else
                    subpath = tr_strdup( file->name );
            }
        }

        if( ( file->dnd ) || ( ioMode < TR_IO_WRITE ) )
            preallocationMode = TR_PREALLOCATE_NONE;
        else
            preallocationMode = tor->session->preallocationMode;

        filename = tr_buildPath( base, subpath, NULL );
        if( !fileExists )
            err = checkOperation( tor, file, filename, ioMode );

        if( !err && ( fd = tr_fdFileCheckout( session, tor->uniqueId,
                                              indexNum, indexType,
                                              filename, doWrite,
                                              preallocationMode,
                                              desiredSize ) ) < 0 )
        {
            err = errno;
            tr_torerr( tor, "tr_fdFileCheckout failed for \"%s\": %s",
                       filename, tr_strerror( err ) );
        }

        if( doWrite && !err )
            tr_statsFileCreated( tor->session );

        tr_free( filename );
        tr_free( subpath );
    }
    else
    {
        struct stat sb;
        /* Check that the file corresponding to 'fd' still exists. */
        if( !fstat( fd, &sb ) && sb.st_nlink < 1 )
        {
            tr_torrentSetLocalError( tor,
                _( "File deleted while still in cache: %s" ),
                file->name );
            err = ENOENT;
        }
    }

    if( !err )
    {
        if( ioMode == TR_IO_READ ) {
            const int rc = tr_pread( fd, buf, buflen, offset );
            if( rc < 0 ) {
                err = errno;
                tr_torerr( tor, "read failed for \"%s\": %s",
                           file->name, tr_strerror( err ) );
            }
        } else if( ioMode == TR_IO_PREFETCH ) {
            const int rc = tr_prefetch( fd, offset, buflen );
            if( rc < 0 ) {
                /* (don't set "err" here... it's okay for prefetch to fail) */
                tr_tordbg( tor, "prefetch failed for \"%s\": %s",
                           file->name, tr_strerror( errno ) );
            }
        } else if( ioMode == TR_IO_WRITE ) {
            const int rc = tr_pwrite( fd, buf, buflen, offset );
            if( rc < 0 ) {
                err = errno;
                tr_torerr( tor, "write failed for \"%s\": %s",
                           file->name, tr_strerror( err ) );
            }
        } else {
            abort();
        }
    }

    return err;
}

static int
compareOffsetToFile( const void * a, const void * b )
{
    const uint64_t  offset = *(const uint64_t*)a;
    const tr_file * file = b;

    if( offset < file->offset ) return -1;
    if( offset >= file->offset + file->length ) return 1;
    return 0;
}

void
tr_ioFindFileLocation( const tr_torrent * tor,
                       tr_piece_index_t   pieceIndex,
                       uint32_t           pieceOffset,
                       tr_file_index_t  * fileIndex,
                       uint64_t         * fileOffset )
{
    const uint64_t  offset = tr_pieceOffset( tor, pieceIndex, pieceOffset, 0 );
    const tr_file * file;

    assert( tr_isTorrent( tor ) );
    assert( offset < tor->info.totalSize );

    file = bsearch( &offset,
                    tor->info.files, tor->info.fileCount, sizeof( tr_file ),
                    compareOffsetToFile );

    assert( file != NULL );

    *fileIndex = file - tor->info.files;
    *fileOffset = offset - file->offset;

    assert( *fileIndex < tor->info.fileCount );
    assert( *fileOffset < file->length );
    assert( tor->info.files[*fileIndex].offset + *fileOffset == offset );
}

/* returns 0 on success, or an errno on failure */
static int
readOrWritePiece( tr_torrent       * tor,
                  int                ioMode,
                  tr_piece_index_t   pieceIndex,
                  uint32_t           pieceOffset,
                  uint8_t          * buf,
                  size_t             buflen )
{
    int             err = 0;
    tr_file_index_t fileIndex;
    uint64_t        fileOffset;
    const tr_info * info = &tor->info;

    if( pieceIndex >= tor->info.pieceCount )
        return EINVAL;
    //if( pieceOffset + buflen > tr_torPieceCountBytes( tor, pieceIndex ) )
    //    return EINVAL;

    tr_ioFindFileLocation( tor, pieceIndex, pieceOffset,
                           &fileIndex, &fileOffset );

    while( buflen && !err )
    {
        uint32_t leftInPiece;
        uint32_t bytesThisPass;
        uint64_t leftInFile;
        const tr_file * file = &info->files[fileIndex];

        leftInPiece = tr_torPieceCountBytes( tor, pieceIndex ) - pieceOffset;
        leftInFile = file->length - fileOffset;
        bytesThisPass = MIN( leftInFile, leftInPiece );
        bytesThisPass = MIN( bytesThisPass, buflen );

        err = readOrWriteBytes( tor->session, tor, ioMode,
                                pieceIndex, pieceOffset,
                                fileIndex, fileOffset,
                                buf, bytesThisPass );
        buf += bytesThisPass;
        buflen -= bytesThisPass;
        leftInPiece -= bytesThisPass;
        leftInFile -= bytesThisPass;
        pieceOffset += bytesThisPass;
        fileOffset += bytesThisPass;

        if( leftInPiece == 0 )
        {
            ++pieceIndex;
            pieceOffset = 0;
        }
        if( leftInFile == 0 )
        {
            ++fileIndex;
            fileOffset = 0;
        }

        if( err != 0 && ioMode != TR_IO_PREFETCH && tor->error != TR_STAT_LOCAL_ERROR )
        {
            char * path = tr_buildPath( tor->downloadDir, file->name, NULL );
            tr_torrentSetLocalError( tor, "%s (%s)", tr_strerror( err ), path );
            tr_free( path );
        }
    }

    return err;
}

int
tr_ioRead( tr_torrent       * tor,
           tr_piece_index_t   pieceIndex,
           uint32_t           begin,
           uint32_t           len,
           uint8_t          * buf )
{
    return readOrWritePiece( tor, TR_IO_READ, pieceIndex, begin, buf, len );
}

int
tr_ioPrefetch( tr_torrent       * tor,
               tr_piece_index_t   pieceIndex,
               uint32_t           begin,
               uint32_t           len)
{
    return readOrWritePiece( tor, TR_IO_PREFETCH, pieceIndex, begin,
                             NULL, len );
}

int
tr_ioWrite( tr_torrent       * tor,
            tr_piece_index_t   pieceIndex,
            uint32_t           begin,
            uint32_t           len,
            const uint8_t    * buf )
{
    return readOrWritePiece( tor, TR_IO_WRITE, pieceIndex, begin,
                             (uint8_t*)buf,
                             len );
}

/****
*****
****/

static tr_bool
recalculateHash( tr_torrent       * tor,
                 tr_piece_index_t   pieceIndex,
                 uint8_t          * setme )
{
    size_t   bytesLeft;
    uint32_t offset = 0;
    tr_bool  success = TRUE;
    const size_t buflen = tor->blockSize;
    void * buffer = tr_valloc( buflen );
    SHA_CTX  sha;

    assert( tor != NULL );
    assert( pieceIndex < tor->info.pieceCount );
    assert( buffer != NULL );
    assert( buflen > 0 );
    assert( setme != NULL );

    SHA1_Init( &sha );
    bytesLeft = tr_torPieceCountBytes( tor, pieceIndex );

    tr_ioPrefetch( tor, pieceIndex, offset, bytesLeft );

    while( bytesLeft )
    {
        const int len = MIN( bytesLeft, buflen );
        success = !tr_cacheReadBlock( tor->session->cache, tor, pieceIndex, offset, len, buffer );
        if( !success )
            break;
        SHA1_Update( &sha, buffer, len );
        offset += len;
        bytesLeft -= len;
    }

    if( success )
        SHA1_Final( setme, &sha );

    tr_free( buffer );
    return success;
}

tr_bool
tr_ioTestPiece( tr_torrent * tor, tr_piece_index_t piece )
{
    uint8_t hash[SHA_DIGEST_LENGTH];

    return recalculateHash( tor, piece, hash )
           && !memcmp( hash, tor->info.pieces[piece].hash, SHA_DIGEST_LENGTH );
}
