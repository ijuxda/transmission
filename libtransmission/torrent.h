/*
 * This file Copyright (C) 2009-2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#ifndef __TRANSMISSION__
 #error only libtransmission should #include this header.
#endif

#ifndef TR_TORRENT_H
#define TR_TORRENT_H 1

#include "completion.h" /* tr_completion */
#include "session.h" /* tr_sessionLock(), tr_sessionUnlock() */
#include "utils.h" /* TR_GNUC_PRINTF */

struct tr_bandwidth;
struct tr_torrent_tiers;
struct tr_magnet_info;

/**
***  Package-visible ctor API
**/

void        tr_ctorSetSave( tr_ctor * ctor,
                            tr_bool   saveMetadataInOurTorrentsDir );

int         tr_ctorGetSave( const tr_ctor * ctor );

void        tr_ctorInitTorrentPriorities( const tr_ctor * ctor, tr_torrent * tor );

void        tr_ctorInitTorrentWanted( const tr_ctor * ctor, tr_torrent * tor );

/**
***
**/

/* just like tr_torrentSetFileDLs but doesn't trigger a fastresume save */
void        tr_torrentInitFileDLs( tr_torrent              * tor,
                                   const tr_file_index_t   * files,
                                   tr_file_index_t          fileCount,
                                   tr_bool                  do_download );

void        tr_torrentRecheckCompleteness( tr_torrent * );

void        tr_torrentSetHasPiece( tr_torrent *     tor,
                                   tr_piece_index_t pieceIndex,
                                   tr_bool          has );

void        tr_torrentChangeMyPort( tr_torrent * session );

tr_torrent* tr_torrentFindFromHashString( tr_session * session,
                                          const char * hashString );

tr_torrent* tr_torrentFindFromObfuscatedHash( tr_session    * session,
                                              const uint8_t * hash );

tr_bool     tr_torrentIsPieceTransferAllowed( const tr_torrent * torrent,
                                              tr_direction       direction );



#define tr_block( a, b ) _tr_block( tor, a, b )
tr_block_index_t _tr_block( const tr_torrent * tor,
                            tr_piece_index_t   index,
                            uint32_t           offset );

tr_bool          tr_torrentReqIsValid( const tr_torrent * tor,
                                       tr_piece_index_t   index,
                                       uint32_t           offset,
                                       uint32_t           length );

uint64_t         tr_pieceOffset( const tr_torrent * tor,
                                 tr_piece_index_t   index,
                                 uint32_t           offset,
                                 uint32_t           length );

void             tr_torrentInitFilePriority( tr_torrent       * tor,
                                             tr_file_index_t    fileIndex,
                                             tr_priority_t      priority );

void             tr_torrentSetPieceChecked( tr_torrent       * tor,
                                            tr_piece_index_t   piece );

void             tr_torrentSetChecked( tr_torrent * tor, time_t when );

tr_torrent*      tr_torrentNext( tr_session  * session,
                                 tr_torrent  * current );

void             tr_torrentCheckSeedLimit( tr_torrent * tor );

/** save a torrent's .resume file if it's changed since the last time it was saved */
void             tr_torrentSave( tr_torrent * tor );

void             tr_torrentSetLocalError( tr_torrent * tor, const char * fmt, ... ) TR_GNUC_PRINTF( 2, 3 );



typedef enum
{
    TR_VERIFY_NONE,
    TR_VERIFY_WAIT,
    TR_VERIFY_NOW
}
tr_verify_state;

void             tr_torrentSetVerifyState( tr_torrent      * tor,
                                           tr_verify_state   state );

tr_torrent_activity tr_torrentGetActivity( tr_torrent * tor );

struct tr_incomplete_metadata;

/** @brief Torrent object */
struct tr_torrent
{
    tr_session *             session;
    tr_info                  info;

    int                      magicNumber;

    tr_stat_errtype          error;
    char                     errorString[128];
    char                     errorTracker[128];

    uint8_t                  obfuscatedHash[SHA_DIGEST_LENGTH];

    /* Used when the torrent has been created with a magnet link
     * and we're in the process of downloading the metainfo from
     * other peers */
    struct tr_incomplete_metadata  * incompleteMetadata;

    /* If the initiator of the connection receives a handshake in which the
     * peer_id does not match the expected peerid, then the initiator is
     * expected to drop the connection. Note that the initiator presumably
     * received the peer information from the tracker, which includes the
     * peer_id that was registered by the peer. The peer_id from the tracker
     * and in the handshake are expected to match.
     */
    uint8_t * peer_id;

    /* Where the files will be when it's complete */
    char * downloadDir;

    /* Where the files are when the torrent is incomplete */
    char * incompleteDir;

    /* Where temporary piece files are stored. */
    char * pieceTempDir;

    /* Length, in bytes, of the "info" dict in the .torrent file. */
    int infoDictLength;

    /* Offset, in bytes, of the beginning of the "info" dict in the .torrent file.
     *
     * Used by the torrent-magnet code for serving metainfo to peers.
     * This field is lazy-generated and might not be initialized yet. */
    int infoDictOffset;

    /* Where the files are now.
     * This pointer will be equal to downloadDir or incompleteDir */
    const char * currentDir;

    /* How many bytes we ask for per request */
    uint32_t                   blockSize;
    tr_block_index_t           blockCount;

    uint32_t                   lastBlockSize;
    uint32_t                   lastPieceSize;

    uint16_t                   blockCountInPiece;
    uint16_t                   blockCountInLastPiece;

    struct tr_completion       completion;

    tr_completeness            completeness;

    struct tr_torrent_tiers  * tiers;

    time_t                     dhtAnnounceAt;
    time_t                     dhtAnnounce6At;
    tr_bool                    dhtAnnounceInProgress;
    tr_bool                    dhtAnnounce6InProgress;

    time_t                     lpdAnnounceAt;

    uint64_t                   downloadedCur;
    uint64_t                   downloadedPrev;
    uint64_t                   uploadedCur;
    uint64_t                   uploadedPrev;
    uint64_t                   corruptCur;
    uint64_t                   corruptPrev;

    uint64_t                   etaDLSpeedCalculatedAt;
    double                     etaDLSpeed_KBps;
    uint64_t                   etaULSpeedCalculatedAt;
    double                     etaULSpeed_KBps;

    time_t                     addedDate;
    time_t                     activityDate;
    time_t                     doneDate;
    time_t                     startDate;
    time_t                     anyDate;

    time_t                     secondsDownloading;
    time_t                     secondsSeeding;

    tr_torrent_metadata_func  * metadata_func;
    void                      * metadata_func_user_data;

    tr_torrent_completeness_func  * completeness_func;
    void                          *  completeness_func_user_data;

    tr_torrent_ratio_limit_hit_func  * ratio_limit_hit_func;
    void                             * ratio_limit_hit_func_user_data;

    tr_torrent_idle_limit_hit_func  * idle_limit_hit_func;
    void                            * idle_limit_hit_func_user_data;

    tr_bool                    isRunning;
    tr_bool                    isStopping;
    tr_bool                    isDeleting;
    tr_bool                    startAfterVerify;
    tr_bool                    isDirty;

    tr_bool                    infoDictOffsetIsCached;

    uint16_t                   maxConnectedPeers;

    tr_verify_state            verifyState;

    time_t                     lastStatTime;
    tr_stat                    stats;

    tr_torrent *               next;

    int                        uniqueId;

    struct tr_bandwidth      * bandwidth;

    struct tr_torrent_peers  * torrentPeers;

    double                     desiredRatio;
    tr_ratiolimit              ratioLimitMode;

    uint16_t                   idleLimitMinutes;
    tr_idlelimit               idleLimitMode;
    tr_bool                    finishedSeedingByIdle;
};

/* get the index of this piece's first block */
static inline tr_block_index_t
tr_torPieceFirstBlock( const tr_torrent * tor, const tr_piece_index_t piece )
{
    return piece * tor->blockCountInPiece;
}

/* what piece index is this block in? */
static inline tr_piece_index_t
tr_torBlockPiece( const tr_torrent * tor, const tr_block_index_t block )
{
    return block / tor->blockCountInPiece;
}

/* how many blocks are in this piece? */
static inline uint16_t
tr_torPieceCountBlocks( const tr_torrent * tor, const tr_piece_index_t piece )
{
    if( piece + 1 == tor->info.pieceCount )
        return tor->blockCountInLastPiece;
    else
        return tor->blockCountInPiece;
}

/* how many bytes are in this piece? */
static inline uint32_t
tr_torPieceCountBytes( const tr_torrent * tor, const tr_piece_index_t piece )
{
    return piece == tor->info.pieceCount - 1 ? tor->lastPieceSize
                                             : tor->info.pieceSize;
}

/* how many bytes are in this block? */
static inline uint32_t
tr_torBlockCountBytes( const tr_torrent * tor, const tr_block_index_t block )
{
    return block == tor->blockCount - 1 ? tor->lastBlockSize
                                        : tor->blockSize;
}

static inline void tr_torrentLock( const tr_torrent * tor )
{
    tr_sessionLock( tor->session );
}

static inline void tr_torrentUnlock( const tr_torrent * tor )
{
    tr_sessionUnlock( tor->session );
}

static inline tr_bool
tr_torrentExists( const tr_session * session, const uint8_t *   torrentHash )
{
    return tr_torrentFindFromHash( (tr_session*)session, torrentHash ) != NULL;
}

static inline tr_bool
tr_torrentIsSeed( const tr_torrent * tor )
{
    return tor->completeness != TR_LEECH;
}

static inline tr_bool tr_torrentIsPrivate( const tr_torrent * tor )
{
    return ( tor != NULL ) && tor->info.isPrivate;
}

static inline tr_bool tr_torrentAllowsPex( const tr_torrent * tor )
{
    return ( tor != NULL )
        && ( tor->session->isPexEnabled )
        && ( !tr_torrentIsPrivate( tor ) );
}

static inline tr_bool tr_torrentAllowsDHT( const tr_torrent * tor )
{
    return ( tor != NULL )
        && ( tr_sessionAllowsDHT( tor->session ) )
        && ( !tr_torrentIsPrivate( tor ) );
}

static inline tr_bool tr_torrentAllowsLPD( const tr_torrent * tor )
{
    return ( tor != NULL )
        && ( tr_sessionAllowsLPD( tor->session ) )
        && ( !tr_torrentIsPrivate( tor ) );
}

/***
****
***/

enum
{
    TORRENT_MAGIC_NUMBER = 95549
};

static inline tr_bool tr_isTorrent( const tr_torrent * tor )
{
    return ( tor != NULL )
        && ( tor->magicNumber == TORRENT_MAGIC_NUMBER )
        && ( tr_isSession( tor->session ) );
}

/* set a flag indicating that the torrent's .resume file
 * needs to be saved when the torrent is closed */
static inline
void tr_torrentSetDirty( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    tor->isDirty = TRUE;
}

static inline
const char * tr_torrentName( const tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    return tor->info.name;
}

uint32_t tr_getBlockSize( uint32_t pieceSize );

/**
 * Tell the tr_torrent that one of its files has become complete
 */
void tr_torrentFileCompleted( tr_torrent * tor, tr_file_index_t fileNo );


/**
 * @brief Like tr_torrentFindFile(), but splits the filename into base and subpath;
 *
 * If the file is found, "tr_buildPath( base, subpath, NULL )"
 * will generate the complete filename.
 *
 * @return true if the file is found, false otherwise.
 *
 * @param base if the torrent is found, this will be either
 *             tor->downloadDir or tor->incompleteDir
 * @param subpath on success, this pointer is assigned a newly-allocated
 *                string holding the second half of the filename.
 */
tr_bool tr_torrentFindFile2( const tr_torrent *, tr_file_index_t fileNo,
                             const char ** base, char ** subpath );

/**
 * Get the full path of the temporary piece file for piece
 * with index "pieceIndex".
 *
 * @return a newly allocated string containing the full filename,
 *         or NULL if it does not exist.
 */
char * tr_torrentFindPieceTemp( const tr_torrent * tor,
                                tr_piece_index_t   pieceIndex );

/**
 * @brief Like tr_torrentFindFile2, but for temporary piece files.
 */
tr_bool tr_torrentFindPieceTemp2( const tr_torrent  * tor,
                                  tr_piece_index_t    pieceIndex,
                                  const char       ** base,
                                  char             ** subpath );

/**
 * @brief Get the directory where temporary piece files are stored.
 */
const char * tr_torrentGetPieceTempDir( const tr_torrent * tor );

/**
 * @brief Delete all temporary piece files for the torrent.
 */
void tr_torrentRemovePieceTemp( tr_torrent * tor );

/**
 * @brief All data in temporary pieces files is removed from the torrent.
 */
void tr_torrentInvalidatePieceTemp( tr_torrent * tor );

/**
 * Remove all temporary piece files used by file with index "fileIndex".
 * @note No validation or locking is done on the arguments.
 */
void tr_torrentInvalidatePieceTempFile( tr_torrent      * tor,
                                        tr_file_index_t   fileIndex );

/* Returns a newly-allocated version of the tr_file.name string
 * that's been modified to denote that it's not a complete file yet.
 * In the current implementation this is done by appending ".part"
 * a la Firefox. */
char* tr_torrentBuildPartial( const tr_torrent *, tr_file_index_t fileNo );

/* for when the info dict has been fundamentally changed wrt files,
 * piece size, etc. such as in BEP 9 where peers exchange metadata */
void tr_torrentGotNewInfoDict( tr_torrent * tor );

void tr_torrentSetSpeedLimit_Bps  ( tr_torrent *, tr_direction, int Bps );
int tr_torrentGetSpeedLimit_Bps  ( const tr_torrent *, tr_direction );

/**
 * @return true if this piece needs to be tested
 */
tr_bool tr_torrentPieceNeedsCheck( const tr_torrent * tor, tr_piece_index_t pieceIndex );

/**
 * @brief Test a piece against its info dict checksum
 * @return true if the piece's passes the checksum test
 */
tr_bool tr_torrentCheckPiece( tr_torrent * tor, tr_piece_index_t pieceIndex );

uint64_t tr_torrentGetCurrentSizeOnDisk( const tr_torrent * tor );


#endif
