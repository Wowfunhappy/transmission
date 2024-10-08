/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <errno.h>
#include <string.h> /* strcmp(), strlen(), strncmp() */

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/bufferevent.h>

#include "transmission.h"
#include "clients.h"
#include "crypto-utils.h"
#include "handshake.h"
#include "log.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "session.h"
#include "torrent.h"
#include "tr-assert.h"
#include "tr-dht.h"
#include "utils.h"

/* enable LibTransmission extension protocol */
#define ENABLE_LTEP * /
/* fast extensions */
#define ENABLE_FAST * /
/* DHT */
#define ENABLE_DHT * /

/***
****
***/

#define HANDSHAKE_NAME "\023BitTorrent protocol"

enum
{
    /* BitTorrent Handshake Constants */
    HANDSHAKE_NAME_LEN = 20,
    HANDSHAKE_FLAGS_LEN = 8,
    HANDSHAKE_SIZE = 68,
    INCOMING_HANDSHAKE_LEN = 48,
    /* Encryption Constants */
    PadA_MAXLEN = 512,
    PadB_MAXLEN = 512,
    PadC_MAXLEN = 512,
    PadD_MAXLEN = 512,
    VC_LENGTH = 8,
    CRYPTO_PROVIDE_PLAINTEXT = 1,
    CRYPTO_PROVIDE_CRYPTO = 2,
    /* how long to wait before giving up on a handshake */
    HANDSHAKE_TIMEOUT_SEC = 30
};

#ifdef ENABLE_LTEP
#define HANDSHAKE_HAS_LTEP(bits) (((bits)[5] & 0x10) != 0)
#define HANDSHAKE_SET_LTEP(bits) ((bits)[5] |= 0x10)
#else
#define HANDSHAKE_HAS_LTEP(bits) (false)
#define HANDSHAKE_SET_LTEP(bits) ((void)0)
#endif

#ifdef ENABLE_FAST
#define HANDSHAKE_HAS_FASTEXT(bits) (((bits)[7] & 0x04) != 0)
#define HANDSHAKE_SET_FASTEXT(bits) ((bits)[7] |= 0x04)
#else
#define HANDSHAKE_HAS_FASTEXT(bits) (false)
#define HANDSHAKE_SET_FASTEXT(bits) ((void)0)
#endif

#ifdef ENABLE_DHT
#define HANDSHAKE_HAS_DHT(bits) (((bits)[7] & 0x01) != 0)
#define HANDSHAKE_SET_DHT(bits) ((bits)[7] |= 0x01)
#else
#define HANDSHAKE_HAS_DHT(bits) (false)
#define HANDSHAKE_SET_DHT(bits) ((void)0)
#endif

/**
***
**/

typedef enum
{
    /*
       incoming/outgoing (handshake can be initiated by either) 
       The handshake + peer id is also the first message sent
       after encryption negotiated.
    */
    AWAITING_HANDSHAKE,
    AWAITING_PEER_ID,
    /* incoming connections */
    AWAITING_YA,
    AWAITING_PAD_A,
    AWAITING_CRYPTO_PROVIDE,
    AWAITING_PAD_C,
    AWAITING_IA,
    /* outgoing connections (we wait for other party to respond with) */
    AWAITING_YB,
    AWAITING_VC,
    AWAITING_CRYPTO_SELECT,
    AWAITING_PAD_D,
    /* */
    N_STATES
}
handshake_state_t;

struct tr_handshake
{
    bool haveReadAnythingFromPeer;
    bool havePeerID;
    bool haveSentBitTorrentHandshake;
    tr_peerIo* io;
    tr_crypto* crypto;
    tr_session* session;
    handshake_state_t state;
    tr_encryption_mode encryptionMode;
    uint16_t pad_c_len;
    uint16_t pad_d_len;
    uint16_t ia_len;
    uint32_t crypto_select;
    uint32_t crypto_provide;
    uint8_t myReq1[SHA_DIGEST_LENGTH];
    handshakeDoneCB doneCB;
    void* doneUserData;
    struct event* timeout_timer;
};

/**
***
**/

#define dbgmsg(handshake, ...) tr_logAddDeepNamed(tr_peerIoGetAddrStr((handshake)->io), __VA_ARGS__)

static char const* getStateName(handshake_state_t const state)
{
    static char const* const state_strings[N_STATES] =
    {
        "awaiting handshake", /* AWAITING_HANDSHAKE */
        "awaiting peer id", /* AWAITING_PEER_ID */
        "awaiting ya", /* AWAITING_YA */
        "awaiting pad a", /* AWAITING_PAD_A */
        "awaiting crypto_provide", /* AWAITING_CRYPTO_PROVIDE */
        "awaiting pad c", /* AWAITING_PAD_C */
        "awaiting ia", /* AWAITING_IA */
        "awaiting yb", /* AWAITING_YB */
        "awaiting vc", /* AWAITING_VC */
        "awaiting crypto select", /* AWAITING_CRYPTO_SELECT */
        "awaiting pad d" /* AWAITING_PAD_D */
    };

    return state < N_STATES ? state_strings[state] : "unknown state";
}

static void setState(tr_handshake* handshake, handshake_state_t state)
{
    dbgmsg(handshake, "setting to state [%s]", getStateName(state));
    handshake->state = state;
}

static void setReadState(tr_handshake* handshake, handshake_state_t state)
{
    setState(handshake, state);
}

static bool buildHandshakeMessage(tr_handshake* handshake, uint8_t* buf)
{
    unsigned char const* peer_id = NULL;
    uint8_t const* torrentHash;
    tr_torrent* tor;

    if ((torrentHash = tr_cryptoGetTorrentHash(handshake->crypto)) != NULL)
    {
        if (((tor = tr_torrentFindFromHash(handshake->session, torrentHash)) != NULL) && tor->isRunning)
        {
            peer_id = tr_torrentGetPeerId(tor);
        }
    }

    if (peer_id == NULL)
    {
        return false;
    }

    uint8_t* walk = buf;

    memcpy(walk, HANDSHAKE_NAME, HANDSHAKE_NAME_LEN);
    walk += HANDSHAKE_NAME_LEN;
    memset(walk, 0, HANDSHAKE_FLAGS_LEN);
    HANDSHAKE_SET_LTEP(walk);
    HANDSHAKE_SET_FASTEXT(walk);

    /* Note that this doesn't depend on whether the torrent is private.
     * We don't accept DHT peers for a private torrent,
     * but we participate in the DHT regardless. */
    if (tr_dhtEnabled(handshake->session))
    {
        HANDSHAKE_SET_DHT(walk);
    }

    walk += HANDSHAKE_FLAGS_LEN;
    memcpy(walk, torrentHash, SHA_DIGEST_LENGTH);
    walk += SHA_DIGEST_LENGTH;
    memcpy(walk, peer_id, PEER_ID_LEN);
    walk += PEER_ID_LEN;

    TR_ASSERT(walk - buf == HANDSHAKE_SIZE);

    return true;
}

static ReadState tr_handshakeDone(tr_handshake* handshake, bool isConnected);

/***
****
****  OUTGOING CONNECTIONS
****
***/

/* 1 A->B: Diffie Hellman Ya, PadA */
static void sendYa(tr_handshake* handshake)
{
    int len;
    uint8_t const* public_key;
    char outbuf[KEY_LEN + PadA_MAXLEN];
    char* walk = outbuf;

    /* add our public key (Ya) */
    public_key = tr_cryptoGetMyPublicKey(handshake->crypto, &len);
    TR_ASSERT(len == KEY_LEN);
    TR_ASSERT(public_key != NULL);
    memcpy(walk, public_key, len);
    walk += len;

    /* add some bullshit padding */
    len = tr_rand_int(PadA_MAXLEN);
    tr_rand_buffer(walk, len);
    walk += len;

    /* send it */
    dbgmsg(handshake, "sendYa: Writing %ld bytes", walk - outbuf);
    tr_peerIoWriteBytes(handshake->io, outbuf, walk - outbuf, false);
    setReadState(handshake, AWAITING_YB);
}

static uint32_t getCryptoProvide(tr_handshake const* handshake)
{
    uint32_t provide = 0;

    switch (handshake->encryptionMode)
    {
    case TR_ENCRYPTION_REQUIRED:
    case TR_ENCRYPTION_PREFERRED:
        provide |= CRYPTO_PROVIDE_CRYPTO;
        break;

    // For clear preferred mode, we always make an outgoing plaintext handshake
    // instead. While we could do an MSE handshake and offer plaintext encryption,
    // older versions of Transmission don't properly support downgrading crypto.
    case TR_CLEAR_PREFERRED:
        CHECK(false && "Clear preferred should not be calling into getCryptoProvide.");
        break;
    default:
        CHECK(false && "Unexpected encryption mode in getCryptoProvide.");
        break;
    }


    return provide;
}

static uint32_t getCryptoSelect(tr_handshake const* handshake, uint32_t crypto_provide)
{
    uint32_t choices[2];
    int nChoices = 0;

    switch (handshake->encryptionMode)
    {
    case TR_ENCRYPTION_REQUIRED:
        choices[nChoices++] = CRYPTO_PROVIDE_CRYPTO;
        break;

    case TR_ENCRYPTION_PREFERRED:
        choices[nChoices++] = CRYPTO_PROVIDE_CRYPTO;
        choices[nChoices++] = CRYPTO_PROVIDE_PLAINTEXT;
        break;

    case TR_CLEAR_PREFERRED:
        choices[nChoices++] = CRYPTO_PROVIDE_PLAINTEXT;
        choices[nChoices++] = CRYPTO_PROVIDE_CRYPTO;
        break;
    }

    for (int i = 0; i < nChoices; ++i)
    {
        if ((crypto_provide & choices[i]) != 0)
        {
            return choices[i];
        }
    }

    return 0;
}

static void computeRequestHash(tr_handshake const* handshake, char const* name, uint8_t* hash)
{
    tr_cryptoSecretKeySha1(handshake->crypto, name, 4, NULL, 0, hash);
}

static ReadState readYb(tr_handshake* handshake, struct evbuffer* inbuf)
{
    bool isEncrypted;
    uint8_t yb[KEY_LEN];
    struct evbuffer* outbuf;
    size_t needlen = HANDSHAKE_NAME_LEN;

    if (evbuffer_get_length(inbuf) < needlen)
    {
        return READ_LATER;
    }

    isEncrypted = memcmp(evbuffer_pullup(inbuf, HANDSHAKE_NAME_LEN), HANDSHAKE_NAME, HANDSHAKE_NAME_LEN) != 0;

    if (isEncrypted)
    {
        needlen = KEY_LEN;

        if (evbuffer_get_length(inbuf) < needlen)
        {
            return READ_LATER;
        }
    }

    dbgmsg(handshake, "got an %s handshake", (isEncrypted ? "encrypted" : "plain"));

    if (!isEncrypted)
    {
        setState(handshake, AWAITING_HANDSHAKE);
        return READ_NOW;
    }

    handshake->haveReadAnythingFromPeer = true;

    /* compute the secret */
    evbuffer_remove(inbuf, yb, KEY_LEN);

    if (!tr_cryptoComputeSecret(handshake->crypto, yb))
    {
        return tr_handshakeDone(handshake, false);
    }

    /* now send these: HASH('req1', S), HASH('req2', SKEY) xor HASH('req3', S),
     * ENCRYPT(VC, crypto_provide, len(PadC), PadC, len(IA)), ENCRYPT(IA) */
    outbuf = evbuffer_new();

    /* HASH('req1', S) */
    {
        uint8_t req1[SHA_DIGEST_LENGTH];
        computeRequestHash(handshake, "req1", req1);
        evbuffer_add(outbuf, req1, SHA_DIGEST_LENGTH);
    }

    /* HASH('req2', SKEY) xor HASH('req3', S) */
    {
        uint8_t req2[SHA_DIGEST_LENGTH];
        uint8_t req3[SHA_DIGEST_LENGTH];
        uint8_t buf[SHA_DIGEST_LENGTH];

        tr_sha1(req2, "req2", 4, tr_cryptoGetTorrentHash(handshake->crypto), SHA_DIGEST_LENGTH, NULL);
        computeRequestHash(handshake, "req3", req3);

        for (int i = 0; i < SHA_DIGEST_LENGTH; ++i)
        {
            buf[i] = req2[i] ^ req3[i];
        }

        evbuffer_add(outbuf, buf, SHA_DIGEST_LENGTH);
    }

    // Send out the two unencrypted messages.
    tr_peerIoWriteBuf(handshake->io, outbuf, false);

    // Remaining messages are encrypted. Decryption will be initialized
    // in readVc
    tr_cryptoEncryptInit(handshake->crypto);
    tr_peerIoSetEncryption(handshake->io, PEER_ENCRYPTION_RC4);

    /* ENCRYPT(VC, crypto_provide, len(PadC), PadC
     * PadC is reserved for future extensions to the handshake...
     * standard practice at this time is for it to be zero-length */
    {
        uint8_t vc[VC_LENGTH] = { 0, 0, 0, 0, 0, 0, 0, 0 };

        evbuffer_add(outbuf, vc, VC_LENGTH);
        evbuffer_add_uint32(outbuf, getCryptoProvide(handshake));
        evbuffer_add_uint16(outbuf, 0);
    }

    /* ENCRYPT len(IA)), ENCRYPT(IA) */
    {
        uint8_t msg[HANDSHAKE_SIZE];

        if (!buildHandshakeMessage(handshake, msg))
        {
            return tr_handshakeDone(handshake, false);
        }

        evbuffer_add_uint16(outbuf, sizeof(msg));
        evbuffer_add(outbuf, msg, sizeof(msg));

        handshake->haveSentBitTorrentHandshake = true;
    }

    /* send it */
    setReadState(handshake, AWAITING_VC);
    tr_peerIoWriteBuf(handshake->io, outbuf, false);

    /* cleanup */
    evbuffer_free(outbuf);
    return READ_LATER;
}

// MSE (Message Stream Encryption) spec: "Since the length of [PadB is] unknown,
// A will be able to resynchronize on ENCRYPT(VC)"
// https://archive.ph/POn7v
static ReadState readVC(tr_handshake* handshake, struct evbuffer* inbuf)
{
    uint8_t tmp[VC_LENGTH];
    int const key_len = VC_LENGTH;
    uint8_t const key[VC_LENGTH] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    /* note: this works w/o having to `unwind' the buffer if
     * we read too much, but it is pretty brute-force.
     * it would be nice to make this cleaner. */
     // See https://github.com/transmission/transmission/pull/3471 for
     // possible optimization and https://github.com/transmission/transmission/pull/6025
     // for a fix of that.
    for (;;)
    {
        if (evbuffer_get_length(inbuf) < VC_LENGTH)
        {
            dbgmsg(handshake, "not enough bytes... returning read_more");
            return READ_LATER;
        }

        memcpy(tmp, evbuffer_pullup(inbuf, key_len), key_len);
        // Initialize decryption state. Note that we must re-initialize
        // each loop since we always want to parse with a fresh crypto state.
        tr_cryptoDecryptInit(handshake->crypto);
        tr_cryptoDecrypt(handshake->crypto, key_len, tmp, tmp);

        if (memcmp(tmp, key, key_len) == 0)
        {
            break;
        }

        evbuffer_drain(inbuf, 1);
    }

    dbgmsg(handshake, "got it!");
    evbuffer_drain(inbuf, key_len);
    setState(handshake, AWAITING_CRYPTO_SELECT);
    return READ_NOW;
}

static ReadState readCryptoSelect(tr_handshake* handshake, struct evbuffer* inbuf)
{
    uint16_t pad_d_len;
    uint32_t crypto_select;
    static size_t const needlen = sizeof(uint32_t) + sizeof(uint16_t);

    if (evbuffer_get_length(inbuf) < needlen)
    {
        return READ_LATER;
    }

    tr_peerIoReadUint32(handshake->io, inbuf, &crypto_select);
    handshake->crypto_select = crypto_select;
    dbgmsg(handshake, "crypto select is %d", (int)crypto_select);

    if ((crypto_select & getCryptoProvide(handshake)) == 0)
    {
        dbgmsg(handshake, "peer selected an encryption option we didn't offer");
        return tr_handshakeDone(handshake, false);
    }

    tr_peerIoReadUint16(handshake->io, inbuf, &pad_d_len);
    dbgmsg(handshake, "pad_d_len is %d", (int)pad_d_len);

    if (pad_d_len > PadD_MAXLEN)
    {
        dbgmsg(handshake, "encryption handshake: pad_d_len is too long");
        return tr_handshakeDone(handshake, false);
    }

    handshake->pad_d_len = pad_d_len;

    setState(handshake, AWAITING_PAD_D);
    return READ_NOW;
}

static ReadState readPadD(tr_handshake* handshake, struct evbuffer* inbuf)
{
    size_t const needlen = handshake->pad_d_len;

    dbgmsg(handshake, "pad d: need %zu, got %zu", needlen, evbuffer_get_length(inbuf));

    if (evbuffer_get_length(inbuf) < needlen)
    {
        return READ_LATER;
    }

    tr_peerIoDrain(handshake->io, inbuf, needlen);

    // Possibly downgrade encryption mode if peer selected plaintext
    tr_peerIoSetEncryption(handshake->io, handshake->crypto_select);

    setState(handshake, AWAITING_HANDSHAKE);
    return READ_NOW;
}

/***
****
****  INCOMING/OUTGOING CONNECTIONS
****
***/

static ReadState readHandshake(tr_handshake* handshake, struct evbuffer* inbuf)
{
    uint8_t name[HANDSHAKE_NAME_LEN];
    uint8_t reserved[HANDSHAKE_FLAGS_LEN];
    uint8_t hash[SHA_DIGEST_LENGTH];

    // Note that because RC4 is a stream cipher, ciphertext length is same as plaintext length.
    // This is why we can compare lengths in this fashion.
    dbgmsg(handshake, "payload: need %d, got %zu", INCOMING_HANDSHAKE_LEN, evbuffer_get_length(inbuf));

    if (evbuffer_get_length(inbuf) < INCOMING_HANDSHAKE_LEN)
    {
        return READ_LATER;
    }

    handshake->haveReadAnythingFromPeer = true;


    /* peek, don't read. We may be handing inbuf to AWAITING_YA */
    bool isEncrypted = memcmp(evbuffer_pullup(inbuf, HANDSHAKE_NAME_LEN), HANDSHAKE_NAME, HANDSHAKE_NAME_LEN) != 0;
    if (!isEncrypted)
    {
        if (handshake->encryptionMode == TR_ENCRYPTION_REQUIRED)
        {
            dbgmsg(handshake, "peer is unencrypted, and we're disallowing that");
            return tr_handshakeDone(handshake, false);
        } else if (tr_peerIoIsEncrypted(handshake->io)) {
            dbgmsg(handshake, "peer is unencrypted, and that does not agree with our handshake");
            return tr_handshakeDone(handshake, false);
        }
    }
    else /* encrypted or corrupt */
    {
        // If we haven't yet completed an encrypted handshake.
        if (tr_peerIoIsIncoming(handshake->io) && !tr_peerIoHasTorrentHash(handshake->io))
        {
            dbgmsg(handshake, "I think peer is sending us an encrypted handshake...");
            setState(handshake, AWAITING_YA);
            return READ_NOW;
        }

        if (!tr_peerIoIsEncrypted(handshake->io)) {
            dbgmsg(handshake, "peer is encrypted, and that does not agree with our handshake");
            return tr_handshakeDone(handshake, false);
        }
    }

    /* confirm the protocol */
    tr_peerIoReadBytes(handshake->io, inbuf, name, HANDSHAKE_NAME_LEN);
    if (memcmp(name, HANDSHAKE_NAME, HANDSHAKE_NAME_LEN) != 0)
    {
        dbgmsg(handshake, "handshake prefix not correct");
        return tr_handshakeDone(handshake, false);
    }

    /* read the reserved bytes */
    tr_peerIoReadBytes(handshake->io, inbuf, reserved, sizeof(reserved));

    /**
    *** Set Extensions
    **/

    tr_peerIoEnableDHT(handshake->io, HANDSHAKE_HAS_DHT(reserved));
    tr_peerIoEnableLTEP(handshake->io, HANDSHAKE_HAS_LTEP(reserved));
    tr_peerIoEnableFEXT(handshake->io, HANDSHAKE_HAS_FASTEXT(reserved));

    /* torrent hash */
    tr_peerIoReadBytes(handshake->io, inbuf, hash, sizeof(hash));

    if (tr_peerIoIsIncoming(handshake->io) && !tr_peerIoHasTorrentHash(handshake->io)) /*incoming plain handshake*/
    {
        tr_torrent *tor = tr_torrentFindFromHash(handshake->session, hash);
        if (tor == NULL)
        {
            dbgmsg(handshake, "peer is trying to connect to us for a torrent we don't have.");
            return tr_handshakeDone(handshake, false);
        } else if (!tor->isRunning) {
            dbgmsg(handshake, "peer is trying to connect to us for a torrent not running.");
            return tr_handshakeDone(handshake, false);
        }
        tr_peerIoSetTorrentHash(handshake->io, hash);
    }
    else /* outgoing, or incoming MSE handshake */
    {
        TR_ASSERT(tr_peerIoHasTorrentHash(handshake->io));

        if (memcmp(hash, tr_peerIoGetTorrentHash(handshake->io), SHA_DIGEST_LENGTH) != 0)
        {
            dbgmsg(handshake, "peer returned the wrong hash. wtf?");
            return tr_handshakeDone(handshake, false);
        }
    }

    /**
    ***  If it's an incoming message, we need to send a response handshake
    **/

    if (!handshake->haveSentBitTorrentHandshake)
    {
        uint8_t msg[HANDSHAKE_SIZE];

        if (!buildHandshakeMessage(handshake, msg))
        {
            return tr_handshakeDone(handshake, false);
        }

        tr_peerIoWriteBytes(handshake->io, msg, sizeof(msg), false);
        handshake->haveSentBitTorrentHandshake = true;
    }

    setReadState(handshake, AWAITING_PEER_ID);
    return READ_NOW;
}

static ReadState readPeerId(tr_handshake* handshake, struct evbuffer* inbuf)
{
    bool connected_to_self;
    char client[128];
    uint8_t peer_id[PEER_ID_LEN];
    tr_torrent* tor;

    if (evbuffer_get_length(inbuf) < PEER_ID_LEN)
    {
        return READ_LATER;
    }

    /* peer id */
    tr_peerIoReadBytes(handshake->io, inbuf, peer_id, PEER_ID_LEN);
    tr_peerIoSetPeersId(handshake->io, peer_id);
    handshake->havePeerID = true;
    tr_clientForId(client, sizeof(client), peer_id);
    dbgmsg(handshake, "peer-id is [%s] ... isIncoming is %d", client, tr_peerIoIsIncoming(handshake->io));

    /* if we've somehow connected to ourselves, don't keep the connection */
    tor = tr_torrentFindFromHash(handshake->session, tr_peerIoGetTorrentHash(handshake->io));
    connected_to_self = tor != NULL && memcmp(peer_id, tr_torrentGetPeerId(tor), PEER_ID_LEN) == 0;

    return tr_handshakeDone(handshake, !connected_to_self);
}

/***
****
****  INCOMING CONNECTIONS
****
***/
static ReadState readYa(tr_handshake* handshake, struct evbuffer* inbuf)
{
    uint8_t ya[KEY_LEN];
    uint8_t* walk;
    uint8_t outbuf[KEY_LEN + PadB_MAXLEN];
    uint8_t const* myKey;
    int len;

    dbgmsg(handshake, "in readYa... need %d, have %zu", KEY_LEN, evbuffer_get_length(inbuf));

    if (evbuffer_get_length(inbuf) < KEY_LEN)
    {
        return READ_LATER;
    }

    /* read the incoming peer's public key */
    evbuffer_remove(inbuf, ya, KEY_LEN);

    if (!tr_cryptoComputeSecret(handshake->crypto, ya))
    {
        return tr_handshakeDone(handshake, false);
    }

    computeRequestHash(handshake, "req1", handshake->myReq1);

    /* send our public key to the peer */
    dbgmsg(handshake, "sending B->A: Diffie Hellman Yb, PadB");
    walk = outbuf;
    myKey = tr_cryptoGetMyPublicKey(handshake->crypto, &len);
    memcpy(walk, myKey, len);
    walk += len;
    len = tr_rand_int(PadB_MAXLEN);
    tr_rand_buffer(walk, len);
    walk += len;

    setReadState(handshake, AWAITING_PAD_A);
    tr_peerIoWriteBytes(handshake->io, outbuf, walk - outbuf, false);
    return READ_NOW;
}

static ReadState readPadA(tr_handshake* handshake, struct evbuffer* inbuf)
{
    /* resynchronizing on HASH('req1', S) */
    struct evbuffer_ptr ptr = evbuffer_search(inbuf, (char const*)handshake->myReq1, SHA_DIGEST_LENGTH, NULL);

    if (ptr.pos != -1) /* match */
    {
        evbuffer_drain(inbuf, ptr.pos);
        dbgmsg(handshake, "found it... looking setting to awaiting_crypto_provide");
        setState(handshake, AWAITING_CRYPTO_PROVIDE);
        return READ_NOW;
    }
    else
    {
        size_t const len = evbuffer_get_length(inbuf);

        if (len > SHA_DIGEST_LENGTH)
        {
            evbuffer_drain(inbuf, len - SHA_DIGEST_LENGTH);
        }

        return READ_LATER;
    }
}

static ReadState readCryptoProvide(tr_handshake* handshake, struct evbuffer* inbuf)
{
    /* HASH('req2', SKEY) xor HASH('req3', S), ENCRYPT(VC, crypto_provide, len(PadC)) */

    uint8_t vc_in[VC_LENGTH];
    uint8_t req2[SHA_DIGEST_LENGTH];
    uint8_t req3[SHA_DIGEST_LENGTH];
    uint8_t obfuscatedTorrentHash[SHA_DIGEST_LENGTH];
    uint16_t padc_len = 0;
    uint32_t crypto_provide = 0;
    tr_torrent* tor;
    size_t const needlen = SHA_DIGEST_LENGTH + /* HASH('req1', s) */
        SHA_DIGEST_LENGTH + /* HASH('req2', SKEY) xor HASH('req3', S) */
        VC_LENGTH + sizeof(crypto_provide) + sizeof(padc_len);

    if (evbuffer_get_length(inbuf) < needlen)
    {
        return READ_LATER;
    }

    /* TODO: confirm they sent HASH('req1',S) here? */
    evbuffer_drain(inbuf, SHA_DIGEST_LENGTH);

    /* This next piece is HASH('req2', SKEY) xor HASH('req3', S) ...
     * we can get the first half of that (the obufscatedTorrentHash)
     * by building the latter and xor'ing it with what the peer sent us */
    dbgmsg(handshake, "reading obfuscated torrent hash...");
    evbuffer_remove(inbuf, req2, SHA_DIGEST_LENGTH);
    computeRequestHash(handshake, "req3", req3);

    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i)
    {
        obfuscatedTorrentHash[i] = req2[i] ^ req3[i];
    }

    if ((tor = tr_torrentFindFromObfuscatedHash(handshake->session, obfuscatedTorrentHash)) != NULL)
    {
        bool const clientIsSeed = tr_torrentIsSeed(tor);
        bool const peerIsSeed = tr_peerMgrPeerIsSeed(tor, tr_peerIoGetAddress(handshake->io, NULL));
        dbgmsg(handshake, "got INCOMING connection's encrypted handshake for torrent [%s]", tr_torrentName(tor));
        tr_peerIoSetTorrentHash(handshake->io, tor->info.hash);
        if (!tor->isRunning) {
            dbgmsg(handshake, "we are not running that torrent...");
            return tr_handshakeDone(handshake, false);
        }
        if (clientIsSeed && peerIsSeed)
        {
            dbgmsg(handshake, "another seed tried to reconnect to us!");
            return tr_handshakeDone(handshake, false);
        }
    }
    else
    {
        dbgmsg(handshake, "can't find that torrent...");
        return tr_handshakeDone(handshake, false);
    }

    /* next part: ENCRYPT(VC, crypto_provide, len(PadC), */

    tr_cryptoDecryptInit(handshake->crypto);
    tr_cryptoEncryptInit(handshake->crypto);
    tr_peerIoSetEncryption(handshake->io, PEER_ENCRYPTION_RC4);

    tr_peerIoReadBytes(handshake->io, inbuf, vc_in, VC_LENGTH);
    // TODO: Verify VC_IN is all zero?

    tr_peerIoReadUint32(handshake->io, inbuf, &crypto_provide);
    handshake->crypto_provide = crypto_provide;
    dbgmsg(handshake, "crypto_provide is %d", (int)crypto_provide);

    tr_peerIoReadUint16(handshake->io, inbuf, &padc_len);
    dbgmsg(handshake, "padc len is %d", (int)padc_len);

    if (padc_len > PadC_MAXLEN)
    {
        dbgmsg(handshake, "encryption handshake: peer's PadC is too big");
        return tr_handshakeDone(handshake, false);
    }

    handshake->pad_c_len = padc_len;
    setState(handshake, AWAITING_PAD_C);
    return READ_NOW;
}

static ReadState readPadC(tr_handshake* handshake, struct evbuffer* inbuf)
{
    char* padc;
    uint16_t ia_len;
    size_t const needlen = handshake->pad_c_len + sizeof(uint16_t);

    if (evbuffer_get_length(inbuf) < needlen)
    {
        return READ_LATER;
    }

    /* read the throwaway padc */
    evbuffer_drain(inbuf, handshake->pad_c_len);

    /* read ia_len */
    tr_peerIoReadUint16(handshake->io, inbuf, &ia_len);
    dbgmsg(handshake, "ia_len is %d", (int)ia_len);
    handshake->ia_len = ia_len;
    setState(handshake, AWAITING_IA);
    return READ_NOW;
}

static ReadState readIA(tr_handshake* handshake, struct evbuffer* inbuf)
{
    size_t const needlen = handshake->ia_len;
    struct evbuffer* outbuf;
    uint32_t crypto_select;

    dbgmsg(handshake, "reading IA... have %zu, need %zu", evbuffer_get_length(inbuf), needlen);

    if (evbuffer_get_length(inbuf) < needlen)
    {
        return READ_LATER;
    }

    /**
    ***  B->A: ENCRYPT(VC, crypto_select, len(padD), padD), ENCRYPT2(Payload Stream)
    **/

    outbuf = evbuffer_new();

    {
        /* write VC */
        uint8_t vc[VC_LENGTH];
        memset(vc, 0, VC_LENGTH);
        evbuffer_add(outbuf, vc, VC_LENGTH);
        dbgmsg(handshake, "sending vc");
    }

    /* write crypto_select */
    crypto_select = getCryptoSelect(handshake, handshake->crypto_provide);

    if (crypto_select != 0)
    {
        dbgmsg(handshake, "selecting crypto mode '%d'", (int)crypto_select);
        evbuffer_add_uint32(outbuf, crypto_select);
    }
    else
    {
        dbgmsg(handshake, "peer didn't offer an encryption mode we like.");
        evbuffer_free(outbuf);
        return tr_handshakeDone(handshake, false);
    }

    dbgmsg(handshake, "sending pad d");

    /* ENCRYPT(VC, crypto_select, len(PadD), PadD
     * PadD is reserved for future extensions to the handshake...
     * standard practice at this time is for it to be zero-length */
    {
        uint16_t const len = 0;
        evbuffer_add_uint16(outbuf, len);
    }

    /* maybe de-encrypt our connection */
    if (crypto_select == CRYPTO_PROVIDE_PLAINTEXT)
    {
        tr_peerIoWriteBuf(handshake->io, outbuf, false);
        // While new content is not encrypted, previous IA content
        // would still be present. Switching the mode here would give
        // erroneous results. To make this seamless we transparently decrypt
        // any existing IA content.
        tr_peerIoDecryptBuf(handshake->io, inbuf, handshake->ia_len);
        tr_peerIoSetEncryption(handshake->io, PEER_ENCRYPTION_NONE);

        /*
         Versions of Transmission older than 4-ish have a bug where they do not properly de-encrypt.
         That is, say client A talks to a Tr3 client B.
         A sends a crypto-provide with plaintext option in readYb, and the Tr3 client B
         (in clear-preferred mode) ends up selecting a plaintext option here in readIa.

         B ends up sending the rest of the info (along with the handshake). So from A's
         perspective the handshake will finish successfully. However when B subsequently
         goes to parse the handshake, it won't like the encrypted contents and so will abort.
         This unfortunately means A won't even bother retrying (since the handshake
         was successful from its perspective, instead it's like the peer suddenly dropped offline.)

         Mainline Tr3 -> Tr3 actually won't exhibit this particular issue though because
         Tr3 never makes an outgoing MSE handshake that offers plaintext.
        */
    }

    dbgmsg(handshake, "sending handshake");

    // write our handshake. Per the spec since each step is blocking, the incoming
    // payload stream might only be sent by our peer after the peer receives our outgoing one?
    // Also note that we _must_ send out (on the wire) our crypto select/provide message,
    // before incoming payload is received so we may as well write out handshake info as well.
    {
        uint8_t msg[HANDSHAKE_SIZE];

        if (!buildHandshakeMessage(handshake, msg))
        {
            return tr_handshakeDone(handshake, false);
        }

        evbuffer_add(outbuf, msg, sizeof(msg));
        handshake->haveSentBitTorrentHandshake = true;
    }

    /* send it out */
    tr_peerIoWriteBuf(handshake->io, outbuf, false);
    evbuffer_free(outbuf);

    /* now await the handshake. It consists of both IA and the payload stream.  */
    setState(handshake, AWAITING_HANDSHAKE);
    return READ_NOW;
}

/***
****
****
****
***/

static ReadState canRead(struct tr_peerIo* io, void* arg, size_t* piece)
{
    TR_ASSERT(tr_isPeerIo(io));

    ReadState ret;
    tr_handshake* handshake = arg;
    struct evbuffer* inbuf = tr_peerIoGetReadBuffer(io);

    /* no piece data in handshake */
    *piece = 0;

    dbgmsg(handshake, "handling canRead; state is [%s]", getStateName(handshake->state));

    do
    {
        switch (handshake->state)
        {
        case AWAITING_HANDSHAKE:
            ret = readHandshake(handshake, inbuf);
            break;

        case AWAITING_PEER_ID:
            ret = readPeerId(handshake, inbuf);
            // As this is a terminal state, if handshake was successful we should not
            // loop again but instead bubble back up to the peer-io read loop (as we have
            // changed the read callback).
            return ret;

        case AWAITING_YA:
            ret = readYa(handshake, inbuf);
            break;

        case AWAITING_PAD_A:
            ret = readPadA(handshake, inbuf);
            break;

        case AWAITING_CRYPTO_PROVIDE:
            ret = readCryptoProvide(handshake, inbuf);
            break;

        case AWAITING_PAD_C:
            ret = readPadC(handshake, inbuf);
            break;

        case AWAITING_IA:
            ret = readIA(handshake, inbuf);
            break;

        case AWAITING_YB:
            ret = readYb(handshake, inbuf);
            break;

        case AWAITING_VC:
            ret = readVC(handshake, inbuf);
            break;

        case AWAITING_CRYPTO_SELECT:
            ret = readCryptoSelect(handshake, inbuf);
            break;

        case AWAITING_PAD_D:
            ret = readPadD(handshake, inbuf);
            break;

        default:
            ret = READ_ERR;
            CHECK_MSG(false, "unhandled handshake state %d", (int)handshake->state);
        }

    // If READ_NOW is requested, can optimize by directly looping in here
    // instead of bubbling back up to peer-io read loop. This avoids
    // some possibly expensive bookkeeping logic for number of bytes written/read.
    } while (ret == READ_NOW);

    return ret;
}

static bool fireDoneFunc(tr_handshake* handshake, bool isConnected)
{
    uint8_t const* peer_id = (isConnected && handshake->havePeerID) ? tr_peerIoGetPeersId(handshake->io) : NULL;
    bool const success = (*handshake->doneCB)(handshake, handshake->io, handshake->haveReadAnythingFromPeer, isConnected,
        peer_id, handshake->doneUserData);

    return success;
}

static void tr_handshakeFree(tr_handshake* handshake)
{
    if (handshake->io != NULL)
    {
        tr_peerIoUnref(handshake->io); /* balanced by the ref in tr_handshakeNew */
    }

    event_free(handshake->timeout_timer);
    tr_free(handshake);
}

static ReadState tr_handshakeDone(tr_handshake* handshake, bool isOK)
{
    bool success;

    dbgmsg(handshake, "handshakeDone: %s", isOK ? "connected" : "aborting");
    tr_peerIoSetIOFuncs(handshake->io, NULL, NULL, NULL, NULL);

    success = fireDoneFunc(handshake, isOK);

    tr_handshakeFree(handshake);

    // The responding client of a handshake usually starts sending BT messages immediately after
    // the handshake, so we need to return READ_NOW to ensure those messages are processed.
    return success ? READ_NOW : READ_ERR;
}

void tr_handshakeAbort(tr_handshake* handshake)
{
    if (handshake != NULL)
    {
        tr_handshakeDone(handshake, false);
    }
}

static void sendPlainTextHandshake(tr_handshake* handshake) {
    uint8_t msg[HANDSHAKE_SIZE];
    if (!buildHandshakeMessage(handshake, msg))
    {
        tr_handshakeDone(handshake, false);
        return;
    }
    handshake->haveSentBitTorrentHandshake = true;
    setReadState(handshake, AWAITING_HANDSHAKE);
    tr_peerIoWriteBytes(handshake->io, msg, sizeof(msg), false);
}

static void gotError(tr_peerIo* io, short what, void* vhandshake)
{
    int errcode = errno;
    tr_handshake* handshake = vhandshake;

    // Note that a uTP connection could fail either because the peer does not support uTP
    // or because it does not support encryption. rTorrent is the only notable client which
    // does not implement uTP. rTorrent supports encryption, but by default it is disabled
    // even for inbound connections. Deluge/qBitTorrent support both, but have an option
    // to disable both of them. Transmission does not expose an option to disable inbound
    // encrpytion. It does have an option to disable uTP or TCP connectivity
    // (in the GUI, only uTP can be disabled).

    // As a tradeoff between maximum peer reachability while minimizing number of reconnects, the order
    // is always the following.

    // With UTP enabled in Transmission preferences:
    // Outgoing handshake as UTP, encrypted
    // If fails, retry TCP, encrypted
    // If fails, retry TCP, plaintext.

    // With UTP disabled in Transmission preferences:
    // Outgoing handshake as TCP, encrypted
    // If fails, retry TCP, plaintext.

    // The encryption settings in Transmission only control whether handshake-only encryption
    // is offered as an option in the encrypted exchange, whether we accept the result of an exchange
    // with a header-only encrypt peer, and whether we sfallback to plaintext handshake.

    // (We don't try the UTP/plaintext combination, because such a combination is exceedingly unlikely
    // and even impossible to set in two common clients [transmission/rtorrent]).
    // We do make an effort to retry as encrypted rather than jumping to plaintext because many
    // clients may have the require-encryption enabled (many users misunderstand what it does, and such
    // users may have checked the box without understanding the details or conseqeuences).
    bool resendPlainHandshake = false;
    dbgmsg(handshake, "libevent got an error what==%d, errno=%d (%s%s)",
           (int)what, errno, tr_strerror(errno), (what & BEV_EVENT_EOF)  ? " eof " : "");


    if (io->socket.type == TR_PEER_SOCKET_TYPE_UTP && !io->isIncoming && 
        (handshake->state == AWAITING_YB /* encrypted utp */||
        (handshake->state == AWAITING_HANDSHAKE && !handshake->haveReadAnythingFromPeer) /*plaintext utp*/))
    {
        /* This peer probably doesn't speak uTP. */
        dbgmsg(handshake, "uTP %s handshake failed", handshake->state == AWAITING_YB ? "encrypted" : "plaintext");
        tr_torrent* tor;

        if (tr_peerIoHasTorrentHash(io))
        {
            tor = tr_torrentFindFromHash(handshake->session, tr_peerIoGetTorrentHash(io));
        }
        else
        {
            tor = NULL;
        }

        // Don't mark a peer as non-uTP unless it's really a connect failure.
        // This info is saved to spped up future connection attempts by skipping
        // the uTP handshake & timeout.
        if ((errcode == ETIMEDOUT || errcode == ECONNREFUSED) && tr_isTorrent(tor))
        {
            dbgmsg(handshake, "Marking peer as not supporting uTP.");
            tr_peerMgrSetUtpFailed(tor, tr_peerIoGetAddress(io, NULL), true);
        }

        // We would be in AWAITING_YB state if we started by sending out encrypted handshake
        // In such a case we should retry as encrypted TCP handshake.
        if (handshake->state == AWAITING_YB) {
            if (tr_peerIoReconnect(handshake->io) == 0) {
                dbgmsg(handshake, "Retrying with encrypted TCP handshake...");
                // The first message is always sent without encryption.
                tr_peerIoSetEncryption(io, PEER_ENCRYPTION_NONE);
                sendYa(handshake);
                return;
            }
        } else {
            // If we sent out a plaintext uTP handshake, we would be in AWAITING_HANDSHAKE
            // state. In such case we should retry as plaintext TCP. We might also
            // be in AWAITING_HANDSHAKE if we completed the encrypted uTP handshake but
            // then peer disconnected before sending the initial payload or something. We
            // detect this case by looking at whether we ever got any resonse from peer.
            resendPlainHandshake = true;
        }

    }

    /* if the error happened while we were sending a public key, we might
     * have encountered a peer that doesn't do encryption... reconnect and
     * try a plaintext handshake */
    if (io->socket.type == TR_PEER_SOCKET_TYPE_TCP && handshake->state == AWAITING_YB)
    {
        resendPlainHandshake = true;
        dbgmsg(handshake, "Encrypted TCP handshake failed");
    }

    if (resendPlainHandshake && handshake->encryptionMode != TR_ENCRYPTION_REQUIRED &&
          tr_peerIoReconnect(handshake->io) == 0) {
        dbgmsg(handshake, "Retrying with plaintext TCP handshake...");
        tr_peerIoSetEncryption(io, PEER_ENCRYPTION_NONE);
        sendPlainTextHandshake(handshake);
        return;
    } else {
        // All attempts failed, give up on this peer...
        dbgmsg(handshake, "Giving up on peer...");
        tr_handshakeDone(handshake, false);
    }
}

/**
***
**/

static void handshakeTimeout(evutil_socket_t foo UNUSED, short bar UNUSED, void* handshake)
{
    dbgmsg((tr_handshake*)handshake, "Handshake timed out after %d seconds, aborting", HANDSHAKE_TIMEOUT_SEC);
    tr_handshakeAbort(handshake);
}

tr_handshake* tr_handshakeNew(tr_peerIo* io, tr_encryption_mode encryptionMode, handshakeDoneCB doneCB, void* doneUserData)
{
    tr_handshake* handshake;
    tr_session* session = tr_peerIoGetSession(io);

    handshake = tr_new0(tr_handshake, 1);
    handshake->io = io;
    handshake->crypto = tr_peerIoGetCrypto(io);
    handshake->encryptionMode = encryptionMode;
    handshake->doneCB = doneCB;
    handshake->doneUserData = doneUserData;
    handshake->session = session;
    handshake->timeout_timer = evtimer_new(session->event_base, handshakeTimeout, handshake);
    tr_timerAdd(handshake->timeout_timer, HANDSHAKE_TIMEOUT_SEC, 0);

    tr_peerIoRef(io); /* balanced by the unref in tr_handshakeFree */
    tr_peerIoSetIOFuncs(handshake->io, canRead, NULL, gotError, handshake);
    tr_peerIoSetEncryption(io, PEER_ENCRYPTION_NONE);

    dbgmsg(handshake, "New %s handshake requested.", io->socket.type == TR_PEER_SOCKET_TYPE_UTP ? "utp" : "tcp");

    if (tr_peerIoIsIncoming(handshake->io))
    {
        setReadState(handshake, AWAITING_HANDSHAKE);
    }
    else if (encryptionMode != TR_CLEAR_PREFERRED)
    {
        sendYa(handshake);
    } else {
        sendPlainTextHandshake(handshake);
    }

    return handshake;
}

struct tr_peerIo* tr_handshakeStealIO(tr_handshake* handshake)
{
    TR_ASSERT(handshake != NULL);
    TR_ASSERT(handshake->io != NULL);

    struct tr_peerIo* io = handshake->io;
    handshake->io = NULL;
    return io;
}

tr_address const* tr_handshakeGetAddr(struct tr_handshake const* handshake, tr_port* port)
{
    TR_ASSERT(handshake != NULL);
    TR_ASSERT(handshake->io != NULL);

    return tr_peerIoGetAddress(handshake->io, port);
}
