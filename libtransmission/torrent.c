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

#include <sys/types.h> /* stat */
#include <sys/stat.h> /* stat */
#ifndef WIN32
 #include <sys/wait.h> /* wait() */
#else
 #include <process.h>
 #define waitpid(pid, status, options)	_cwait(status, pid, WAIT_CHILD)
#endif
#include <unistd.h> /* stat */
#include <dirent.h>

#include <assert.h>
#include <limits.h> /* INT_MAX */
#include <math.h>
#include <stdarg.h>
#include <string.h> /* memcmp */
#include <stdlib.h> /* qsort */

#include <event2/util.h> /* evutil_vsnprintf() */

#include "transmission.h"
#include "announcer.h"
#include "bandwidth.h"
#include "bencode.h"
#include "cache.h"
#include "completion.h"
#include "crypto.h" /* for tr_sha1 */
#include "resume.h"
#include "fdlimit.h" /* tr_fdTorrentClose */
#include "inout.h" /* tr_ioTestPiece() */
#include "list.h"
#include "magnet.h"
#include "metainfo.h"
#include "peer-common.h" /* MAX_BLOCK_SIZE */
#include "peer-mgr.h"
#include "platform.h" /* TR_PATH_DELIMITER_STR */
#include "ptrarray.h"
#include "session.h"
#include "torrent.h"
#include "torrent-magnet.h"
#include "trevent.h" /* tr_runInEventThread() */
#include "utils.h"
#include "verify.h"
#include "version.h"

/***
****
***/

#define tr_deeplog_tor( tor, ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, tr_torrentName( tor ), __VA_ARGS__ ); \
    } while( 0 )

/***
****
***/

const char *
tr_torrentName( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->info.rename ? tor->info.rename : tor->info.name;
}

int
tr_torrentId( const tr_torrent * tor )
{
    return tor->uniqueId;
}

tr_torrent*
tr_torrentFindFromId( tr_session * session, int id )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( tor->uniqueId == id )
            return tor;

    return NULL;
}

tr_torrent*
tr_torrentFindFromHashString( tr_session *  session, const char * str )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( !strcasecmp( str, tor->info.hashString ) )
            return tor;

    return NULL;
}

tr_torrent*
tr_torrentFindFromHash( tr_session * session, const uint8_t * torrentHash )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( *tor->info.hash == *torrentHash )
            if( !memcmp( tor->info.hash, torrentHash, SHA_DIGEST_LENGTH ) )
                return tor;

    return NULL;
}

tr_torrent*
tr_torrentFindFromMagnetLink( tr_session * session, const char * magnet )
{
    tr_magnet_info * info;
    tr_torrent * tor = NULL;

    if(( info = tr_magnetParse( magnet )))
    {
        tor = tr_torrentFindFromHash( session, info->hash );
        tr_magnetFree( info );
    }

    return tor;
}

tr_torrent*
tr_torrentFindFromObfuscatedHash( tr_session * session,
                                  const uint8_t * obfuscatedTorrentHash )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( session, tor )))
        if( !memcmp( tor->obfuscatedHash, obfuscatedTorrentHash,
                     SHA_DIGEST_LENGTH ) )
            return tor;

    return NULL;
}

tr_bool
tr_torrentIsPieceTransferAllowed( const tr_torrent  * tor,
                                  tr_direction        direction )
{
    int limit;
    tr_bool allowed = TRUE;

    if( tr_torrentUsesSpeedLimit( tor, direction ) )
        if( tr_torrentGetSpeedLimit_Bps( tor, direction ) <= 0 )
            allowed = FALSE;

    if( tr_torrentUsesSessionLimits( tor ) )
        if( tr_sessionGetActiveSpeedLimit_Bps( tor->session, direction, &limit ) )
            if( limit <= 0 )
                allowed = FALSE;

    return allowed;
}

/***
****  PER-TORRENT UL / DL SPEEDS
***/

void
tr_torrentSetSpeedLimit_Bps( tr_torrent * tor, tr_direction dir, int Bps )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );
    assert( Bps >= 0 );

    if( tr_bandwidthSetDesiredSpeed_Bps( tor->bandwidth, dir, Bps ) )
        tr_torrentSetDirty( tor );
}
void
tr_torrentSetSpeedLimit_KBps( tr_torrent * tor, tr_direction dir, int KBps )
{
    tr_torrentSetSpeedLimit_Bps( tor, dir, toSpeedBytes( KBps ) );
}

int
tr_torrentGetSpeedLimit_Bps( const tr_torrent * tor, tr_direction dir )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );

    return tr_bandwidthGetDesiredSpeed_Bps( tor->bandwidth, dir );
}
int
tr_torrentGetSpeedLimit_KBps( const tr_torrent * tor, tr_direction dir )
{
    return toSpeedKBps( tr_torrentGetSpeedLimit_Bps( tor, dir ) );
}

void
tr_torrentUseSpeedLimit( tr_torrent * tor, tr_direction dir, tr_bool do_use )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );

    if( tr_bandwidthSetLimited( tor->bandwidth, dir, do_use ) )
        tr_torrentSetDirty( tor );
}

tr_bool
tr_torrentUsesSpeedLimit( const tr_torrent * tor, tr_direction dir )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isDirection( dir ) );

    return tr_bandwidthIsLimited( tor->bandwidth, dir );
}

void
tr_torrentUseSessionLimits( tr_torrent * tor, tr_bool doUse )
{
    tr_bool changed;

    assert( tr_isTorrent( tor ) );

    changed = tr_bandwidthHonorParentLimits( tor->bandwidth, TR_UP, doUse );
    changed |= tr_bandwidthHonorParentLimits( tor->bandwidth, TR_DOWN, doUse );

    if( changed )
        tr_torrentSetDirty( tor );
}

tr_bool
tr_torrentUsesSessionLimits( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tr_bandwidthAreParentLimitsHonored( tor->bandwidth, TR_UP );
}

/***
****
***/

void
tr_torrentSetRatioMode( tr_torrent *  tor, tr_ratiolimit mode )
{
    assert( tr_isTorrent( tor ) );
    assert( mode==TR_RATIOLIMIT_GLOBAL || mode==TR_RATIOLIMIT_SINGLE || mode==TR_RATIOLIMIT_UNLIMITED  );

    if( mode != tor->ratioLimitMode )
    {
        tor->ratioLimitMode = mode;

        tr_torrentSetDirty( tor );
    }
}

tr_ratiolimit
tr_torrentGetRatioMode( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->ratioLimitMode;
}

void
tr_torrentSetRatioLimit( tr_torrent * tor, double desiredRatio )
{
    assert( tr_isTorrent( tor ) );

    if( (int)(desiredRatio*100.0) != (int)(tor->desiredRatio*100.0) )
    {
        tor->desiredRatio = desiredRatio;

        tr_torrentSetDirty( tor );
    }
}

double
tr_torrentGetRatioLimit( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->desiredRatio;
}

tr_bool
tr_torrentGetSeedRatio( const tr_torrent * tor, double * ratio )
{
    tr_bool isLimited;

    switch( tr_torrentGetRatioMode( tor ) )
    {
        case TR_RATIOLIMIT_SINGLE:
            isLimited = TRUE;
            if( ratio )
                *ratio = tr_torrentGetRatioLimit( tor );
            break;

        case TR_RATIOLIMIT_GLOBAL:
            isLimited = tr_sessionIsRatioLimited( tor->session );
            if( isLimited && ratio )
                *ratio = tr_sessionGetRatioLimit( tor->session );
            break;

        default: /* TR_RATIOLIMIT_UNLIMITED */
            isLimited = FALSE;
            break;
    }

    return isLimited;
}

/* returns true if the seed ratio applies --
 * it applies if the torrent's a seed AND it has a seed ratio set */
static tr_bool
tr_torrentGetSeedRatioBytes( tr_torrent  * tor,
                             uint64_t    * setmeLeft,
                             uint64_t    * setmeGoal )
{
    double seedRatio;
    tr_bool seedRatioApplies = FALSE;

    if( tr_torrentGetSeedRatio( tor, &seedRatio ) )
    {
        const uint64_t u = tor->uploadedCur + tor->uploadedPrev;
        const uint64_t d = tor->downloadedCur + tor->downloadedPrev;
        const uint64_t baseline = d ? d : tr_cpSizeWhenDone( &tor->completion );
        const uint64_t goal = baseline * seedRatio;
        if( setmeLeft ) *setmeLeft = goal > u ? goal - u : 0;
        if( setmeGoal ) *setmeGoal = goal;
        seedRatioApplies = tr_torrentIsSeed( tor );
    }

    return seedRatioApplies;
}

static tr_bool
tr_torrentIsSeedRatioDone( tr_torrent * tor )
{
    uint64_t bytesLeft;
    return tr_torrentGetSeedRatioBytes( tor, &bytesLeft, NULL ) && !bytesLeft;
}

/***
****
***/

void
tr_torrentSetIdleMode( tr_torrent *  tor, tr_idlelimit mode )
{
    assert( tr_isTorrent( tor ) );
    assert( mode==TR_IDLELIMIT_GLOBAL || mode==TR_IDLELIMIT_SINGLE || mode==TR_IDLELIMIT_UNLIMITED  );

    if( mode != tor->idleLimitMode )
    {
        tor->idleLimitMode = mode;

        tr_torrentSetDirty( tor );
    }
}

tr_idlelimit
tr_torrentGetIdleMode( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->idleLimitMode;
}

void
tr_torrentSetIdleLimit( tr_torrent * tor, uint16_t idleMinutes )
{
    assert( tr_isTorrent( tor ) );

    if( idleMinutes > 0 )
    {
        tor->idleLimitMinutes = idleMinutes;

        tr_torrentSetDirty( tor );
    }
}

uint16_t
tr_torrentGetIdleLimit( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->idleLimitMinutes;
}

tr_bool
tr_torrentGetSeedIdle( const tr_torrent * tor, uint16_t * idleMinutes )
{
    tr_bool isLimited;

    switch( tr_torrentGetIdleMode( tor ) )
    {
        case TR_IDLELIMIT_SINGLE:
            isLimited = TRUE;
            if( idleMinutes )
                *idleMinutes = tr_torrentGetIdleLimit( tor );
            break;

        case TR_IDLELIMIT_GLOBAL:
            isLimited = tr_sessionIsIdleLimited( tor->session );
            if( isLimited && idleMinutes )
                *idleMinutes = tr_sessionGetIdleLimit( tor->session );
            break;

        default: /* TR_IDLELIMIT_UNLIMITED */
            isLimited = FALSE;
            break;
    }

    return isLimited;
}

static tr_bool
tr_torrentIsSeedIdleLimitDone( tr_torrent * tor )
{
    uint16_t idleMinutes;
    return tr_torrentGetSeedIdle( tor, &idleMinutes )
        && difftime(tr_time(), MAX(tor->startDate, tor->activityDate)) >= idleMinutes * 60u;
}

/***
****
***/

void
tr_torrentCheckSeedLimit( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( !tor->isRunning || !tr_torrentIsSeed( tor ) )
        return;

    /* if we're seeding and reach our seed ratio limit, stop the torrent */
    if( tr_torrentIsSeedRatioDone( tor ) )
    {
        tr_torinf( tor, "Seed ratio reached; pausing torrent" );

        tor->isStopping = TRUE;

        /* maybe notify the client */
        if( tor->ratio_limit_hit_func != NULL )
            tor->ratio_limit_hit_func( tor, tor->ratio_limit_hit_func_user_data );
    }
    /* if we're seeding and reach our inactiviy limit, stop the torrent */
    else if( tr_torrentIsSeedIdleLimitDone( tor ) )
    {
        tr_torinf( tor, "Seeding idle limit reached; pausing torrent" );

        tor->isStopping = TRUE;
        tor->finishedSeedingByIdle = TRUE;

        /* maybe notify the client */
        if( tor->idle_limit_hit_func != NULL )
            tor->idle_limit_hit_func( tor, tor->idle_limit_hit_func_user_data );
    }
}

/***
****
***/

void
tr_torrentSetLocalError( tr_torrent * tor, const char * fmt, ... )
{
    va_list ap;

    assert( tr_isTorrent( tor ) );

    va_start( ap, fmt );
    tor->error = TR_STAT_LOCAL_ERROR;
    tor->errorTracker[0] = '\0';
    evutil_vsnprintf( tor->errorString, sizeof( tor->errorString ), fmt, ap );
    va_end( ap );

    tr_torerr( tor, "%s", tor->errorString );

    if( tor->isRunning )
        tor->isStopping = TRUE;
}

static void
tr_torrentClearError( tr_torrent * tor )
{
    tor->error = TR_STAT_OK;
    tor->errorString[0] = '\0';
    tor->errorTracker[0] = '\0';
}

static void
onTrackerResponse( tr_torrent * tor, const tr_tracker_event * event, void * unused UNUSED )
{
    switch( event->messageType )
    {
        case TR_TRACKER_PEERS:
        {
            size_t i;
            const int8_t seedProbability = event->seedProbability;
            const tr_bool allAreSeeds = seedProbability == 100;

             if( allAreSeeds )
                tr_tordbg( tor, "Got %zu seeds from tracker", event->pexCount );
            else
                tr_tordbg( tor, "Got %zu peers from tracker", event->pexCount );

            for( i = 0; i < event->pexCount; ++i )
                tr_peerMgrAddPex( tor, TR_PEER_FROM_TRACKER, &event->pex[i], seedProbability );

            if( allAreSeeds && tr_torrentIsPrivate( tor ) )
                tr_peerMgrMarkAllAsSeeds( tor );

            break;
        }

        case TR_TRACKER_WARNING:
            tr_torerr( tor, _( "Tracker warning: \"%s\"" ), event->text );
            tor->error = TR_STAT_TRACKER_WARNING;
            tr_strlcpy( tor->errorTracker, event->tracker, sizeof( tor->errorTracker ) );
            tr_strlcpy( tor->errorString, event->text, sizeof( tor->errorString ) );
            break;

        case TR_TRACKER_ERROR:
            tr_torerr( tor, _( "Tracker error: \"%s\"" ), event->text );
            tor->error = TR_STAT_TRACKER_ERROR;
            tr_strlcpy( tor->errorTracker, event->tracker, sizeof( tor->errorTracker ) );
            tr_strlcpy( tor->errorString, event->text, sizeof( tor->errorString ) );
            break;

        case TR_TRACKER_ERROR_CLEAR:
            if( tor->error != TR_STAT_LOCAL_ERROR )
                tr_torrentClearError( tor );
            break;
    }
}

/***
****
****  TORRENT INSTANTIATION
****
***/

static void
initFilePieces( tr_torrent * tor, tr_file_index_t fileIndex )
{
    tr_info * info = &tor->info;
    tr_file * file;
    uint64_t  firstByte, lastByte;

    assert( info );
    assert( fileIndex < info->fileCount );

    file = &info->files[fileIndex];
    firstByte = file->offset;
    lastByte = firstByte + ( file->length ? file->length - 1 : 0 );
    file->firstPiece = tr_torBytePiece( tor, firstByte );
    file->lastPiece = tr_torBytePiece( tor, lastByte );
}

static int
pieceHasFile( tr_piece_index_t piece,
              const tr_file *  file )
{
    return ( file->firstPiece <= piece ) && ( piece <= file->lastPiece );
}

static tr_priority_t
calculatePiecePriority( const tr_torrent * tor,
                        tr_piece_index_t   piece,
                        int                fileHint )
{
    tr_file_index_t i;
    tr_priority_t priority = TR_PRI_LOW;

    /* find the first file that has data in this piece */
    if( fileHint >= 0 ) {
        i = fileHint;
        while( i > 0 && pieceHasFile( piece, &tor->info.files[i - 1] ) )
            --i;
    } else {
        for( i = 0; i < tor->info.fileCount; ++i )
            if( pieceHasFile( piece, &tor->info.files[i] ) )
                break;
    }

    /* the piece's priority is the max of the priorities
     * of all the files in that piece */
    for( ; i < tor->info.fileCount; ++i )
    {
        const tr_file * file = &tor->info.files[i];

        if( !pieceHasFile( piece, file ) )
            break;

        priority = MAX( priority, file->priority );

        /* when dealing with multimedia files, getting the first and
           last pieces can sometimes allow you to preview it a bit
           before it's fully downloaded... */
        if( file->priority >= TR_PRI_NORMAL )
            if( file->firstPiece == piece || file->lastPiece == piece )
                priority = TR_PRI_HIGH;
    }

    return priority;
}

static void
tr_torrentInitFilePieces( tr_torrent * tor )
{
    int * firstFiles;
    tr_file_index_t f;
    tr_piece_index_t p;
    uint64_t offset = 0;
    tr_info * inf = &tor->info;

    /* assign the file offsets */
    for( f=0; f<inf->fileCount; ++f ) {
        inf->files[f].offset = offset;
        offset += inf->files[f].length;
        initFilePieces( tor, f );
    }

    /* build the array of first-file hints to give calculatePiecePriority */
    firstFiles = tr_new( int, inf->pieceCount );
    for( p=f=0; p<inf->pieceCount; ++p ) {
        while( inf->files[f].lastPiece < p )
            ++f;
        firstFiles[p] = f;
    }

#if 0
    /* test to confirm the first-file hints are correct */
    for( p=0; p<inf->pieceCount; ++p ) {
        f = firstFiles[p];
        assert( inf->files[f].firstPiece <= p );
        assert( inf->files[f].lastPiece >= p );
        if( f > 0 )
            assert( inf->files[f-1].lastPiece < p );
        for( f=0; f<inf->fileCount; ++f )
            if( pieceHasFile( p, &inf->files[f] ) )
                break;
        assert( (int)f == firstFiles[p] );
    }
#endif

    for( p=0; p<inf->pieceCount; ++p )
        inf->pieces[p].priority = calculatePiecePriority( tor, p, firstFiles[p] );

    tr_free( firstFiles );
}

static void torrentStart( tr_torrent * tor );

uint32_t
tr_getBlockSize( uint32_t pieceSize )
{
    return MIN( MAX_BLOCK_SIZE, pieceSize );
}

static void refreshCurrentDir( tr_torrent * tor );

#define CMOD( a, b ) ( ( a ) % ( b ) == 0 ? ( b ) : ( a ) % ( b ) )
#define CDIV( a, b ) ( ( ( a ) + ( b ) - 1 ) / ( b ) )

static void
torrentInitFromInfo( tr_torrent * tor )
{
    const tr_info * info = &tor->info;
    const uint64_t total_size = info->totalSize;
    const uint32_t piece_size = info->pieceSize;
    const uint32_t block_size = tr_getBlockSize( piece_size );
    const tr_piece_index_t piece_count = info->pieceCount;
    uint64_t check;

    if( !tr_torrentHasMetadata( tor ) )
        goto OUT;

    assert( total_size > 0 );
    assert( piece_size > 0 );
    assert( piece_count > 0 );
    assert( block_size > 0 );
    assert( piece_count == CDIV( total_size, piece_size ) );

    tor->whole_piece_final_block_size = CMOD( piece_size, block_size );
    tor->whole_piece_block_count = CDIV( piece_size, block_size );

    tor->final_piece_size = CMOD( total_size, piece_size );
    tor->final_piece_final_block_size = CMOD( tor->final_piece_size,
                                              block_size );
    tor->final_piece_block_count = CDIV( tor->final_piece_size,
                                         block_size );

    tor->block_size = block_size;
    tor->block_count = ( piece_count - 1 ) * tor->whole_piece_block_count;
    tor->block_count += tor->final_piece_block_count ;

    check = piece_count - 1;
    check *= piece_size;
    check += tor->final_piece_size;
    assert( check == total_size );

    check = tor->whole_piece_block_count - 1;
    check *= tor->block_size;
    check += tor->whole_piece_final_block_size;
    assert( check == piece_size );

    check = tor->final_piece_block_count - 1;
    check *= tor->block_size;
    check += tor->final_piece_final_block_size;
    assert( check == tor->final_piece_size );

OUT:
    tr_cpConstruct( &tor->completion, tor );
    tr_torrentInitFilePieces( tor );
    tor->completeness = tr_cpGetStatus( &tor->completion );
}

static void tr_torrentFireMetadataCompleted( tr_torrent * tor );

void
tr_torrentGotNewInfoDict( tr_torrent * tor )
{
    torrentInitFromInfo( tor );

    tr_torrentFireMetadataCompleted( tor );
}

/**
 * Check that the piece completion status matches the
 * existence of files in the filesystem. If pieces are
 * complete but files containing those pieces do not exist,
 * an error state is set by tr_torrentSetLocalError() and
 * FALSE is returned.
 *
 * The file's @a exists fields are updated to match their
 * current state of either being present or being absent in
 * the filesystem.
 *
 * @return TRUE if files exist for all completed pieces.
 *
 * @note This function assumes that there is no data pending
 *       in the file cache.
 *
 * @note This function will still return TRUE if files exist
 *       when they should not according to piece completion.
 *
 * @see checkOperation()
 */
static tr_bool
updateFileExistence( tr_torrent * tor )
{
    const tr_completion * cp;
    tr_piece_index_t pi;
    tr_file_index_t fi;

    assert( tr_torrentIsLocked( tor ) );

    if( !tr_torrentHasMetadata( tor ) )
        return TRUE;

    cp = &tor->completion;
    for( fi = 0; fi < tor->info.fileCount; ++fi )
    {
        tr_file * file = &tor->info.files[fi];
        file->exists = tr_torrentFindFile2( tor, fi, NULL, NULL );
        if( file->exists || file->usept )
            continue;
        for( pi = file->firstPiece; pi <= file->lastPiece; ++pi )
        {
            if( tr_cpPieceIsComplete( cp, pi ) )
            {
                tr_torrentSetLocalError( tor,
                    _( "Expected file not found: %s" ), file->name );
                return FALSE;
            }
        }
    }
    return TRUE;
}

static void
torrentInit( tr_torrent * tor, const tr_ctor * ctor )
{
    char * s;
    int doStart;
    uint64_t loaded;
    const char * dir;
    tr_bool isNewTorrent;
    struct stat st;
    static int nextUniqueId = 1;
    tr_session * session = tr_ctorGetSession( ctor );

    assert( session != NULL );

    tr_sessionLock( session );

    tor->session   = session;
    tor->uniqueId = nextUniqueId++;
    tor->magicNumber = TORRENT_MAGIC_NUMBER;

    tr_sha1( tor->obfuscatedHash, "req2", 4,
             tor->info.hash, SHA_DIGEST_LENGTH,
             NULL );
    tr_free( tor->peer_id );
    tor->peer_id = tr_peerIdNew( session );

    if( !tr_ctorGetDownloadDir( ctor, TR_FORCE, &dir ) ||
        !tr_ctorGetDownloadDir( ctor, TR_FALLBACK, &dir ) )
            tor->downloadDir = tr_strdup( dir );

    if( tr_ctorGetIncompleteDir( ctor, &dir ) )
        dir = tr_sessionGetIncompleteDir( session );
    if( tr_sessionIsIncompleteDirEnabled( session ) )
        tor->incompleteDir = tr_strdup( dir );

    s = tr_metainfoGetBasename( &tor->info );
    tor->pieceTempDir = tr_buildPath( tr_getPieceDir( tor->session ), s, NULL );
    tr_free( s );

    tor->bandwidth = tr_bandwidthNew( session, session->bandwidth );

    tor->bandwidth->priority = tr_ctorGetBandwidthPriority( ctor );

    tor->error = TR_STAT_OK;

    tor->finishedSeedingByIdle = FALSE;

    tr_peerMgrAddTorrent( session->peerMgr, tor );

    assert( !tor->downloadedCur );
    assert( !tor->uploadedCur );

    tr_torrentSetAddedDate( tor, tr_time( ) ); /* this is a default value to be
                                                  overwritten by the resume file */

    torrentInitFromInfo( tor );
    loaded = tr_torrentLoadResume( tor, ~0, ctor );
    tor->completeness = tr_cpGetStatus( &tor->completion );
    updateFileExistence( tor );

    tr_ctorInitTorrentPriorities( ctor, tor );
    tr_ctorInitTorrentWanted( ctor, tor );

    refreshCurrentDir( tor );

    doStart = tor->isRunning;
    tor->isRunning = 0;

    if( !( loaded & TR_FR_SPEEDLIMIT ) )
    {
        tr_torrentUseSpeedLimit( tor, TR_UP, FALSE );
        tr_torrentSetSpeedLimit_Bps( tor, TR_UP, tr_sessionGetSpeedLimit_Bps( tor->session, TR_UP ) );
        tr_torrentUseSpeedLimit( tor, TR_DOWN, FALSE );
        tr_torrentSetSpeedLimit_Bps( tor, TR_DOWN, tr_sessionGetSpeedLimit_Bps( tor->session, TR_DOWN ) );
        tr_torrentUseSessionLimits( tor, TRUE );
    }

    if( !( loaded & TR_FR_RATIOLIMIT ) )
    {
        tr_torrentSetRatioMode( tor, TR_RATIOLIMIT_GLOBAL );
        tr_torrentSetRatioLimit( tor, tr_sessionGetRatioLimit( tor->session ) );
    }

    if( !( loaded & TR_FR_IDLELIMIT ) )
    {
        tr_torrentSetIdleMode( tor, TR_IDLELIMIT_GLOBAL );
        tr_torrentSetIdleLimit( tor, tr_sessionGetIdleLimit( tor->session ) );
    }

    {
        tr_torrent * it = NULL;
        tr_torrent * last = NULL;
        while( ( it = tr_torrentNext( session, it ) ) )
            last = it;

        if( !last )
            session->torrentList = tor;
        else
            last->next = tor;
        ++session->torrentCount;
    }

    /* if we don't have a local .torrent file already, assume the torrent is new */
    isNewTorrent = stat( tor->info.torrent, &st );

    /* maybe save our own copy of the metainfo */
    if( tr_ctorGetSave( ctor ) )
    {
        const tr_benc * val;
        if( !tr_ctorGetMetainfo( ctor, &val ) )
        {
            const char * path = tor->info.torrent;
            const int err = tr_bencToFile( val, TR_FMT_BENC, path );
            if( err )
                tr_torrentSetLocalError( tor, "Unable to save torrent file: %s", tr_strerror( err ) );
            tr_sessionSetTorrentFile( tor->session, tor->info.hashString, path );
        }
    }

    tor->tiers = tr_announcerAddTorrent( tor, onTrackerResponse, NULL );

    if( isNewTorrent )
    {
        tor->startAfterVerify = doStart;
        tr_torrentVerify( tor );
    }
    else if( doStart )
    {
        torrentStart( tor );
    }

    tr_sessionUnlock( session );
}

static tr_parse_result
torrentParseImpl( const tr_ctor * ctor, tr_info * setmeInfo,
                  tr_bool * setmeHasInfo, int * dictLength )
{
    int             doFree;
    tr_bool         didParse;
    tr_bool         hasInfo = FALSE;
    tr_info         tmp;
    const tr_benc * metainfo;
    tr_session    * session = tr_ctorGetSession( ctor );
    tr_parse_result result = TR_PARSE_OK;

    if( setmeInfo == NULL )
        setmeInfo = &tmp;
    memset( setmeInfo, 0, sizeof( tr_info ) );

    if( tr_ctorGetMetainfo( ctor, &metainfo ) )
        return TR_PARSE_ERR;

    didParse = tr_metainfoParse( session, metainfo, setmeInfo,
                                 &hasInfo, dictLength );
    doFree = didParse && ( setmeInfo == &tmp );

    if( !didParse )
        result = TR_PARSE_ERR;

    if( didParse && hasInfo && !tr_getBlockSize( setmeInfo->pieceSize ) )
        result = TR_PARSE_ERR;

    if( didParse && session && tr_torrentExists( session, setmeInfo->hash ) )
        result = TR_PARSE_DUPLICATE;

    if( doFree )
        tr_metainfoFree( setmeInfo );

    if( setmeHasInfo != NULL )
        *setmeHasInfo = hasInfo;

    return result;
}

tr_parse_result
tr_torrentParse( const tr_ctor * ctor, tr_info * setmeInfo )
{
    return torrentParseImpl( ctor, setmeInfo, NULL, NULL );
}

tr_torrent *
tr_torrentNew( const tr_ctor * ctor, int * setmeError )
{
    int len;
    tr_bool hasInfo;
    tr_info tmpInfo;
    tr_parse_result r;
    tr_torrent * tor = NULL;

    assert( ctor != NULL );
    assert( tr_isSession( tr_ctorGetSession( ctor ) ) );

    r = torrentParseImpl( ctor, &tmpInfo, &hasInfo, &len );
    if( r == TR_PARSE_OK )
    {
        tor = tr_new0( tr_torrent, 1 );
        tor->info = tmpInfo;
        if( hasInfo )
            tor->infoDictLength = len;
        torrentInit( tor, ctor );
    }
    else
    {
        if( r == TR_PARSE_DUPLICATE )
            tr_metainfoFree( &tmpInfo );

        if( setmeError )
            *setmeError = r;
    }

    return tor;
}

/**
***
**/

void
tr_torrentSetDownloadDir( tr_torrent * tor, const char * path )
{
    assert( tr_isTorrent( tor  ) );

    if( !path || !tor->downloadDir || strcmp( path, tor->downloadDir ) )
    {
        tr_free( tor->downloadDir );
        tor->downloadDir = tr_strdup( path );
        tr_torrentSetDirty( tor );
    }

    refreshCurrentDir( tor );
}

const char*
tr_torrentGetDownloadDir( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    return tor->downloadDir;
}

const char *
tr_torrentGetCurrentDir( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    return tor->currentDir;
}


void
tr_torrentChangeMyPort( tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    if( tor->isRunning )
        tr_announcerChangeMyPort( tor );
}

static inline void
tr_torrentManualUpdateImpl( void * vtor )
{
    tr_torrent * tor = vtor;

    assert( tr_isTorrent( tor  ) );

    if( tor->isRunning )
        tr_announcerManualAnnounce( tor );
}

void
tr_torrentManualUpdate( tr_torrent * tor )
{
    assert( tr_isTorrent( tor  ) );

    tr_runInEventThread( tor->session, tr_torrentManualUpdateImpl, tor );
}

tr_bool
tr_torrentCanManualUpdate( const tr_torrent * tor )
{
    return ( tr_isTorrent( tor  ) )
        && ( tor->isRunning )
        && ( tr_announcerCanManualAnnounce( tor ) );
}

const tr_info *
tr_torrentInfo( const tr_torrent * tor )
{
    return tr_isTorrent( tor ) ? &tor->info : NULL;
}

const tr_stat *
tr_torrentStatCached( tr_torrent * tor )
{
    const time_t now = tr_time( );

    return tr_isTorrent( tor ) && ( now == tor->lastStatTime )
         ? &tor->stats
         : tr_torrentStat( tor );
}

void
tr_torrentSetVerifyState( tr_torrent * tor, tr_verify_state state )
{
    assert( tr_isTorrent( tor ) );
    assert( state==TR_VERIFY_NONE || state==TR_VERIFY_WAIT || state==TR_VERIFY_NOW );

    tor->verifyState = state;
    tor->anyDate = tr_time( );
}

tr_torrent_activity
tr_torrentGetActivity( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    tr_torrentRecheckCompleteness( tor );

    if( tor->verifyState == TR_VERIFY_NOW )
        return TR_STATUS_CHECK;
    if( tor->verifyState == TR_VERIFY_WAIT )
        return TR_STATUS_CHECK_WAIT;
    if( !tor->isRunning )
        return TR_STATUS_STOPPED;
    if( tor->completeness == TR_LEECH )
        return TR_STATUS_DOWNLOAD;

    return TR_STATUS_SEED;
}

void
tr_torrentSetVerifyProgress( tr_torrent * tor, double d )
{
    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );
    tor->verifyProgress = d;
    tr_torrentUnlock( tor );
}

const tr_stat *
tr_torrentStat( tr_torrent * tor )
{
    tr_stat *               s;
    int                     usableSeeds;
    uint64_t                now;
    double                  d;
    uint64_t                seedRatioBytesLeft;
    uint64_t                seedRatioBytesGoal;
    tr_bool                 seedRatioApplies;
    uint16_t                seedIdleMinutes;
    tr_tracker_stat *       tracker_stats;
    int                     tracker_count;
    int                     tracker_index;

    if( !tor )
        return NULL;

    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    tor->lastStatTime = tr_time( );

    s = &tor->stats;
    s->id = tor->uniqueId;
    if( tor->peer_id )
        tr_strlcpy( s->peerID, tor->peer_id, sizeof( s->peerID ) );
    else
        s->peerID[0] = '\0';
    s->activity = tr_torrentGetActivity( tor );
    s->error = tor->error;
    memcpy( s->errorString, tor->errorString, sizeof( s->errorString ) );

    s->manualAnnounceTime = tr_announcerNextManualAnnounce( tor );

    tr_peerMgrTorrentStats( tor,
                            &s->peersKnown,
                            &s->peersConnected,
                            &s->seedersConnected,
                            &s->webseedsSendingToUs,
                            &s->peersSendingToUs,
                            &s->peersGettingFromUs,
                            s->peersFrom );
    usableSeeds = s->seedersConnected;
    s->leechersConnected = s->peersConnected - s->seedersConnected;

    now = tr_time_msec( );
    d = tr_peerMgrGetWebseedSpeed_Bps( tor, now );
    s->rawUploadSpeed_KBps     = toSpeedKBps( tr_bandwidthGetRawSpeed_Bps  ( tor->bandwidth, now, TR_UP ) );
    s->pieceUploadSpeed_KBps   = toSpeedKBps( tr_bandwidthGetPieceSpeed_Bps( tor->bandwidth, now, TR_UP ) );
    s->rawDownloadSpeed_KBps   = toSpeedKBps( d + tr_bandwidthGetRawSpeed_Bps  ( tor->bandwidth, now, TR_DOWN ) );
    s->pieceDownloadSpeed_KBps = toSpeedKBps( d + tr_bandwidthGetPieceSpeed_Bps( tor->bandwidth, now, TR_DOWN ) );

    s->swarmSeeders = 0;
    s->swarmLeechers = 0;
    tracker_stats = tr_torrentTrackers( tor, &tracker_count );
    for( tracker_index = 0; tracker_index < tracker_count; ++tracker_index )
    {
        const tr_tracker_stat * st = &tracker_stats[tracker_index];
        s->swarmSeeders = MAX( s->swarmSeeders, st->seederCount );
        s->swarmLeechers = MAX( s->swarmLeechers, st->leecherCount );
    }
    tr_torrentTrackersFree( tracker_stats, tracker_count );
    s->swarmSeeders = MAX( s->swarmSeeders, usableSeeds );
    s->swarmLeechers = MAX( s->swarmLeechers, s->peersConnected - usableSeeds );

    usableSeeds += tor->info.webseedCount;

    s->percentComplete = tr_cpPercentComplete ( &tor->completion );
    s->metadataPercentComplete = tr_torrentGetMetadataPercent( tor );

    s->percentDone         = tr_cpPercentDone  ( &tor->completion );
    s->leftUntilDone       = tr_cpLeftUntilDone( &tor->completion );
    s->sizeWhenDone        = tr_cpSizeWhenDone ( &tor->completion );
    s->recheckProgress     = s->activity == TR_STATUS_CHECK ? tor->verifyProgress : 0;
    s->activityDate        = tor->activityDate;
    s->addedDate           = tor->addedDate;
    s->doneDate            = tor->doneDate;
    s->startDate           = tor->startDate;
    s->secondsSeeding      = tor->secondsSeeding;
    s->secondsDownloading  = tor->secondsDownloading;

    if ((s->activity == TR_STATUS_DOWNLOAD || s->activity == TR_STATUS_SEED) && s->startDate != 0)
        s->idleSecs = difftime(tr_time(), MAX(s->startDate, s->activityDate));
    else
        s->idleSecs = -1;

    s->corruptEver     = tor->corruptCur    + tor->corruptPrev;
    s->downloadedEver  = tor->downloadedCur + tor->downloadedPrev;
    s->uploadedEver    = tor->uploadedCur   + tor->uploadedPrev;
    s->haveValid       = tr_cpHaveValid( &tor->completion );
    s->haveUnchecked   = tr_cpHaveTotal( &tor->completion ) - s->haveValid;

    if( usableSeeds > 0 )
    {
        s->desiredAvailable = s->leftUntilDone;
    }
    else if( !s->leftUntilDone || !s->peersConnected )
    {
        s->desiredAvailable = 0;
    }
    else
    {
        tr_piece_index_t i;
        tr_bitfield *    peerPieces = tr_peerMgrGetAvailable( tor );
        s->desiredAvailable = 0;
        for( i = 0; i < tor->info.pieceCount; ++i )
            if( !tor->info.pieces[i].dnd && tr_bitfieldHasFast( peerPieces, i ) )
                s->desiredAvailable += tr_cpMissingBytesInPiece( &tor->completion, i );
        tr_bitfieldFree( peerPieces );
    }

    s->ratio = tr_getRatio( s->uploadedEver,
                            s->downloadedEver ? s->downloadedEver : s->haveValid );

    seedRatioApplies = tr_torrentGetSeedRatioBytes( tor, &seedRatioBytesLeft,
                                                         &seedRatioBytesGoal );

    switch( s->activity )
    {
        /* etaXLSpeed exists because if we use the piece speed directly,
         * brief fluctuations cause the ETA to jump all over the place.
         * so, etaXLSpeed is a smoothed-out version of the piece speed
         * to dampen the effect of fluctuations */

        case TR_STATUS_DOWNLOAD:
            if( ( tor->etaDLSpeedCalculatedAt + 800 ) < now ) {
                tor->etaDLSpeed_KBps = ( ( tor->etaDLSpeedCalculatedAt + 4000 ) < now )
                    ? s->pieceDownloadSpeed_KBps /* if no recent previous speed, no need to smooth */
                    : ((tor->etaDLSpeed_KBps*4.0) + s->pieceDownloadSpeed_KBps)/5.0; /* smooth across 5 readings */
                tor->etaDLSpeedCalculatedAt = now;
            }

            if( s->leftUntilDone > s->desiredAvailable )
                s->eta = TR_ETA_NOT_AVAIL;
            else if( tor->etaDLSpeed_KBps < 1 )
                s->eta = TR_ETA_UNKNOWN;
            else
                s->eta = s->leftUntilDone / toSpeedBytes(tor->etaDLSpeed_KBps);

            s->etaIdle = TR_ETA_NOT_AVAIL;
            break;

        case TR_STATUS_SEED: {
            if( !seedRatioApplies )
                s->eta = TR_ETA_NOT_AVAIL;
            else {
                if( ( tor->etaULSpeedCalculatedAt + 800 ) < now ) {
                    tor->etaULSpeed_KBps = ( ( tor->etaULSpeedCalculatedAt + 4000 ) < now )
                        ? s->pieceUploadSpeed_KBps /* if no recent previous speed, no need to smooth */
                        : ((tor->etaULSpeed_KBps*4.0) + s->pieceUploadSpeed_KBps)/5.0; /* smooth across 5 readings */
                    tor->etaULSpeedCalculatedAt = now;
                }
                if( tor->etaULSpeed_KBps < 1 )
                    s->eta = TR_ETA_UNKNOWN;
                else
                    s->eta = seedRatioBytesLeft / toSpeedBytes(tor->etaULSpeed_KBps);
            }

            if( tor->etaULSpeed_KBps < 1 && tr_torrentGetSeedIdle( tor, &seedIdleMinutes ) )
                s->etaIdle = seedIdleMinutes * 60 - s->idleSecs;
            else
                s->etaIdle = TR_ETA_NOT_AVAIL;
            break;
        }

        default:
            s->eta = TR_ETA_NOT_AVAIL;
            s->etaIdle = TR_ETA_NOT_AVAIL;
            break;
    }

    /* s->haveValid is here to make sure a torrent isn't marked 'finished'
     * when the user hits "uncheck all" prior to starting the torrent... */
    s->finished = tor->finishedSeedingByIdle || (seedRatioApplies && !seedRatioBytesLeft && s->haveValid);

    if( !seedRatioApplies || s->finished )
        s->seedRatioPercentDone = 1;
    else if( !seedRatioBytesGoal ) /* impossible? safeguard for div by zero */
        s->seedRatioPercentDone = 0;
    else
        s->seedRatioPercentDone = (double)(seedRatioBytesGoal - seedRatioBytesLeft) / seedRatioBytesGoal;

    tr_torrentUnlock( tor );

    return s;
}

/***
****
***/

static uint64_t
fileBytesCompleted( const tr_torrent * tor, tr_file_index_t index )
{
    const tr_completion * cp = &tor->completion;
    const tr_file * f = &tor->info.files[index];
    tr_piece_index_t pi;
    uint64_t total = 0;

    if( f->length == 0 )
        return 0;

    if( f->firstPiece == f->lastPiece )
        return tr_cpPieceIsComplete( cp, f->firstPiece ) ? f->length : 0;

    /* the first piece */
    if( tr_cpPieceIsComplete( cp, f->firstPiece ) )
        total += ( tr_torPieceCountBytes( tor, f->firstPiece )
                   - ( f->offset - tr_torPieceByte( tor, f->firstPiece ) ) );

    /* the middle pieces */
    for( pi = f->firstPiece + 1; pi < f->lastPiece; ++pi )
        if( tr_cpPieceIsComplete( cp, pi ) )
            total += tr_torPieceCountBytes( tor, pi );

    /* the last piece */
    if( tr_cpPieceIsComplete( cp, f->lastPiece ) )
        total += f->offset + f->length - tr_torPieceByte( tor, f->lastPiece );

    return total;
}

tr_file_stat *
tr_torrentFiles( const tr_torrent * tor,
                 tr_file_index_t *  fileCount )
{
    tr_file_index_t       i;
    const tr_file_index_t n = tor->info.fileCount;
    tr_file_stat *        files = tr_new0( tr_file_stat, n );
    tr_file_stat *        walk = files;
    const tr_bool         isSeed = tor->completeness == TR_SEED;

    assert( tr_isTorrent( tor ) );

    for( i=0; i<n; ++i, ++walk ) {
        const uint64_t b = isSeed ? tor->info.files[i].length : fileBytesCompleted( tor, i );
        walk->bytesCompleted = b;
        walk->progress = tor->info.files[i].length > 0 ? ( (float)b / tor->info.files[i].length ) : 1.0f;
    }

    if( fileCount )
        *fileCount = n;

    return files;
}

void
tr_torrentFilesFree( tr_file_stat *            files,
                     tr_file_index_t fileCount UNUSED )
{
    tr_free( files );
}

/***
****
***/

double*
tr_torrentWebSpeeds_KBps( const tr_torrent * tor )
{
    double * ret = NULL;

    if( tr_isTorrent( tor ) )
    {
        tr_torrentLock( tor );
        ret = tr_peerMgrWebSpeeds_KBps( tor );
        tr_torrentUnlock( tor );
    }

    return ret;
}

tr_peer_stat *
tr_torrentPeers( const tr_torrent * tor, int * peerCount )
{
    tr_peer_stat * ret = NULL;

    if( tr_isTorrent( tor ) )
    {
        tr_torrentLock( tor );
        ret = tr_peerMgrPeerStats( tor, peerCount );
        tr_torrentUnlock( tor );
    }

    return ret;
}

void
tr_torrentPeersFree( tr_peer_stat * peers, int peerCount )
{
    int i;
    for( i = 0; i < peerCount; ++i )
    {
        tr_free( peers[i].peer_id );
        tr_free( peers[i].user_agent );
        tr_free( peers[i].extensions );
    }
    if( peers && peerCount > 0 )
        memset( peers, 0, sizeof( *peers ) * peerCount );
    tr_free( peers );
}

tr_tracker_stat *
tr_torrentTrackers( const tr_torrent * torrent, int * setmeTrackerCount )
{
    tr_tracker_stat * ret = NULL;

    if( tr_isTorrent( torrent ) )
    {
        tr_torrentLock( torrent );
        ret = tr_announcerStats( torrent, setmeTrackerCount );
        tr_torrentUnlock( torrent );
    }

    return ret;
}

void
tr_torrentTrackersFree( tr_tracker_stat * trackers, int trackerCount )
{
    tr_announcerStatsFree( trackers, trackerCount );
}

void
tr_torrentAvailability( const tr_torrent * tor, int8_t * tab, int size )
{
    if( tr_isTorrent( tor ) && ( tab != NULL ) && ( size > 0 ) )
    {
        tr_torrentLock( tor );
        tr_peerMgrTorrentAvailability( tor, tab, size );
        tr_torrentUnlock( tor );
    }
}

void
tr_torrentAmountFinished( const tr_torrent * tor,
                          float *            tab,
                          int                size )
{
    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );
    tr_cpGetAmountDone( &tor->completion, tab, size );
    tr_torrentUnlock( tor );
}

static void
tr_torrentResetTransferStats( tr_torrent * tor )
{
    tr_torrentLock( tor );

    tor->downloadedPrev += tor->downloadedCur;
    tor->downloadedCur   = 0;
    tor->uploadedPrev   += tor->uploadedCur;
    tor->uploadedCur     = 0;
    tor->corruptPrev    += tor->corruptCur;
    tor->corruptCur      = 0;

    tr_torrentSetDirty( tor );

    tr_torrentUnlock( tor );
}

void
tr_torrentSetHasPiece( tr_torrent *     tor,
                       tr_piece_index_t pieceIndex,
                       tr_bool          has )
{
    assert( tr_isTorrent( tor ) );
    assert( pieceIndex < tor->info.pieceCount );

    if( has )
        tr_cpPieceAdd( &tor->completion, pieceIndex );
    else
        tr_cpPieceRem( &tor->completion, pieceIndex );
}

/***
****
***/

static void
freeTorrent( tr_torrent * tor )
{
    tr_torrent * t;
    tr_session *  session = tor->session;
    tr_info *    inf = &tor->info;

    assert( !tor->isRunning );

    tr_sessionLock( session );

    tr_peerMgrRemoveTorrent( tor );

    tr_cpDestruct( &tor->completion );

    tr_announcerRemoveTorrent( session->announcer, tor );

    tr_free( tor->downloadDir );
    tr_free( tor->incompleteDir );
    tr_free( tor->pieceTempDir );
    tr_free( tor->peer_id );

    if( tor == session->torrentList )
        session->torrentList = tor->next;
    else for( t = session->torrentList; t != NULL; t = t->next ) {
        if( t->next == tor ) {
            t->next = tor->next;
            break;
        }
    }

    assert( session->torrentCount >= 1 );
    session->torrentCount--;

    tr_bandwidthFree( tor->bandwidth );

    tr_metainfoFree( inf );
    memset( tor, 0, sizeof( *tor ) );
    tr_free( tor );

    tr_sessionUnlock( session );
}

/**
***  Start/Stop Callback
**/

static void
torrentStartImpl( void * vtor )
{
    time_t now;
    tr_torrent * tor = vtor;

    assert( tr_isTorrent( tor ) );

    tr_sessionLock( tor->session );

    tr_torrentRecheckCompleteness( tor );

    now = tr_time( );
    tor->isRunning = TRUE;
    tor->completeness = tr_cpGetStatus( &tor->completion );
    tor->startDate = tor->anyDate = now;
    tr_torrentClearError( tor );
    tor->finishedSeedingByIdle = FALSE;

    tr_torrentResetTransferStats( tor );
    tr_torrentSave( tor );

    tr_announcerTorrentStarted( tor );
    tor->dhtAnnounceAt = now + tr_cryptoWeakRandInt( 20 );
    tor->dhtAnnounce6At = now + tr_cryptoWeakRandInt( 20 );
    tor->lpdAnnounceAt = now;
    tr_peerMgrStartTorrent( tor );

    tr_sessionUnlock( tor->session );
}

uint64_t
tr_torrentGetCurrentSizeOnDisk( const tr_torrent * tor )
{
    tr_file_index_t i;
    uint64_t byte_count = 0;
    const tr_file_index_t n = tor->info.fileCount;

    for( i=0; i<n; ++i )
    {
        struct stat sb;
        char * filename = tr_torrentFindFile( tor, i );

        sb.st_size = 0;
        if( filename && !stat( filename, &sb ) )
            byte_count += sb.st_size;

        tr_free( filename );
    }

    return byte_count;
}

static void
torrentStart( tr_torrent * tor )
{
    /* already running... */
    if( tor->isRunning )
        return;

    /* verifying right now... wait until that's done so
     * we'll know what completeness to use/announce */
    if( tor->verifyState != TR_VERIFY_NONE ) {
        tor->startAfterVerify = TRUE;
        return;
    }

    /* otherwise, start it now... */
    tr_sessionLock( tor->session );

    if( !updateFileExistence( tor ) )
        goto OUT;

    /* allow finished torrents to be resumed */
    if( tr_torrentIsSeedRatioDone( tor ) ) {
        tr_torinf( tor, _( "Restarted manually -- disabling its seed ratio" ) );
        tr_torrentSetRatioMode( tor, TR_RATIOLIMIT_UNLIMITED );
    }

    /* corresponds to the peer_id sent as a tracker request parameter.
     * one tracker admin says: "When the same torrent is opened and
     * closed and opened again without quitting Transmission ...
     * change the peerid. It would help sometimes if a stopped event
     * was missed to ensure that we didn't think someone was cheating. */
    tr_free( tor->peer_id );
    tor->peer_id = tr_peerIdNew( tor->session );
    tor->isRunning = 1;
    tr_torrentSetDirty( tor );
    tr_runInEventThread( tor->session, torrentStartImpl, tor );

OUT:
    tr_sessionUnlock( tor->session );
}

void
tr_torrentStart( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
        torrentStart( tor );
}

static void
torrentRecheckDoneImpl( void * vtor )
{
    tr_torrent * tor = vtor;
    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    tr_torrentRecheckCompleteness( tor );

    if( tor->startAfterVerify ) {
        tor->startAfterVerify = FALSE;
        torrentStart( tor );
    }
    else
    {
        tr_torrentSave( tor );
    }

    tr_torrentUnlock( tor );
}

static void
torrentRecheckDoneCB( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    tr_runInEventThread( tor->session, torrentRecheckDoneImpl, tor );
}

static void
verifyTorrent( void * vtor )
{
    tr_torrent * tor = vtor;

    tr_sessionLock( tor->session );

    /* if the torrent's already being verified, stop it */
    tr_verifyRemove( tor );

    /* if the torrent's running, stop it & set the restart-after-verify flag */
    if( tor->startAfterVerify || tor->isRunning ) {
        /* don't clobber isStopping */
        const tr_bool startAfter = tor->isStopping ? FALSE : TRUE;
        tr_torrentStop( tor );
        tor->startAfterVerify = startAfter;
    }

    tr_torrentClearError( tor );
    tr_verifyAdd( tor, torrentRecheckDoneCB );

    tr_sessionUnlock( tor->session );
}

void
tr_torrentVerify( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
        tr_runInEventThread( tor->session, verifyTorrent, tor );
}

static void
setExistingFilesVerified( tr_torrent * tor )
{
    tr_file_index_t fi;
    tr_piece_index_t pi;
    const tr_info * info = tr_torrentInfo( tor );
    tr_bool * missing = tr_new0( tr_bool, info->pieceCount );

    for( fi = 0; fi < info->fileCount; ++fi  )
    {
        const tr_file * file = &info->files[fi];
        const tr_bool have = !file->dnd
            && tr_torrentFindFile2( tor, fi, NULL, NULL );

        for( pi = file->firstPiece; pi <= file->lastPiece; ++pi )
            if( !missing[pi] && !have )
                missing[pi] = TRUE;
    }

    for( pi = 0; pi < info->pieceCount; ++pi )
        tr_torrentSetHasPiece( tor, pi, !missing[pi] );
    tr_free( missing );
}

static void
setTorrentFilesVerified( void * vtor )
{
    tr_torrent * tor = vtor;
    tr_bool startAfter = FALSE;

    assert( tr_isTorrent( tor ) );
    tr_sessionLock( tor->session );

    tr_verifyRemove( tor );
    if( tor->startAfterVerify || tor->isRunning ) {
        startAfter = !tor->isStopping;
        tr_torrentStop( tor );
    }

    setExistingFilesVerified( tor );
    tor->anyDate = tr_time( );
    tr_torrentRecheckCompleteness( tor );

    if( startAfter )
        torrentStart( tor );

    tr_sessionUnlock( tor->session );
}

void
tr_torrentSetFilesVerified( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
        tr_runInEventThread( tor->session, setTorrentFilesVerified, tor );
}

void
tr_torrentSave( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( tor->isDirty )
    {
        tor->isDirty = FALSE;
        tr_torrentSaveResume( tor );
    }
}

static void
stopTorrent( void * vtor )
{
    tr_torrent * tor = vtor;
    tr_torinf( tor, "Pausing" );

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );

    tr_verifyRemove( tor );
    tr_peerMgrStopTorrent( tor );
    tr_announcerTorrentStopped( tor );
    tr_cacheFlushTorrent( tor->session->cache, tor );

    tr_fdTorrentClose( tor->session, tor->uniqueId );

    if( !tor->isDeleting )
        tr_torrentSave( tor );

    tr_torrentUnlock( tor );
}

void
tr_torrentStop( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( tr_isTorrent( tor ) )
    {
        tr_sessionLock( tor->session );

        tor->isRunning = 0;
        tor->isStopping = 0;
        tr_torrentSetDirty( tor );
        tr_runInEventThread( tor->session, stopTorrent, tor );

        tr_sessionUnlock( tor->session );
    }
}

static void
deleteLocalFile( const char * filename, tr_fileFunc fileFunc )
{
    struct stat sb;
    if( !stat( filename, &sb ) ) /* if file exists... */
        fileFunc( filename );
}

/**
 * @brief Delete all temporary piece files for the torrent.
 */
static void
tr_torrentRemovePieceTemp( tr_torrent * tor )
{
    DIR * dir;
    struct dirent * d;
    tr_list * l;
    tr_list * files = NULL;
    const char * path = tor->pieceTempDir;

    if(( dir = opendir( path )))
    {
        while(( d = readdir( dir )))
        {
            const char * name = d->d_name;

            if( name && strcmp( name, "." ) && strcmp( name, ".." ) )
                tr_list_append( &files, tr_buildPath( path, name, NULL ) );
        }

        closedir( dir );
        tr_list_append( &files, tr_strdup( path ) );
    }

    for( l = files; l != NULL; l = l->next )
        deleteLocalFile( l->data, remove );

    tr_list_free( &files, tr_free );
}

static void
closeTorrent( void * vtor )
{
    tr_benc * d;
    tr_torrent * tor = vtor;

    assert( tr_isTorrent( tor ) );

    d = tr_bencListAddDict( &tor->session->removedTorrents, 2 );
    tr_bencDictAddInt( d, "id", tor->uniqueId );
    tr_bencDictAddInt( d, "date", tr_time( ) );

    tr_torinf( tor, "%s", _( "Removing torrent" ) );

    stopTorrent( tor );

    if( tor->isDeleting )
    {
        tr_metainfoRemoveSaved( tor->session, &tor->info );
        tr_torrentRemoveResume( tor );
        tr_torrentRemovePieceTemp( tor );
    }

    tor->isRunning = 0;
    freeTorrent( tor );
}

void
tr_torrentFree( tr_torrent * tor )
{
    if( tr_isTorrent( tor ) )
    {
        tr_session * session = tor->session;
        assert( tr_isSession( session ) );
        tr_sessionLock( session );

        tr_torrentClearCompletenessCallback( tor );
        tr_runInEventThread( session, closeTorrent, tor );

        tr_sessionUnlock( session );
    }
}

struct remove_data
{
    tr_torrent   * tor;
    tr_bool        deleteFlag;
    tr_fileFunc  * deleteFunc;
};

static void tr_torrentDeleteLocalData( tr_torrent *, tr_fileFunc );

static void
removeTorrent( void * vdata )
{
    struct remove_data * data = vdata;

    if( data->deleteFlag )
        tr_torrentDeleteLocalData( data->tor, data->deleteFunc );

    tr_torrentClearCompletenessCallback( data->tor );
    closeTorrent( data->tor );
    tr_free( data );
}

void
tr_torrentRemove( tr_torrent   * tor,
                  tr_bool        deleteFlag, 
                  tr_fileFunc    deleteFunc )
{
    struct remove_data * data;

    assert( tr_isTorrent( tor ) );
    tor->isDeleting = 1;

    data = tr_new0( struct remove_data, 1 );
    data->tor = tor;
    data->deleteFlag = deleteFlag;
    data->deleteFunc = deleteFunc;
    tr_runInEventThread( tor->session, removeTorrent, data );
}

/**
***  Completeness
**/

static const char *
getCompletionString( int type )
{
    switch( type )
    {
        /* Translators: this is a minor point that's safe to skip over, but FYI:
           "Complete" and "Done" are specific, different terms in Transmission:
           "Complete" means we've downloaded every file in the torrent.
           "Done" means we're done downloading the files we wanted, but NOT all
           that exist */
        case TR_PARTIAL_SEED:
            return _( "Done" );

        case TR_SEED:
            return _( "Complete" );

        default:
            return _( "Incomplete" );
    }
}

static void
fireCompletenessChange( tr_torrent       * tor,
                        tr_completeness    status,
                        tr_bool            wasRunning )
{
    assert( ( status == TR_LEECH )
         || ( status == TR_SEED )
         || ( status == TR_PARTIAL_SEED ) );

    if( tor->completeness_func )
        tor->completeness_func( tor, status, wasRunning,
                                tor->completeness_func_user_data );
}

void
tr_torrentSetCompletenessCallback( tr_torrent                    * tor,
                                   tr_torrent_completeness_func    func,
                                   void                          * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->completeness_func = func;
    tor->completeness_func_user_data = user_data;
}

void
tr_torrentClearCompletenessCallback( tr_torrent * torrent )
{
    tr_torrentSetCompletenessCallback( torrent, NULL, NULL );
}

void
tr_torrentSetRatioLimitHitCallback( tr_torrent                     * tor,
                                    tr_torrent_ratio_limit_hit_func  func,
                                    void                           * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->ratio_limit_hit_func = func;
    tor->ratio_limit_hit_func_user_data = user_data;
}

void
tr_torrentClearRatioLimitHitCallback( tr_torrent * torrent )
{
    tr_torrentSetRatioLimitHitCallback( torrent, NULL, NULL );
}

void
tr_torrentSetIdleLimitHitCallback( tr_torrent                    * tor,
                                   tr_torrent_idle_limit_hit_func  func,
                                   void                          * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->idle_limit_hit_func = func;
    tor->idle_limit_hit_func_user_data = user_data;
}

void
tr_torrentClearIdleLimitHitCallback( tr_torrent * torrent )
{
    tr_torrentSetIdleLimitHitCallback( torrent, NULL, NULL );
}

static void
onSigCHLD( int i UNUSED )
{
    waitpid( -1, NULL, WNOHANG );
}

static void
torrentCallScript( const tr_torrent * tor, const char * script )
{
    char timeStr[128];
    const time_t now = tr_time( );

    tr_strlcpy( timeStr, ctime( &now ), sizeof( timeStr ) );
    *strchr( timeStr,'\n' ) = '\0';

    if( script && *script )
    {
        int i;
        char * cmd[] = { tr_strdup( script ), NULL };
        char * env[] = {
            tr_strdup_printf( "TR_APP_VERSION=%s", SHORT_VERSION_STRING ),
            tr_strdup_printf( "TR_TIME_LOCALTIME=%s", timeStr ),
            tr_strdup_printf( "TR_TORRENT_DIR=%s", tor->currentDir ),
            tr_strdup_printf( "TR_TORRENT_ID=%d", tr_torrentId( tor ) ),
            tr_strdup_printf( "TR_TORRENT_HASH=%s", tor->info.hashString ),
            tr_strdup_printf( "TR_TORRENT_NAME=%s", tr_torrentName( tor ) ),
            NULL };

        tr_torinf( tor, "Calling script \"%s\"", script ); 

#ifdef WIN32
        _spawnvpe( _P_NOWAIT, script, (const char*)cmd, env );
#else
        signal( SIGCHLD, onSigCHLD );

        if( !fork( ) )
        {
            execve( script, cmd, env );
            _exit( 0 );
        }
#endif

        for( i=0; cmd[i]; ++i ) tr_free( cmd[i] );
        for( i=0; env[i]; ++i ) tr_free( env[i] );
    }
}

void
tr_torrentRecheckCompleteness( tr_torrent * tor )
{
    tr_completeness completeness;

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );

    completeness = tr_cpGetStatus( &tor->completion );

    if( completeness != tor->completeness )
    {
        const int recentChange = tor->downloadedCur != 0;
        const tr_bool wasLeeching = !tr_torrentIsSeed( tor );
        const tr_bool wasRunning = tor->isRunning;

        if( recentChange )
        {
            tr_torinf( tor, _( "State changed from \"%1$s\" to \"%2$s\"" ),
                      getCompletionString( tor->completeness ),
                      getCompletionString( completeness ) );
        }

        tor->completeness = completeness;
        tr_fdTorrentClose( tor->session, tor->uniqueId );

        if( tr_torrentIsSeed( tor ) )
        {
            if( recentChange )
            {
                tr_announcerTorrentCompleted( tor );
                tor->doneDate = tor->anyDate = tr_time( );
            }

            if( wasLeeching && wasRunning )
            {
                /* clear interested flag on all peers */
                tr_peerMgrClearInterest( tor );

                /* if completeness was TR_LEECH then the seed limit check will have been skipped in bandwidthPulse */
                tr_torrentCheckSeedLimit( tor );
            }

            if( tor->currentDir == tor->incompleteDir )
                tr_torrentSetLocation( tor, tor->downloadDir, TRUE, NULL, NULL );

            if( tr_sessionIsTorrentDoneScriptEnabled( tor->session ) )
                torrentCallScript( tor, tr_sessionGetTorrentDoneScript( tor->session ) );
        }

        fireCompletenessChange( tor, wasRunning, completeness );

        tr_torrentSetDirty( tor );
    }

    tr_torrentUnlock( tor );
}

/***
****
***/

static void
tr_torrentFireMetadataCompleted( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    if( tor->metadata_func )
        tor->metadata_func( tor, tor->metadata_func_user_data );
}

void
tr_torrentSetMetadataCallback( tr_torrent                * tor,
                               tr_torrent_metadata_func    func,
                               void                      * user_data )
{
    assert( tr_isTorrent( tor ) );

    tor->metadata_func = func;
    tor->metadata_func_user_data = user_data;
}


/**
***  File priorities
**/

void
tr_torrentInitFilePriority( tr_torrent *    tor,
                            tr_file_index_t fileIndex,
                            tr_priority_t   priority )
{
    tr_piece_index_t i;
    tr_file *        file;

    assert( tr_isTorrent( tor ) );
    assert( fileIndex < tor->info.fileCount );
    assert( tr_isPriority( priority ) );

    file = &tor->info.files[fileIndex];
    file->priority = priority;
    for( i = file->firstPiece; i <= file->lastPiece; ++i )
        tor->info.pieces[i].priority = calculatePiecePriority( tor, i, fileIndex );
}

void
tr_torrentSetFilePriorities( tr_torrent             * tor,
                             const tr_file_index_t  * files,
                             tr_file_index_t          fileCount,
                             tr_priority_t            priority )
{
    tr_file_index_t i;
    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    for( i = 0; i < fileCount; ++i )
        if( files[i] < tor->info.fileCount )
            tr_torrentInitFilePriority( tor, files[i], priority );
    tr_torrentSetDirty( tor );
    tr_peerMgrRebuildRequests( tor );

    tr_torrentUnlock( tor );
}

tr_priority_t*
tr_torrentGetFilePriorities( const tr_torrent * tor )
{
    tr_file_index_t i;
    tr_priority_t * p;

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );
    p = tr_new0( tr_priority_t, tor->info.fileCount );
    for( i = 0; i < tor->info.fileCount; ++i )
        p[i] = tor->info.files[i].priority;
    tr_torrentUnlock( tor );

    return p;
}

/**
***  File Names
**/

void
tr_torrentInitFileName( tr_torrent *    tor,
                        tr_file_index_t fileIndex,
                        const char *    name )
{
    tr_file * file;

    assert( tr_isTorrent( tor ) );
    assert( fileIndex < tor->info.fileCount );
    assert( name != NULL );
    assert( name[0] != '\0' );

    file = &tor->info.files[fileIndex];
    tr_free( file->name );
    file->name = tr_strdup( name );
}

/**
***
**/

static tr_bool fileExists( const char * filename );

tr_bool
tr_torrentFindPieceTemp2( const tr_torrent  * tor,
                          tr_piece_index_t    pieceIndex,
                          const char       ** base,
                          char             ** subpath )
{
    const char * b = tor->pieceTempDir;
    char * s, * filename;
    tr_bool exists = FALSE;

    s = tr_strdup_printf( "%010u.dat", pieceIndex );

    filename = tr_buildPath( b, s, NULL );
    exists = fileExists( filename );
    tr_free( filename );

    if( base )
        *base = b;
    if( subpath )
        *subpath = s;
    else
        tr_free( s );

    return exists;
}

char *
tr_torrentFindPieceTemp( const tr_torrent * tor,
                         tr_piece_index_t   pieceIndex )
{
    const char * base;
    char * subpath, * filename = NULL;

    if( tr_torrentFindPieceTemp2( tor, pieceIndex, &base, &subpath ) )
    {
        filename = tr_buildPath( base, subpath, NULL );
        tr_free( subpath );
    }
    return filename;
}

/**
***  File DND
**/

static void
removePieceTemp( tr_torrent * tor, tr_piece_index_t piece )
{
    char * filename;
    tr_fdFileClose( tor->session, tor, piece, TR_FD_INDEX_PIECE );
    if( ( filename = tr_torrentFindPieceTemp( tor, piece ) ) )
    {
        deleteLocalFile( filename, remove );
        tr_free( filename );
    }
}

/**
 * @return TRUE if the file should use temporary piece files.
 */
static tr_bool
usePieceTemp( tr_torrent * tor, tr_file_index_t i )
{
    int fd;

    if( !tor->info.files[i].dnd )
        return FALSE;

    fd = tr_fdFileGetCached( tor->session, tr_torrentId( tor ),
                             i, TR_FD_INDEX_FILE, FALSE );
    return fd < 0 && !tr_torrentFindFile2( tor, i, NULL, NULL );
}

/**
 * Calculate the offset and amount of overlap that the file
 * given by index @a fi has with its first and last pieces. The
 * offsets are relative to the start of pieces, and the the
 * overlap sizes are less than or equal to the piece size.
 *
 * @note All of the @a setme_* arguments are assumed non-NULL.
 *
 * @note For small files, be sure to check whether the file
 *       is completely contained in a single piece, i.e.
 *       whether @code file->firstPiece == file->lastPiece @endcode.
 */
static void
getFileOverlap( tr_torrent * tor,
                tr_file_index_t fi,
                size_t * setme_fpoffset,
                size_t * setme_fpoverlap,
                size_t * setme_lpoffset,
                size_t * setme_lpoverlap )
{
    const tr_file * file = &tor->info.files[fi];
    tr_piece_index_t fpindex = file->firstPiece;
    tr_piece_index_t lpindex = file->lastPiece;
    size_t fpoffset, fpoverlap, lpoffset, lpoverlap;

    fpoffset = file->offset - tr_pieceOffset( tor, fpindex, 0, 0 );
    fpoverlap = tr_torPieceCountBytes( tor, fpindex ) - fpoffset;
    if( fpoverlap > file->length )
        fpoverlap = file->length;

    if( fpindex != lpindex )
    {
        lpoffset = 0;
        lpoverlap = ( file->offset + file->length
                      - tr_pieceOffset( tor, lpindex, 0, 0 ) );
    }
    else
    {
        lpoffset = fpoffset;
        lpoverlap = fpoverlap;
    }

    *setme_fpoffset = fpoffset;
    *setme_fpoverlap = fpoverlap;
    *setme_lpoffset = lpoffset;
    *setme_lpoverlap = lpoverlap;
}

/**
 * @note This function assumes @a tor is valid and already locked, and
 *       @a file_index is a valid file index for the torrent.
 * @note When @a file->dnd is TRUE and @a dnd is FALSE, this function has
 *       the side effect of copying over data from temporary piece files
 *       to the destination file.
 * @see readOrWriteBytes()
 */
static void
setFileDND( tr_torrent * tor, tr_file_index_t file_index, int8_t dnd )
{
    tr_file * file = &tor->info.files[file_index];
    tr_file_index_t i;
    const tr_piece_index_t fpindex = file->firstPiece;
    const tr_piece_index_t lpindex = file->lastPiece;
    tr_bool fpmovept, lpmovept, fpdnd, lpdnd, fpnopt, lpnopt;
    size_t fpoverlap, lpoverlap, fpoffset, lpoffset;
    uint8_t * fpbuf, * lpbuf;

    if( file->dnd == dnd )
        return;

    /* Flags indicating whether we need to copy over existing data
     * from temporary piece files to the actual destination file. */
    fpmovept = file->usept && !dnd;
    lpmovept = fpmovept && fpindex != lpindex;

    /* Check cache and filesystem to make sure temporary piece files exist. */
    if( fpmovept )
    {
        tr_cacheFlushPiece( tor->session->cache, tor, fpindex );
        fpmovept = tr_torrentFindPieceTemp2( tor, fpindex, NULL, NULL );
    }
    if( lpmovept )
    {
        tr_cacheFlushPiece( tor->session->cache, tor, lpindex );
        lpmovept = tr_torrentFindPieceTemp2( tor, lpindex, NULL, NULL );
    }

    getFileOverlap( tor, file_index,
                    &fpoffset, &fpoverlap,
                    &lpoffset, &lpoverlap );

    if( fpmovept )
    {
        fpbuf = tr_malloc0( fpoverlap );
        if( tr_ioRead( tor, fpindex, fpoffset, fpoverlap, fpbuf ) != 0 )
        {
            tr_free( fpbuf );
            fpmovept = FALSE;
        }
    }

    if( lpmovept )
    {
        lpbuf = tr_malloc0( lpoverlap );
        if( tr_ioRead( tor, lpindex, lpoffset, lpoverlap, lpbuf ) != 0 )
        {
            tr_free( lpbuf );
            lpmovept = FALSE;
        }
    }

    file->dnd = dnd;
    if( fpmovept || lpmovept )
        file->usept = FALSE;
    else
        file->usept = usePieceTemp( tor, file_index );

    if( fpmovept )
    {
        tr_ioWrite( tor, fpindex, fpoffset, fpoverlap, fpbuf );
        tr_free( fpbuf );
    }
    if( lpmovept )
    {
        tr_ioWrite( tor, lpindex, lpoffset, lpoverlap, lpbuf );
        tr_free( lpbuf );
    }

    /* Check conditions for setting piece DND and
     * removing temporary piece files:
     * - We can set the piece to DND if all files using
     *   that piece are DND.
     * - We can remove the temporary piece file if all
     *   files using it have 'usept' set to FALSE. */

    fpdnd = file->dnd;
    fpnopt = !file->usept;
    if( file_index > 0 )
    {
        for( i = file_index - 1; fpdnd || fpnopt; --i )
        {
            if( tor->info.files[i].lastPiece != fpindex )
                break;
            if( fpdnd )
                fpdnd = tor->info.files[i].dnd;
            if( fpnopt )
                fpnopt = !tor->info.files[i].usept;
            if( !i )
                break;
        }
    }

    lpdnd = file->dnd;
    lpnopt = !file->usept;
    for( i = file_index + 1; ( lpdnd || lpnopt ) && i < tor->info.fileCount; ++i )
    {
        if( tor->info.files[i].firstPiece != lpindex )
            break;
        if( lpdnd )
            lpdnd = tor->info.files[i].dnd;
        if( lpnopt )
            lpnopt = !tor->info.files[i].usept;
    }

    if( fpindex == lpindex )
    {
        tor->info.pieces[fpindex].dnd = fpdnd && lpdnd;
        if( fpnopt && lpnopt )
            removePieceTemp( tor, fpindex );
    }
    else
    {
        tr_piece_index_t p;
        tor->info.pieces[fpindex].dnd = fpdnd;
        tor->info.pieces[lpindex].dnd = lpdnd;
        for( p = fpindex + 1; p < lpindex; ++p )
            tor->info.pieces[p].dnd = dnd;
        if( fpnopt )
            removePieceTemp( tor, fpindex );
        if( lpnopt )
            removePieceTemp( tor, lpindex );
    }
}

void
tr_torrentInitFileDLs( tr_torrent             * tor,
                       const tr_file_index_t  * files,
                       tr_file_index_t          fileCount,
                       tr_bool                  doDownload )
{
    tr_file_index_t i;

    assert( tr_isTorrent( tor ) );

    tr_torrentLock( tor );

    for( i=0; i<fileCount; ++i )
        if( files[i] < tor->info.fileCount )
            setFileDND( tor, files[i], !doDownload );

    tr_cpInvalidateDND( &tor->completion );

    tr_torrentUnlock( tor );
}

/**
 * Delete a file set to DND, if all pieces making up the file are also
 * set to DND. Otherwise, delete the file and write back the overlapping
 * non-DND piece parts.
 *
 * @note This function assumes it is only called from
 *       tr_torrentDeleteDNDFiles().
 *
 * @return TRUE if the file was deleted.
 *
 * @see setFileDND()
 */
static tr_bool
deleteDNDFile( tr_torrent      * tor,
               tr_file_index_t   file_index,
               tr_fileFunc       removeFunc )
{
    tr_file * file;
    tr_file_index_t fi;
    tr_piece_index_t fpindex, lpindex, pi;
    size_t fpoffset, lpoffset, fpoverlap, lpoverlap;
    int fpblocks, lpblocks;
    tr_bool fpsave, lpsave, fpallpt, lpallpt;
    uint8_t * fpbuf = NULL, * lpbuf = NULL;
    char * path = NULL;

    if( file_index >= tor->info.fileCount )
        return FALSE;

    file = &tor->info.files[file_index];

    if( !file->dnd || file->usept )
        return FALSE;

    fpindex = file->firstPiece;
    lpindex = file->lastPiece;
    fpblocks = tr_cpCompleteBlocksInPiece( &tor->completion, fpindex );
    lpblocks = tr_cpCompleteBlocksInPiece( &tor->completion, lpindex );

    getFileOverlap( tor, file_index,
                    &fpoffset, &fpoverlap,
                    &lpoffset, &lpoverlap );

    /* We need to preserve the overlapping piece parts if they are
     * used by wanted files and have some complete blocks in them. */
    fpsave = ( !tor->info.pieces[fpindex].dnd && fpblocks > 0 );
    lpsave = ( !tor->info.pieces[lpindex].dnd && lpblocks > 0
               && fpindex != lpindex );

    /* Ensure that the data we are about to delete does not
     * remain in the cache. */
    tr_cacheFlushFile( tor->session->cache, tor, file_index );

    path = tr_torrentFindFile( tor, file_index );
    if( !path )
    {
        /* The file is already gone for some reason. */
        file->exists = FALSE;
        return TRUE;
    }

    /* Read the existing overlapping piece parts. */
    if( fpsave )
    {
        fpbuf = tr_malloc( fpoverlap );
        if( tr_ioRead( tor, fpindex, fpoffset, fpoverlap, fpbuf ) != 0 )
        {
            tr_free( fpbuf );
            fpsave = FALSE;
        }
    }
    if( lpsave )
    {
        lpbuf = tr_malloc( lpoverlap );
        if( tr_ioRead( tor, lpindex, lpoffset, lpoverlap, lpbuf ) != 0 )
        {
            tr_free( lpbuf );
            lpsave = FALSE;
        }
    }

    /* Close and delete the file from the file system. */
    tr_fdFileClose( tor->session, tor, file_index, TR_FD_INDEX_FILE );
    deleteLocalFile( path, removeFunc );
    tr_free( path );
    file->exists = FALSE;

    /* Make subsequent writes to temporary piece files, if needed. */
    file->usept = TRUE;

    /* Write the overlapping piece parts back from the buffers. */
    if( fpsave )
    {
        tr_ioWrite( tor, fpindex, fpoffset, fpoverlap, fpbuf );
        tr_free( fpbuf );
    }
    if( lpsave )
    {
        tr_ioWrite( tor, lpindex, lpoffset, lpoverlap, lpbuf );
        tr_free( lpbuf );
    }

    /* Update the piece status of the deleted pieces. */
    for( pi = fpindex; pi <= lpindex; ++pi )
        if( tor->info.pieces[pi].dnd )
            tr_torrentSetHasPiece( tor, pi, FALSE );

    /* Scan for temporary piece files we can remove. */
    fpallpt = file->usept;
    if( file_index > 0 )
    {
        for( fi = file_index - 1; fpallpt; --fi )
        {
            if( tor->info.files[fi].lastPiece != fpindex )
                break;
            fpallpt = tor->info.files[fi].usept;
            if( !fi )
                break;
        }
    }

    lpallpt = file->usept;
    for( fi = file_index + 1; lpallpt && fi < tor->info.fileCount; ++fi )
    {
        if( tor->info.files[fi].firstPiece != lpindex )
            break;
        lpallpt = tor->info.files[fi].usept;
    }

    if( fpindex == lpindex )
    {
        if( fpallpt && lpallpt )
        {
            tor->info.pieces[fpindex].dnd = TRUE;
            tr_torrentSetHasPiece( tor, fpindex, FALSE );
            removePieceTemp( tor, fpindex );
        }
    }
    else
    {
        if( fpallpt )
        {
            tor->info.pieces[fpindex].dnd = TRUE;
            tr_torrentSetHasPiece( tor, fpindex, FALSE );
            removePieceTemp( tor, fpindex );
        }
        if( lpallpt )
        {
            tor->info.pieces[lpindex].dnd = TRUE;
            tr_torrentSetHasPiece( tor, lpindex, FALSE );
            removePieceTemp( tor, lpindex );
        }
    }

    return TRUE;
}

/**
 * @note This function assumes it is only called
 *       from tr_torrentSetFileDLsImpl().
 *
 * @return the number of files deleted.
 */
static tr_file_index_t
tr_torrentDeleteDNDFiles( tr_torrent            * tor,
                          const tr_file_index_t * files,
                          tr_file_index_t         fileCount,
                          tr_fileFunc             removeFunc )
{
    tr_file_index_t count = 0, i;

    for( i = 0; i < fileCount; ++i )
        if( deleteDNDFile( tor, files[i], removeFunc ) )
            ++count;

    return count;
}

static void
tr_torrentSetFileDLsImpl( tr_torrent             * tor,
                          const tr_file_index_t  * files,
                          tr_file_index_t          fileCount,
                          tr_bool                  doDownload,
                          tr_bool                  deleteData,
                          tr_file_index_t        * setmeDeleteCount,
                          tr_fileFunc              removeFunc )
{
    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    tr_torrentInitFileDLs( tor, files, fileCount, doDownload );
    if( !doDownload && deleteData )
    {
        tr_file_index_t count;
        count = tr_torrentDeleteDNDFiles( tor, files, fileCount, removeFunc );
        if( setmeDeleteCount )
            *setmeDeleteCount = count;
    }
    tr_torrentSetDirty( tor );
    tr_peerMgrRebuildRequests( tor );

    tr_torrentUnlock( tor );
}

void
tr_torrentSetFileDLs( tr_torrent             * tor,
                      const tr_file_index_t  * files,
                      tr_file_index_t          fileCount,
                      tr_bool                  doDownload )
{
    tr_torrentSetFileDLsImpl( tor, files, fileCount, doDownload,
                              FALSE, NULL, NULL );
}

tr_file_index_t
tr_torrentDeleteFiles( tr_torrent            * torrent,
                       const tr_file_index_t * files,
                       tr_file_index_t         fileCount,
                       tr_fileFunc             removeFunc )
{
    tr_file_index_t count = 0;
    tr_torrentSetFileDLsImpl( torrent, files, fileCount, FALSE,
                              TRUE, &count, removeFunc );
    return count;
}

/***
****
***/

tr_priority_t
tr_torrentGetPriority( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->bandwidth->priority;
}

void
tr_torrentSetPriority( tr_torrent * tor, tr_priority_t priority )
{
    assert( tr_isTorrent( tor ) );
    assert( tr_isPriority( priority ) );

    if( tor->bandwidth->priority != priority )
    {
        tor->bandwidth->priority = priority;

        tr_torrentSetDirty( tor );
    }
}

/***
****
***/

void
tr_torrentSetPeerLimit( tr_torrent * tor,
                        uint16_t     maxConnectedPeers )
{
    assert( tr_isTorrent( tor ) );

    if ( tor->maxConnectedPeers != maxConnectedPeers )
    {
        tor->maxConnectedPeers = maxConnectedPeers;

        tr_torrentSetDirty( tor );
    }
}

uint16_t
tr_torrentGetPeerLimit( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->maxConnectedPeers;
}

/***
****
***/

tr_block_index_t
_tr_block( const tr_torrent * tor,
           tr_piece_index_t   index,
           uint32_t           offset )
{
    tr_block_index_t ret;

    assert( tr_isTorrent( tor ) );
    assert( offset < tr_torPieceCountBytes( tor, index ) );

    ret = tr_torPieceFirstBlock( tor, index );
    ret += offset / tor->block_size;
    return ret;
}

tr_bool
tr_torrentReqIsValid( const tr_torrent * tor,
                      tr_piece_index_t   index,
                      uint32_t           offset,
                      uint32_t           length )
{
    int err = 0;

    assert( tr_isTorrent( tor ) );

    if( index >= tor->info.pieceCount )
        err = 1;
    else if( length < 1 )
        err = 2;
    else if( ( offset + length ) > tr_torPieceCountBytes( tor, index ) )
        err = 3;
    else if( length > MAX_BLOCK_SIZE )
        err = 4;
    else if( tr_pieceOffset( tor, index, offset, length ) > tor->info.totalSize )
        err = 5;

    if( err ) tr_tordbg( tor, "index %lu offset %lu length %lu err %d\n",
                              (unsigned long)index,
                              (unsigned long)offset,
                              (unsigned long)length,
                              err );

    return !err;
}

uint64_t
tr_pieceOffset( const tr_torrent * tor,
                tr_piece_index_t   index,
                uint32_t           offset,
                uint32_t           length )
{
    uint64_t ret;

    assert( tr_isTorrent( tor ) );

    ret = tor->info.pieceSize;
    ret *= index;
    ret += offset;
    ret += length;
    return ret;
}

/***
****
***/

tr_bool
tr_torrentCheckPiece( tr_torrent * tor, tr_piece_index_t pieceIndex )
{
    const tr_bool pass = tr_ioTestPiece( tor, pieceIndex );

    tr_torrentSetHasPiece( tor, pieceIndex, pass );
    tor->anyDate = tr_time( );
    tr_torrentSetDirty( tor );

    return pass;
}

/***
****
***/

static int
compareTrackerByTier( const void * va, const void * vb )
{
    const tr_tracker_info * a = va;
    const tr_tracker_info * b = vb;

    /* sort by tier */
    if( a->tier != b->tier )
        return a->tier - b->tier;

    /* get the effects of a stable sort by comparing the two elements' addresses */
    if( a < b )
        return -1;
    if( a > b )
        return 1;
    return 0;
}

tr_bool
tr_torrentSetAnnounceList( tr_torrent             * tor,
                           const tr_tracker_info  * trackers_in,
                           int                      trackerCount )
{
    int i;
    tr_benc metainfo;
    tr_bool ok = TRUE;
    tr_tracker_info * trackers;

    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    /* ensure the trackers' tiers are in ascending order */
    trackers = tr_memdup( trackers_in, sizeof( tr_tracker_info ) * trackerCount );
    qsort( trackers, trackerCount, sizeof( tr_tracker_info ), compareTrackerByTier );

    /* look for bad URLs */
    for( i=0; ok && i<trackerCount; ++i )
        if( !tr_urlIsValidTracker( trackers[i].announce ) )
            ok = FALSE;

    /* save to the .torrent file */
    if( ok && !tr_bencLoadFile( &metainfo, TR_FMT_BENC, tor->info.torrent ) )
    {
        tr_bool hasInfo;
        tr_info tmpInfo;

        /* remove the old fields */
        tr_bencDictRemove( &metainfo, "announce" );
        tr_bencDictRemove( &metainfo, "announce-list" );

        /* add the new fields */
        if( trackerCount > 0 )
        {
            tr_bencDictAddStr( &metainfo, "announce", trackers[0].announce );
        }
        if( trackerCount > 1 )
        {
            int i;
            int prevTier = -1;
            tr_benc * tier = NULL;
            tr_benc * announceList = tr_bencDictAddList( &metainfo, "announce-list", 0 );

            for( i=0; i<trackerCount; ++i ) {
                if( prevTier != trackers[i].tier ) {
                    prevTier = trackers[i].tier;
                    tier = tr_bencListAddList( announceList, 0 );
                }
                tr_bencListAddStr( tier, trackers[i].announce );
            }
        }

        /* try to parse it back again, to make sure it's good */
        memset( &tmpInfo, 0, sizeof( tr_info ) );
        if( tr_metainfoParse( tor->session, &metainfo, &tmpInfo,
                              &hasInfo, &tor->infoDictLength ) )
        {
            /* it's good, so keep these new trackers and free the old ones */

            tr_info swap;
            swap.trackers = tor->info.trackers;
            swap.trackerCount = tor->info.trackerCount;
            tor->info.trackers = tmpInfo.trackers;
            tor->info.trackerCount = tmpInfo.trackerCount;
            tmpInfo.trackers = swap.trackers;
            tmpInfo.trackerCount = swap.trackerCount;

            tr_metainfoFree( &tmpInfo );
            tr_bencToFile( &metainfo, TR_FMT_BENC, tor->info.torrent );
        }

        /* cleanup */
        tr_bencFree( &metainfo );

        /* if we had a tracker-related error on this torrent,
         * and that tracker's been removed,
         * then clear the error */
        if(    ( tor->error == TR_STAT_TRACKER_WARNING )
            || ( tor->error == TR_STAT_TRACKER_ERROR ) )
        {
            tr_bool clear = TRUE;

            for( i=0; clear && i<trackerCount; ++i )
                if( !strcmp( trackers[i].announce, tor->errorTracker ) )
                    clear = FALSE;

            if( clear )
                tr_torrentClearError( tor );
        }

        /* tell the announcer to reload this torrent's tracker list */
        tr_announcerResetTorrent( tor->session->announcer, tor );
    }

    tr_torrentUnlock( tor );

    tr_free( trackers );
    return ok;
}

/**
***
**/

void
tr_torrentSetAddedDate( tr_torrent * tor,
                        time_t       t )
{
    assert( tr_isTorrent( tor ) );

    tor->addedDate = t;
    tor->anyDate = MAX( tor->anyDate, tor->addedDate );
}

void
tr_torrentSetActivityDate( tr_torrent * tor, time_t t )
{
    assert( tr_isTorrent( tor ) );

    tor->activityDate = t;
    tor->anyDate = MAX( tor->anyDate, tor->activityDate );
}

void
tr_torrentSetDoneDate( tr_torrent * tor,
                       time_t       t )
{
    assert( tr_isTorrent( tor ) );

    tor->doneDate = t;
    tor->anyDate = MAX( tor->anyDate, tor->doneDate );
}

/**
***
**/

uint64_t
tr_torrentGetBytesLeftToAllocate( const tr_torrent * tor )
{
    tr_file_index_t i;
    uint64_t bytesLeft = 0;

    assert( tr_isTorrent( tor ) );

    for( i=0; i<tor->info.fileCount; ++i )
    {
        if( !tor->info.files[i].dnd )
        {
            struct stat sb;
            const uint64_t length = tor->info.files[i].length;
            char * path = tr_torrentFindFile( tor, i );

            bytesLeft += length;

            if( ( path != NULL ) && !stat( path, &sb )
                                 && S_ISREG( sb.st_mode )
                                 && ( (uint64_t)sb.st_size <= length ) )
                bytesLeft -= sb.st_size;

            tr_free( path );
        }
    }

    return bytesLeft;
}

/****
*****  Removing the torrent's local data
****/

static int
compareLongestFirst( const void * a, const void * b )
{
    const size_t alen = strlen( a );
    const size_t blen = strlen( b );

    if( alen != blen )
        return alen > blen ? -1 : 1;

    return vstrcmp( a, b );
}

static void
addDirtyFile( const char  * root,
              const char  * filename,
              tr_ptrArray * dirtyFolders )
{
    char * dir = tr_dirname( filename );

    /* add the parent folders to dirtyFolders until we reach the root or a known-dirty */
    while (     ( dir != NULL )
             && ( strlen( root ) <= strlen( dir ) )
             && ( tr_ptrArrayFindSorted( dirtyFolders, dir, vstrcmp ) == NULL ) )
    {
        char * tmp;
        tr_ptrArrayInsertSorted( dirtyFolders, tr_strdup( dir ), vstrcmp );

        tmp = tr_dirname( dir );
        tr_free( dir );
        dir = tmp;
    }

    tr_free( dir );
}

static void
walkLocalData( const tr_torrent * tor,
               const char       * root,
               const char       * dir,
               const char       * base,
               tr_ptrArray      * torrentFiles,
               tr_ptrArray      * folders,
               tr_ptrArray      * dirtyFolders )
{
    struct stat sb;
    char * buf = tr_buildPath( dir, base, NULL );
    int i = stat( buf, &sb );

    if( !i )
    {
        DIR * odir = NULL;

        if( S_ISDIR( sb.st_mode ) && ( ( odir = opendir ( buf ) ) ) )
        {
            struct dirent *d;
            tr_ptrArrayInsertSorted( folders, tr_strdup( buf ), vstrcmp );
            for( d = readdir( odir ); d != NULL; d = readdir( odir ) )
                if( d->d_name && strcmp( d->d_name, "." ) && strcmp( d->d_name, ".." ) )
                    walkLocalData( tor, root, buf, d->d_name, torrentFiles, folders, dirtyFolders );
            closedir( odir );
        }
        else if( S_ISREG( sb.st_mode ) && ( sb.st_size > 0 ) )
        {
            const char * sub = buf + strlen( tor->currentDir ) + strlen( TR_PATH_DELIMITER_STR );
            const tr_bool isTorrentFile = tr_ptrArrayFindSorted( torrentFiles, sub, vstrcmp ) != NULL;
            if( !isTorrentFile )
                addDirtyFile( root, buf, dirtyFolders );
        }
    }

    tr_free( buf );
}

static void
deleteLocalData( tr_torrent * tor, tr_fileFunc fileFunc )
{
    int i, n;
    char ** s;
    tr_file_index_t f;
    tr_ptrArray torrentFiles = TR_PTR_ARRAY_INIT;
    tr_ptrArray folders      = TR_PTR_ARRAY_INIT;
    tr_ptrArray dirtyFolders = TR_PTR_ARRAY_INIT; /* dirty == contains non-torrent files */

    const char * firstFile = tor->info.files[0].name;
    const char * cpch = strchr( firstFile, TR_PATH_DELIMITER );
    char * tmp = cpch ? tr_strndup( firstFile, cpch - firstFile ) : NULL;
    char * root = tr_buildPath( tor->currentDir, tmp, NULL );

    for( f=0; f<tor->info.fileCount; ++f ) {
        tr_ptrArrayInsertSorted( &torrentFiles, tr_strdup( tor->info.files[f].name ), vstrcmp );
        tr_ptrArrayInsertSorted( &torrentFiles, tr_torrentBuildPartial( tor, f ), vstrcmp );
    }

    /* build the set of folders and dirtyFolders */
    walkLocalData( tor, root, root, NULL, &torrentFiles, &folders, &dirtyFolders );

    /* try to remove entire folders first, so that the recycle bin will be tidy */
    s = (char**) tr_ptrArrayPeek( &folders, &n );
    for( i=0; i<n; ++i )
        if( tr_ptrArrayFindSorted( &dirtyFolders, s[i], vstrcmp ) == NULL )
            deleteLocalFile( s[i], fileFunc );

    /* now blow away any remaining torrent files, such as torrent files in dirty folders */
    for( i=0, n=tr_ptrArraySize( &torrentFiles ); i<n; ++i ) {
        char * path = tr_buildPath( tor->currentDir, tr_ptrArrayNth( &torrentFiles, i ), NULL );
        deleteLocalFile( path, fileFunc );
        tr_free( path );
    }

    /* Now clean out the directories left empty from the previous step.
     * Work from deepest to shallowest s.t. lower folders
     * won't prevent the upper folders from being deleted */
    {
        tr_ptrArray cleanFolders = TR_PTR_ARRAY_INIT;
        s = (char**) tr_ptrArrayPeek( &folders, &n );
        for( i=0; i<n; ++i )
            if( tr_ptrArrayFindSorted( &dirtyFolders, s[i], vstrcmp ) == NULL )
                tr_ptrArrayInsertSorted( &cleanFolders, s[i], compareLongestFirst );
        s = (char**) tr_ptrArrayPeek( &cleanFolders, &n );
        for( i=0; i<n; ++i ) {
#ifdef SYS_DARWIN
            char * dsStore = tr_buildPath( s[i], ".DS_Store", NULL );
            deleteLocalFile( dsStore, fileFunc );
            tr_free( dsStore );
#endif
            deleteLocalFile( s[i], fileFunc );
        }
        tr_ptrArrayDestruct( &cleanFolders, NULL );
    }

    /* cleanup */
    tr_ptrArrayDestruct( &dirtyFolders, tr_free );
    tr_ptrArrayDestruct( &folders, tr_free );
    tr_ptrArrayDestruct( &torrentFiles, tr_free );
    tr_free( root );
    tr_free( tmp );
}

static void
tr_torrentDeleteLocalData( tr_torrent * tor, tr_fileFunc fileFunc )
{
    assert( tr_isTorrent( tor ) );

    if( fileFunc == NULL )
        fileFunc = remove;

    /* close all the files because we're about to delete them */
    tr_cacheFlushTorrent( tor->session->cache, tor );
    tr_fdTorrentClose( tor->session, tor->uniqueId );

    if( tor->info.fileCount > 1 )
    {
        deleteLocalData( tor, fileFunc );
    }
    else if( tor->info.fileCount == 1 )
    {
        char * tmp;

        /* torrent only has one file */
        char * path = tr_buildPath( tor->currentDir, tor->info.files[0].name, NULL );
        deleteLocalFile( path, fileFunc );
        tr_free( path );

        tmp = tr_torrentBuildPartial( tor, 0 );
        path = tr_buildPath( tor->currentDir, tmp, NULL );
        deleteLocalFile( path, fileFunc );
        tr_free( path );
        tr_free( tmp );
    }
}

/***
****
***/

struct LocationData
{
    tr_bool move_from_old_location;
    volatile int * setme_state;
    volatile double * setme_progress;
    char * location;
    tr_torrent * tor;
};

static void
setLocation( void * vdata )
{
    tr_bool err = FALSE;
    struct LocationData * data = vdata;
    tr_torrent * tor = data->tor;
    const tr_bool do_move = data->move_from_old_location;
    const char * location = data->location;
    double bytesHandled = 0;

    assert( tr_isTorrent( tor ) );

    tr_dbg( "Moving \"%s\" location from currentDir \"%s\" to \"%s\"",
            tr_torrentName(tor), tor->currentDir, location );

    tr_mkdirp( location, 0777 );

    if( !tr_is_same_file( location, tor->currentDir ) )
    {
        tr_file_index_t i;

        /* bad idea to move files while they're being verified... */
        tr_verifyRemove( tor );

        /* try to move the files.
         * FIXME: there are still all kinds of nasty cases, like what
         * if the target directory runs out of space halfway through... */
        for( i=0; !err && i<tor->info.fileCount; ++i )
        {
            const tr_file * f = &tor->info.files[i];
            const char * oldbase;
            char * sub;
            if( tr_torrentFindFile2( tor, i, &oldbase, &sub ) )
            {
                char * oldpath = tr_buildPath( oldbase, sub, NULL );
                char * newpath = tr_buildPath( location, sub, NULL );

                tr_dbg( "Found file #%d: %s", (int)i, oldpath );

                if( do_move && !tr_is_same_file( oldpath, newpath ) )
                {
                    tr_bool renamed = FALSE;
                    errno = 0;
                    tr_torinf( tor, "moving \"%s\" to \"%s\"", oldpath, newpath );
                    if( tr_moveFile( oldpath, newpath, &renamed ) )
                    {
                        err = TRUE;
                        tr_torerr( tor, "error moving \"%s\" to \"%s\": %s",
                                        oldpath, newpath, tr_strerror( errno ) );
                    }
                }

                tr_free( newpath );
                tr_free( oldpath );
                tr_free( sub );
            }

            if( data->setme_progress )
            {
                bytesHandled += f->length;
                *data->setme_progress = bytesHandled / tor->info.totalSize;
            }
        }

        if( !err )
        {
            /* blow away the leftover subdirectories in the old location */
            if( do_move )
                tr_torrentDeleteLocalData( tor, remove );

            /* set the new location and reverify */
            tr_torrentSetDownloadDir( tor, location );
        }
    }

    if( !err && do_move )
    {
        tr_free( tor->incompleteDir );
        tor->incompleteDir = NULL;
        tor->currentDir = tor->downloadDir;
    }

    if( data->setme_state )
        *data->setme_state = err ? TR_LOC_ERROR : TR_LOC_DONE;

    /* cleanup */
    tr_free( data->location );
    tr_free( data );
}

void
tr_torrentSetLocation( tr_torrent       * tor,
                       const char       * location,
                       tr_bool             move_from_old_location,
                       volatile double  * setme_progress,
                       volatile int     * setme_state )
{
    struct LocationData * data;

    assert( tr_isTorrent( tor ) );

    if( setme_state )
        *setme_state = TR_LOC_MOVING;
    if( setme_progress )
        *setme_progress = 0;

    /* run this in the libtransmission thread */
    data = tr_new( struct LocationData, 1 );
    data->tor = tor;
    data->location = tr_strdup( location );
    data->move_from_old_location = move_from_old_location;
    data->setme_state = setme_state;
    data->setme_progress = setme_progress;
    tr_runInEventThread( tor->session, setLocation, data );
}

static tr_bool
dirExists( const char * path )
{
    struct stat sb;
    return stat( path, &sb ) == 0 && S_ISDIR( sb.st_mode );
}

static tr_bool
fileExists( const char * filename )
{
    struct stat sb;
    return stat( filename, &sb ) == 0;
}

int
tr_torrentRename( tr_torrent * tor, const char * newname )
{
    tr_info * info;
    const char * root, * p, * oldname, * base;
    char * oldpath = NULL, * newpath = NULL, * subpath = NULL;
    int err = 0;

    assert( tr_isTorrent( tor ) );
    tr_torrentLock( tor );

    if( !tr_torrentHasMetadata( tor ) )
    {
        err = ENOENT;
        goto OUT;
    }

    if( !newname || !newname[0] || strchr( newname, TR_PATH_DELIMITER )
        || !strcmp( newname, "." ) || !strcmp( newname,  ".." ) )
    {
        err = EINVAL;
        goto OUT;
    }

    info = &tor->info;
    oldname = tr_torrentName( tor );
    if( ( p = strchr( oldname, TR_PATH_DELIMITER ) ) )
    {
        /* Should not happen, but just in case. */
        err = EISDIR;
        goto OUT;
    }
    if( !strcmp( newname, oldname ) )
        goto OUT;

    root = tr_torrentGetCurrentDir( tor );

    if( info->fileCount > 1 )
    {
        tr_file_index_t fi;
        oldpath = tr_buildPath( root, oldname, NULL );
        if( dirExists( oldpath ) )
        {
            newpath = tr_buildPath( root, newname, NULL );
            if( fileExists( newpath ) )
            {
                err = EEXIST;
                goto OUT;
            }
            if( rename( oldpath, newpath ) == -1 )
            {
                err = errno;
                goto OUT;
            }
        }

        for( fi = 0; fi < info->fileCount; ++fi )
        {
            tr_file * file = &info->files[fi];
            char * newfnam;

            if( !( p = strchr( file->name, TR_PATH_DELIMITER ) ) )
                continue;
            newfnam = tr_buildPath( newname, p + 1, NULL );
            tr_free( file->name );
            file->name = newfnam;
        }
    }
    else
    {
        if( tr_torrentFindFile2( tor, 0, &base, &subpath ) )
        {
            oldpath = tr_buildPath( base, subpath, NULL );
            newpath = tr_buildPath( base, newname, NULL );
            if( fileExists( newpath ) )
            {
                err = EEXIST;
                goto OUT;
            }
            if( rename( oldpath, newpath ) == -1 )
            {
                err = errno;
                goto OUT;
            }
        }
        tr_free( info->files[0].name );
        info->files[0].name = tr_strdup( newname );
    }

    tr_free( info->rename );
    if( !strcmp( newname, info->name ) )
        info->rename = NULL;
    else
        info->rename = tr_strdup( newname );
    tr_torrentSetDirty( tor );

OUT:
    if( err )
    {
        const char * es = tr_strerror( err ), * fmt;
        if( oldpath && newpath )
        {
            /* %1$s is the original file path.
             * %2$s is the new file path.
             * %3$s is the error message. */
            fmt = _( "Cannot rename \"%1$s\" to \"%2$s\": %3$s" );
            tr_torerr( tor, fmt, oldpath, newpath, es );
        }
        else if( oldpath )
        {
            /* %1$s is the existing file name.
             * %2$s is the error message. */
            fmt = _( "Cannot rename \"%1$s\": %2$s" );
            tr_torerr( tor, fmt, oldpath, es );
        }
        else
        {
            fmt = _( "Cannot rename torrent: %s" );
            tr_torerr( tor, fmt, es );
        }
    }
    tr_torrentUnlock( tor );
    tr_free( oldpath );
    tr_free( newpath );
    tr_free( subpath );
    return err;
}

/***
****
***/

void
tr_torrentFileCompleted( tr_torrent * tor, tr_file_index_t fileNum )
{
    char * sub;
    const char * base;
    const tr_info * inf = &tor->info;
    const tr_file * f = &inf->files[fileNum];

    /* close the file so that we can reopen in read-only mode as needed */
    tr_fdFileClose( tor->session, tor, fileNum, TR_FD_INDEX_FILE );

    /* if the torrent's current filename isn't the same as the one in the
     * metadata -- for example, if it had the ".part" suffix appended to
     * it until now -- then rename it to match the one in the metadata */
    if( tr_torrentFindFile2( tor, fileNum, &base, &sub ) )
    {
        if( strcmp( sub, f->name ) )
        {
            char * oldpath = tr_buildPath( base, sub, NULL );
            char * newpath = tr_buildPath( base, f->name, NULL );

            if( rename( oldpath, newpath ) )
                tr_torerr( tor, "Error moving \"%s\" to \"%s\": %s", oldpath, newpath, tr_strerror( errno ) );

            tr_free( newpath );
            tr_free( oldpath );
        }

        tr_free( sub );
    }
}

/***
****
***/

tr_bool
tr_torrentFindFile2( const tr_torrent * tor, tr_file_index_t fileNum,
                     const char ** base, char ** subpath )
{
    char * part;
    const tr_file * file;
    const char * b = NULL;
    const char * s = NULL;

    assert( tr_isTorrent( tor ) );
    assert( fileNum < tor->info.fileCount );

    file = &tor->info.files[fileNum];
    part = tr_torrentBuildPartial( tor, fileNum );

    if( b == NULL ) {
        char * filename = tr_buildPath( tor->downloadDir, file->name, NULL );
        if( fileExists( filename ) ) {
            b = tor->downloadDir;
            s = file->name;
        }
        tr_free( filename );
    }

    if( ( b == NULL ) && ( tor->incompleteDir != NULL ) ) {
        char * filename = tr_buildPath( tor->incompleteDir, file->name, NULL );
        if( fileExists( filename ) ) {
            b = tor->incompleteDir;
            s = file->name;
        }
        tr_free( filename );
    }

    if( ( b == NULL ) && ( tor->incompleteDir != NULL ) ) {
        char * filename = tr_buildPath( tor->incompleteDir, part, NULL );
        if( fileExists( filename ) ) {
            b = tor->incompleteDir;
            s = part;
        }
        tr_free( filename );
    }

    if( b == NULL) {
        char * filename = tr_buildPath( tor->downloadDir, part, NULL );
        if( fileExists( filename ) ) {
            b = tor->downloadDir;
            s = part;
        }
        tr_free( filename );
    }

    if( base != NULL )
        *base = b;
    if( subpath != NULL )
        *subpath = tr_strdup( s );

    tr_free( part );
    return b != NULL;
}

char*
tr_torrentFindFile( const tr_torrent * tor, tr_file_index_t fileNum )
{
    char * subpath;
    char * ret = NULL;
    const char * base;

    if( tr_torrentFindFile2( tor, fileNum, &base, &subpath ) )
    {
        ret = tr_buildPath( base, subpath, NULL );
        tr_free( subpath );
    }

    return ret;
}

/* Decide whether we should be looking for files in downloadDir or incompleteDir. */
static void
refreshCurrentDir( tr_torrent * tor )
{
    const char * dir = NULL;

    if( tor->incompleteDir == NULL )
        dir = tor->downloadDir;
    else if( !tr_torrentHasMetadata( tor ) ) /* no files to find */
        dir = tor->incompleteDir;
    else if( !tr_torrentFindFile2( tor, 0, &dir, NULL ) )
        dir = tor->incompleteDir;

    assert( dir != NULL );
    assert( ( dir == tor->downloadDir ) || ( dir == tor->incompleteDir ) );
    tor->currentDir = dir;
}

char*
tr_torrentBuildPartial( const tr_torrent * tor, tr_file_index_t fileNum )
{
    return tr_strdup_printf( "%s.part", tor->info.files[fileNum].name );
}
