#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <sys/socket.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>
#include <math.h>

#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/sha.h>
#include <openssl/aes.h>

#include <curl/curl.h>

#define LH_DECLARE_SHORT_NAMES 1

#include "lh_debug.h"
#include "lh_buffers.h"
#include "lh_bytes.h"
#include "lh_files.h"
#include "lh_net.h"
#include "lh_event.h"
#include "lh_compress.h"
#include "lh_arr.h"

#include "mcp_ids.h"
#include "mcp_packet.h"
#include "mcp_gamestate.h"
//#include "mcp_game.h"

#define SERVER_ADDR "2b2t.org"

#define SERVER_PORT 25565
#define WEBSERVER_PORT 8080

#define ASYNC_THRESHOLD 500000
#define NEAR_THRESHOLD 40000

#define G_MCSERVER  1
#define G_WEBSERVER 2
#define G_PROXY     3

////////////////////////////////////////////////////////////////////////////////

int signal_caught;

void signal_handler(int signum) {
    printf("Caught signal %d, stopping main loop\n",signum);
    signal_caught = 1;
}

////////////////////////////////////////////////////////////////////////////////

lh_pollarray pa;

struct {
    int state;          // handshake state

    int cs;             // connected socket to client
    int ms;             // connected socket to server

    // connections
    lh_conn * cs_conn; // to the client
    lh_conn * ms_conn; // to the server

    // decoded buffers
    lh_buf_t  cs_rx;   // client -> proxy
    lh_buf_t  cs_tx;   // proxy -> client
    lh_buf_t  ms_rx;   // server -> proxy
    lh_buf_t  ms_tx;   // proxy -> server

    // RSA structures/keys for server-side and client-side
    RSA *s_rsa; // public key only - must be freed by RSA_free
    RSA *c_rsa; // public+private key - must be freed by RSA_free

    // verification tokens
    char s_token[4]; // what we received from the server
    char c_token[4]; // generated by us, sent to client

    // AES encryption keys (128 bit)
    char s_skey[16]; // generated by us, sent to the server
    char c_skey[16]; // received from the client

    // data parsed from JSON
    char accessToken[256];
    char selectedProfile[256];
    char serverId[256];

    // server ID - we forward this to client as is, so no need
    // for client- and server side versions.
    // zero-terminated string, so no need for length value
    // Note: serverID is received as UTF-16 string, but converted to ASCII
    // for hashing. The string itself is a hexstring, but it's not converted
    // to bytes or anything
    char s_id[256];

    // DER-encoded public key from the server
    char s_pkey[1024];
    int s_pklen;

    // DER-encoded public key sent to the client
    char c_pkey[1024];
    int c_pklen;

    int encstate;
    int passfirst;

    AES_KEY c_aes;
    char c_enc_iv[16];
    char c_dec_iv[16];

    AES_KEY s_aes;
    char s_enc_iv[16];
    char s_dec_iv[16];

    int enable_encryption;
    int encryption_active;

    FILE * output;
    FILE * dbg;

    int comptr; // compression threshold, -1 means compression is disabled
} mitm;

////////////////////////////////////////////////////////////////////////////////

void write_packet_raw(uint8_t *ptr, ssize_t len, lh_buf_t *buf) {
    uint8_t hbuf[16]; CLEAR(hbuf);
    ssize_t ll = lh_place_varint(hbuf,len) - hbuf;

    ssize_t widx = buf->C(data);
    
    lh_arr_add(GAR4(buf->data),(len+ll));

    memmove(P(buf->data)+widx, hbuf, ll);
    memmove(P(buf->data)+widx+ll, ptr, len);
}

void process_encryption_request(uint8_t *p, lh_buf_t *forw) {
    Rstr(serverID);

    Rvarint(klen);
    memmove(mitm.s_pkey,p,klen);
    Rskip(klen);

    Rvarint(tlen);
    memmove(mitm.s_token,p,tlen);
    Rskip(tlen);

    printf("Encryption Request\n");
                
    sprintf(mitm.s_id,"%s",serverID);
    mitm.s_pklen = klen;
                
    // decode server PUBKEY to an RSA struct
    unsigned char *pp = mitm.s_pkey;
    d2i_RSA_PUBKEY(&mitm.s_rsa, (const unsigned char **)&pp, klen);
    if (mitm.s_rsa == NULL) {
        printf("Failed to decode the server's public key\n");
        exit(1);
    }
    //RSA_print_fp(stdout, mitm.s_rsa, 4);

    // generate the server-side shared key pair
    RAND_pseudo_bytes(mitm.s_skey, 16);
    //printf("Server-side shared key: ");
    //hexprint(mitm.s_skey, 16);

    // create a client-side RSA
    mitm.c_rsa = RSA_generate_key(1024, RSA_F4, NULL, NULL);
    if (mitm.c_rsa == NULL) {
        printf("Failed to generate client-side RSA key\n");
        exit(1);
    }
    //RSA_print_fp(stdout, mitm.c_rsa, 4);
    
    // encode the client-side pubkey as DER
    pp = mitm.c_pkey;
    mitm.c_pklen = i2d_RSA_PUBKEY(mitm.c_rsa, &pp);

    // generate the client-side verification token
    RAND_pseudo_bytes(mitm.c_token, 4);

    // combine it to a MCP message to the client
    uint8_t output[65536];
    uint8_t *w = output;

    if (mitm.comptr>=0) {
        printf("Warning: sending pseudo-compressed Encryption Request\n");
        write_varint(w, 0);
    }

    write_varint(w, PID(SL_EncryptionRequest));
    write_varint(w, strlen(serverID));
    memmove(w, serverID, strlen(serverID));
    w+=strlen(serverID);
    write_varint(w, mitm.c_pklen);
    memmove(w, mitm.c_pkey, mitm.c_pklen);
    w+=mitm.c_pklen;
    write_varint(w, 4);
    memmove(w, mitm.c_token, 4);
    w+=4;
    
    //printf("Sending to client %zd bytes:\n",w-output);
    //hexdump(output, w-output);
    write_packet_raw(output, w-output, forw);
}

void process_encryption_response(uint8_t *p, lh_buf_t *forw) {
    Rvarint(sklen);
    uint8_t *skey = p;
    Rskip(sklen);

    Rvarint(tklen);
    uint8_t *token = p;
    Rskip(tklen);

    char buf[4096];
    int dklen = RSA_private_decrypt(sklen, skey, buf, mitm.c_rsa, RSA_PKCS1_PADDING);
    if (dklen < 0) {
        printf("Failed to decrypt the shared key received from the client\n");
        exit(1);
    }
    //printf("Decrypted client shared key, keylen=%d ",dklen);
    //hexprint(buf, dklen);
    memcpy(mitm.c_skey, buf, 16);
    
    int dtlen = RSA_private_decrypt(tklen, token, buf, mitm.c_rsa, RSA_PKCS1_PADDING);
    if (dtlen < 0) {
        printf("Failed to decrypt the verification token received from the client\n");
        exit(1);
    }
    //printf("Decrypted client token, len=%d ",dtlen);
    //hexprint(buf, dtlen);
    //printf("Original token: ");
    //hexprint(mitm.c_token,4);
    if (memcmp(buf, mitm.c_token, 4)) {
        printf("Token does not match!\n");
        exit(1);
    }
            
    uint8_t output[65536];
    uint8_t *w = output;

    if (mitm.comptr>=0) {
        printf("Warning: sending pseudo-compressed Encryption Response\n");
        write_varint(w, 0);
    }

    // at this point, the client side is verified and the key is established
    // now send our response to the server
    write_varint(w, PID(CL_EncryptionResponse));

    int eklen = RSA_public_encrypt(sizeof(mitm.s_skey), mitm.s_skey, buf, mitm.s_rsa, RSA_PKCS1_PADDING);
    write_varint(w,(short)eklen);
    memcpy(w, buf, eklen);
    w += eklen;
            
    int etlen = RSA_public_encrypt(sizeof(mitm.s_token), mitm.s_token, buf, mitm.s_rsa, RSA_PKCS1_PADDING);
    write_varint(w,(short)etlen);
    memcpy(w, buf, etlen);
    w += etlen;
                
    query_auth_server();
    //hexdump(output, w-output);
    write_packet_raw(output, w-output, forw);

    mitm.enable_encryption = 1;
}

////////////////////////////////////////////////////////////////////////////////

/* process a single MC protocol packet coming from either side
   We will only handle packets related to connection/authentication
   phase here. Everything else will go to process_play_packet in
   mcp_game module
*/
void process_packet(int is_client, uint8_t *ptr, ssize_t len, lh_buf_t *tx) {
    // one nice advantage - we can be sure that we have all data in the buffer,
    // so there's no need for limit checking with the new protocol

    uint8_t *p = ptr;

    if (mitm.comptr>=0) {
        // compression is active
        // quick-and-dirty for compressed packets during the login phase
        // just strip the leading 0 byte
        int32_t uclen=lh_read_varint(p);
        assert(uclen==0);
        // I simply can't comprehend the retardedness of people at Mojang
    }

    uint32_t type = lh_read_varint(p);
    uint32_t stype = ((mitm.state<<24)|(is_client<<28)|(type&0xffffff));

    char *states = "ISLP";
    printf("%c %c type=%02x, len=%zd\n", is_client?'C':'S', states[mitm.state],type,len);
    //hexdump(ptr, len);

    uint8_t output[65536];
    uint8_t *w = output;

    switch (stype) {
        ////////////////////////////////////////////////////////////////////////
        // Idle state

        case CI_Handshake: {
            Rvarint(protocolVer);
            Rstr(serverAddr);
            Rshort(serverPort);
            Rvarint(nextState);
            mitm.state = nextState;
            printf("C %-30s protocol=%d server=%s:%d nextState=%d\n",
                   "Handshake",protocolVer,serverAddr,serverPort,nextState);
            write_packet_raw(ptr, len, tx);
            break;
        }

        ////////////////////////////////////////////////////////////////////////
        // Login

        case CL_EncryptionResponse:
            process_encryption_response(p, tx);
            break;

        case SL_EncryptionRequest:
            process_encryption_request(p, tx);
            break;

        case SL_SetCompression: {
            printf("SetCompression during login phase!\n");
            Rvarint(threshold);
            write_packet_raw(ptr, len, tx);
            mitm.comptr = threshold;
            break;
        }

        case SL_LoginSuccess:
            printf("S Login Success\n");
            mitm.state = STATE_PLAY;
            write_packet_raw(ptr, len, tx);
            break;

        ////////////////////////////////////////////////////////////////////////

        default: {
            // by default, just forward the packet as is
            write_packet_raw(ptr, len, tx);
        }
    }
}


//TODO: this will move to mcp_game
void handle_packet(MCPacket *pkt, MCPacketQueue *tq, MCPacketQueue *bq) {
    switch (pkt->type) {
        default:
            queue_packet(pkt, tq);
    }
}

#define MAXPLEN (4*1024*1024)

uint8_t ubuf[MAXPLEN];
uint8_t cbuf[MAXPLEN];
#define LIM64(len) ((len)>64?64:(len))
#define LIM128(len) ((len)>128?128:(len))

void write_packet(MCPacket *pkt, lh_buf_t *tx) {
    ssize_t ulen = encode_packet(pkt, ubuf);

    if (mitm.comptr >= 0) {
        // compression is active
        uint8_t *w = cbuf;
        ssize_t clen = 0;
        if (ulen >= mitm.comptr) {
            // length is at or over threshold - compress it
            write_varint(w, (int32_t)ulen);
            clen = lh_zlib_encode_to(ubuf, ulen, w, cbuf+sizeof(cbuf)-w);
            assert(clen > 0);
        }
        else {
            // packet is below compression threshold, send uncompressed
            write_varint(w, 0);
            memmove(w, ubuf, ulen);
            clen = ulen;
        }
        clen += (w-cbuf);
        write_packet_raw(cbuf, clen, tx);

#if 0
        printf("%c P clen=%6zd    ",(pkt->type&0x1000000)?'C':'S',clen);
        hexprint(cbuf, LIM64(clen));
#endif

    }
    else {
        // no compression - simply append the packet to the transmission buffer
        write_packet_raw(ubuf, ulen, tx);
#if 0
        printf("%c P ulen=%6zd    ",(pkt->type&0x1000000)?'C':'S',ulen);
        hexprint(ubuf, LIM64(ulen));
#endif

    }
}

void process_play_packet(int is_client, uint8_t *ptr, uint8_t *lim,
                         lh_buf_t *tx, lh_buf_t *bx) {

    char comp = ' ';

    uint8_t *raw_ptr = ptr;       // start of the raw packet (with the complen field)
    uint8_t *raw_lim = lim;       // limit ptr of the raw data
    ssize_t  raw_len = lim-ptr;   // and its length

    uint8_t *p       = ptr;       // decoding pointer, after passing the decomp code
                                  // it should be pointing at the packet type field
    uint8_t *plim    = lim;       // limit ptr of the packet data
    ssize_t  plen    = plim-p;    // length of the decompressed data

    if (mitm.comptr>=0) {
        // compression is enabled
        comp = '.';
        Rvarint(usize); // supposed size of uncompressed data

        if (usize>0) {
            // packet is compressed - uncompress into temp buffer
            comp = '*';
            plen = lh_zlib_decode_to(p,plen,ubuf,sizeof(ubuf));
            assert(plen==usize);

            // correct p and lim to match the decompressed packet
            p=ubuf;
            plim = p+plen;
        }
        // usize==0 means the packet is not compressed, so in effect we simply
        // moved the decoding pointer to the start of the actual packet data

        plen = plim-p;
    }

#if 0
    printf("%c P  len=%6zd %c  ",is_client?'C':'S',raw_len,comp);
    hexprint(raw_ptr, LIM64(raw_len));
#endif

#if 0
    printf("%c P plen=%6zd    ",is_client?'C':'S',plen,comp);
    hexprint(p, LIM64(plen));
#endif

    MCPacket *pkt=decode_packet(is_client, p, plen);
    if (!pkt) {
        printf("Failed to decode packet\n");
        return;
    }

    //TODO: enable compression if SP_SetCompression is received

#if 0
    printf("MCPacket @%p:\n",pkt);
    printf("  type =%08x\n",pkt->type);
    printf("  proto=%08x\n",pkt->protocol);
    printf("    data=%p, len=%zd\n",pkt->p_UnknownPacket.data,pkt->p_UnknownPacket.length);

    hexdump(pkt->p_UnknownPacket.data,LIM128(pkt->p_UnknownPacket.length));
    printf("--------------------------------------------------------------------------------\n");
#endif

    ////////////////////////////////////////////////////////////////////////////

    MCPacketQueue tq = {NULL,0}, bq = {NULL,0};

    handle_packet(pkt, &tq, &bq);

    ////////////////////////////////////////////////////////////////////////////

    int i;
    for(i=0; i<C(tq.queue); i++) {
        MCPacket * tpkt = P(tq.queue)[i];
        write_packet(tpkt, tx);
        free_packet(tpkt);
    }
    for(i=0; i<C(bq.queue); i++) {
        MCPacket * bpkt = P(bq.queue)[i];
        write_packet(bpkt, bx);
        free_packet(bpkt);
    }

    lh_free(P(tq.queue));
    lh_free(P(bq.queue));
}


////////////////////////////////////////////////////////////////////////////////

// handle data incoming on the server or client connection
ssize_t handle_proxy(lh_conn *conn) {
    int is_client = (conn->priv != NULL);

    if (conn->status&CONN_STATUS_REMOTE_EOF) {
        // one of the parties has closed the connection.
        // close both sides, deinit the MITM state and
        // remove our descriptors from the pollarray

        close(mitm.cs);
        close(mitm.ms);
        mitm.state = STATE_IDLE;
        mitm.cs = mitm.ms = -1;
        lh_conn_remove(mitm.cs_conn);
        lh_conn_remove(mitm.ms_conn);
        mitm.comptr = -1;

        return 0;
    }

    // determine decoded buffers for input (rx), output (tx) and retour (bx)
    lh_buf_t *rx = is_client ? &mitm.cs_rx : &mitm.ms_rx;
    lh_buf_t *tx = is_client ? &mitm.cs_tx : &mitm.ms_tx;
    lh_buf_t *bx = is_client ? &mitm.ms_tx : &mitm.cs_tx;

    assert(conn->rbuf.P(data));

    // sptr,slen - pointer and length of data in the receive buffer
    ssize_t slen = conn->rbuf.C(data) - conn->rbuf.ridx;
    uint8_t *sptr = conn->rbuf.P(data) + conn->rbuf.ridx;

    // provide necessary space in the decoded receive buffer
    ssize_t widx = rx->C(data);
    lh_arr_add(GAR4(rx->data),slen);

#if 0
    printf("*** network data %s ***\n",is_client?"C->S":"C<-S");
    hexdump(sptr, slen);
    printf("************************\n");
#endif

    if (mitm.encryption_active) {
        // the connection is already authenticated, decrypt data
        int num = 0;
        if (is_client)
            AES_cfb8_encrypt(sptr, rx->P(data)+widx, slen,
                             &mitm.c_aes, mitm.c_dec_iv, &num, AES_DECRYPT);
        else
            AES_cfb8_encrypt(sptr, rx->P(data)+widx, slen,
                             &mitm.s_aes, mitm.s_dec_iv, &num, AES_DECRYPT);
    }
    else {
        // the authentication phase is not over yet - plaintext data
        memmove(rx->P(data)+widx, sptr, slen);
    }

    // at this point, the rx buffer contains raw, but decrypted data,
    // possibly also packets that could not be processed before
    // (because they were incomplete)

#if 0
    printf("*** decrypted data %s ***\n",is_client?"C->S":"C<-S");
    hexdump(rx->P(data), rx->C(data));
    printf("************************\n");
#endif

    //assert(bx->C(data)==0);

    // try to extract as many packets from the stream as we can in a loop
    while(rx->C(data) > 0) {
        //hexdump(AR(rx->data));
        // do we have a complete packet?
        uint8_t *p = rx->P(data);

        // large varint, data is definitely too short
        if (((*p)&0x80)&&(rx->C(data)<129)) break;

        uint32_t plen = lh_read_varint(p);
        ssize_t ll = p-rx->P(data); // length of the varint
        if (plen+ll > rx->C(data)) break; // packet is incomplete

        if (mitm.output) {
            // write packet to the MCS file
            struct timeval tv;
            gettimeofday(&tv, NULL);

            uint8_t header[4096];
            uint8_t *hp = header;
            write_int(hp, is_client);
            write_int(hp, tv.tv_sec);
            write_int(hp, tv.tv_usec);
            write_int(hp, plen);
            fwrite(header, 1, hp-header, mitm.output);
            fwrite(p, 1, plen, mitm.output);
            fflush(mitm.output);
        }

        // decode and process packet - this will also put a forwarded
        // data and/or responses into tx and bx buffers respectively as needed
        if ( mitm.state == STATE_PLAY )
            // PLAY packets are processed in mcp_game module
            process_play_packet(is_client, p, p+plen, tx, bx);
            //write_packet_raw(p, plen, tx);
        else
            // handle IDLE, STATUS and LOGIN packets here
            process_packet(is_client, p, plen, tx);

        // remove processed packet from the buffer
        lh_arr_delete_range(GAR4(rx->data),0,ll+plen);
    }

    // if there's data in the transmission buffer, encrypt it if needed and send off
    if (tx->C(data) > 0) {
        if (mitm.encryption_active) {
            // since we always write out all data, we just encrypt this in-place
            int num=0;
            if (is_client)
                AES_cfb8_encrypt(tx->P(data), tx->P(data), tx->C(data),
                                 &mitm.s_aes, mitm.s_enc_iv, &num, AES_ENCRYPT);
            else
                AES_cfb8_encrypt(tx->P(data), tx->P(data), tx->C(data),
                                 &mitm.c_aes, mitm.c_enc_iv, &num, AES_ENCRYPT);
        }
        
        // send everything
        lh_conn_write(is_client?mitm.ms_conn:mitm.cs_conn, AR(tx->data));
        tx->C(data) = tx->ridx = 0;
    }
    
    // if there's data in the response buffer, encrypt it if needed and send off
    if (bx->C(data) > 0) {
        if (mitm.encryption_active) {
            // since we always write out all data, we just encrypt this in-place
            int num=0;
            if (is_client)
                AES_cfb8_encrypt(bx->P(data), bx->P(data), bx->C(data),
                                 &mitm.c_aes, mitm.c_enc_iv, &num, AES_ENCRYPT);
            else
                AES_cfb8_encrypt(bx->P(data), bx->P(data), bx->C(data),
                                 &mitm.s_aes, mitm.s_enc_iv, &num, AES_ENCRYPT);
        }

        // send everything
        lh_conn_write(is_client?mitm.cs_conn:mitm.ms_conn, AR(bx->data));
        bx->C(data) = bx->ridx = 0;
    }
    
    if (mitm.enable_encryption) {
        // Set up the encryption. This is delayed so the last auth phase packet
        // CL_EncryptionResponse can go out unencrypted
        AES_set_encrypt_key(mitm.c_skey, 128, &mitm.c_aes);
        memcpy(mitm.c_enc_iv, mitm.c_skey, 16);
        memcpy(mitm.c_dec_iv, mitm.c_skey, 16);

        AES_set_encrypt_key(mitm.s_skey, 128, &mitm.s_aes);
        memcpy(mitm.s_enc_iv, mitm.s_skey, 16);
        memcpy(mitm.s_dec_iv, mitm.s_skey, 16);

#if 0
        printf("c_skey:   "); hexdump(mitm.c_skey,16);
        printf("c_enc_iv: "); hexdump(mitm.c_enc_iv,16);
        printf("c_dec_iv: "); hexdump(mitm.c_dec_iv,16);
        printf("s_skey:   "); hexdump(mitm.s_skey,16);
        printf("s_enc_iv: "); hexdump(mitm.s_enc_iv,16);
        printf("s_dec_iv: "); hexdump(mitm.s_dec_iv,16);
#endif

        mitm.enable_encryption=0;
        mitm.encryption_active=1;
        // from now on the connection is authenticated and encrypted
    }

    return slen;
}

// emergency connection drop - used to protect ourselves from the thunder
void drop_connection() {
    close(mitm.ms);
    close(mitm.cs);
}

////////////////////////////////////////////////////////////////////////////////
// Session Server

/*
  This is needed to handle the authentication process - we need to intercept
  the HTTP request to the session server to get the necessary access token
  Note that for this to work at all, you need to modify the Minecraft launcher
  libraries and hack in a http URL pointing to mcproxy
  (i.e. http://localhost:8080) instead of the official HTTPS server
  (https://sessionserver.mojang.com). This can be done by unpacking
  ~/.minecraft/libraries/com/mojang/authlib/<VER>/authlib-<VER>.jar and editing
  com/mojang/authlib/yggdrasil/YggdrasilMinecraftSessionService.class within
  using a hexeditor. With this neat trick we can avoid having to MITM an
  HTTPS connection, which would be a pain in the ass to implement.

*/                                                                                                                                             

#if 0
//Example of the HTTP conversation:

//POST /session/minecraft/join HTTP/1.1
//Content-Type: application/json; charset=utf-8
//Cache-Control: no-cache
//Pragma: no-cache
//User-Agent: Java/1.6.0_27
//Host: ssessionserver.mojang.com
//Accept: text/html, image/gif, image/jpeg, *; q=.2, */*; q=.2
//Connection: keep-alive
//Content-Length: 156

//{"accessToken":"bbc3cae3264e4ad0b446fd9bb852519a","selectedProfile":"962c6718688448d4a35c249f8d30428b","serverId":"bd651042ec97910e449e11a3991e1274e3e67e5"}HTTP/1.1 401 Authorization Required

//HTTP/1.1 204 No Content                                                                                                    
//Accept-Ranges: bytes                                                                                                       
//Content-length: 0                                                                                                                     
//Date: Mon, 21 Apr 2014 13:13:54 GMT                                                                                                   
//Server: Restlet-Framework/2.2.0                                                                                                       
//Connection: keep-alive                                                                                                                
#endif


int parseJson(const char *buf, const char *str, char *dst, ssize_t size) {
    const char *name = strstr(buf, str);
    if (!name) return 0;

    while(*name != ':') name++;
    while(*name != '"') name++;
    name++; // now it points to the start of the value

    char *d = dst;
    while(*name != '"' && ((d-dst)<(size-1))) { *d++ = *name++; }
    *d = 0;

    return 1;
}

int handle_session_server(int sfd) {
    // accept connection from the client side
    struct sockaddr_in cadr;
    int cs = lh_accept_tcp4(sfd, &cadr);
    lh_net_blocking(cs);
    if (cs < 0)
        LH_ERROR(0, "Failed to accept the client-side connection");
    printf("Accepted from %s:%d (Webserver)\n",
           inet_ntoa(cadr.sin_addr),ntohs(cadr.sin_port));

    // create a stream connection
    FILE *fp = fdopen(cs,"r+");
    if (!fp) {
        close(cs);
        LH_ERROR(0, "Failed to create a stream connection");
    }

    // read the header
    int clen = 0;
    char buf[262144];
    while(1) {
        if (!fgets(buf, sizeof(buf), fp)) {
            fclose(fp);
            LH_ERROR(0, "Failed to read the header");
        }

        //printf(">%s<\n",buf);

        if (!memcmp(buf, "\r\n\0", 3))
            break;

        if (sscanf(buf, "Content-Length: %u", &clen)==1)
            clen=clen; //printf("parsed the content length : %d\n",clen);
    }
    //printf("parsed the header completely\n");

    // read the POST body
    fread(buf, 1, clen, fp);

    //hexdump(buf, clen);
    buf[clen] = 0;
    //printf(">%s<\n",buf);

    // parse the JSON (Q&D) and store the extracted tokens in the mitm struct
    if ( ! (
            parseJson(buf,"accessToken",mitm.accessToken,sizeof(mitm.accessToken))&&
            parseJson(buf,"selectedProfile",mitm.selectedProfile,sizeof(mitm.selectedProfile))&&
            parseJson(buf,"serverId",mitm.serverId,sizeof(mitm.serverId))
            )) {
        fclose(fp);
        LH_ERROR(0, "Failed to parse JSON");
    }

#if 0
    printf("accessToken:     >%s<\n",mitm.accessToken);
    printf("selectedProfile: >%s<\n",mitm.selectedProfile);
    printf("serverId:        >%s<\n",mitm.serverId);
#endif


    // send response
    time_t ts;
    time(&ts);
    fprintf(fp,
            "HTTP/1.1 204 No Content\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-length: 0\r\n"
            "Date: %s\r\n"
            "Server: Restlet-Framework/2.2.0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",asctime(gmtime(&ts)));
    fflush(fp);
    fclose(fp);
    return 1;
}

void print_hex(char *buf, const char *data, ssize_t len) {
    int i;
    char *w = buf;
    int f = 0;
    if (data[0] < 0) {
        *w++ = '-';
        f = 1;
    }
    for(i=0; i<len; i++) {
        char d = data[i];
        if (f) {
            d = -d;
            if (i<len-1)
                d--;
        }
        
        sprintf(w,"%02x",(unsigned char)d);
        w+=2;
    }

    *w++ = 0;

    w = buf+f;
    char *z = w;
    while(*z == '0') z++;
    while(*z) *w++ = *z++;
    *w++ = 0;
}

// this is the server-side handling of the session authentication
// we use Curl to send an HTTPS request to Mojangs session server
int query_auth_server() {
    // the final touch - send the authentication token to the session server
    unsigned char md[SHA_DIGEST_LENGTH];
    SHA_CTX sha; CLEAR(sha);
    
    SHA1_Init(&sha);
    SHA1_Update(&sha, mitm.s_id, strlen(mitm.s_id));
    SHA1_Update(&sha, mitm.s_skey, sizeof(mitm.s_skey));
    SHA1_Update(&sha, mitm.s_pkey, mitm.s_pklen);
    SHA1_Final(md, &sha);
    
    char auth[4096];
    //hexdump(md, SHA_DIGEST_LENGTH);
    print_hex(auth, md, SHA_DIGEST_LENGTH);
    printf("sessionId : %s\n", auth);

    char buf[4096];
    sprintf(buf,"{\"accessToken\":\"%s\",\"selectedProfile\":\"%s\",\"serverId\":\"%s\"}",
            mitm.accessToken, mitm.selectedProfile, auth);

    //printf("request to session server: %s\n",buf);

    // perform a request with a cURL client

    CURL *curl = curl_easy_init();
    CURLcode res;

    // set header options
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_easy_setopt(curl, CURLOPT_URL, "https://sessionserver.mojang.com/session/minecraft/join");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Java/1.6.0_27");
    
    struct curl_slist *headerlist=NULL;
    headerlist = curl_slist_append(headerlist, "Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);

    // set body - our JSON blob
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);

    // make a request
    res = curl_easy_perform(curl);
    if(res != CURLE_OK)
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
 
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Minecraft client connections

// this function is called when a MC client tries to connect to our proxy
int handle_server(int sfd, uint32_t ip, uint16_t port) {
    // accept connection from the local client
    struct sockaddr_in cadr;
    int cs = lh_accept_tcp4(sfd, &cadr);
    if (cs < 0)
        LH_ERROR(0, "Failed to accept the client-side connection");

    printf("Accepted from %s:%d\n",
           inet_ntoa(cadr.sin_addr),ntohs(cadr.sin_port));
    
    // open connection to the remote server (the real MC server)
    int ms = lh_connect_tcp4(ip, port);
    if (ms < 0) {
        close(mitm.cs);
        LH_ERROR(0, "Failed to open the client-side connection");
    }

    // both client-side and server-side connections are now established
    printf("New connection: cs=%d ms=%d\n", cs, ms);
    
    // initialize mitm struct, terminate old state if any
    if (mitm.output) fclose(mitm.output);
    if (mitm.dbg)    fclose(mitm.dbg);
    if (mitm.s_rsa) RSA_free(mitm.s_rsa);
    if (mitm.c_rsa) RSA_free(mitm.c_rsa);
    lh_free(P(mitm.cs_rx.data));
    lh_free(P(mitm.cs_tx.data));
    lh_free(P(mitm.ms_rx.data));
    lh_free(P(mitm.ms_tx.data));
    CLEAR(mitm);
    //DISABLED clear_autobuild();

#if 0
    //DISABLED
    reset_gamestate();
    set_option(GSOP_PRUNE_CHUNKS, 1);
    set_option(GSOP_SEARCH_SPAWNERS, 1);
    set_option(GSOP_TRACK_ENTITIES, 1);
#endif

    // open a new .mcp file to capture MC protocol data
    char fname[4096];
    time_t t;
    time(&t);
    strftime(fname, sizeof(fname), "saved/%Y%m%d_%H%M%S.mcs",localtime(&t));
    mitm.output = fopen(fname, "w");
    if (!mitm.output) {
        close(mitm.ms);
        close(mitm.cs);
        LH_ERROR(0, "Failed to open the .mcp trace %s for writing", fname);
    }
    setvbuf(mitm.output, NULL, _IONBF, 0);

    // open debug log file
    //strftime(fname, sizeof(fname), "saved/%Y%m%d_%H%M%S.dbg",localtime(&t));
    //mitm.dbg = fopen(fname, "w");
    //if (!mitm.dbg) {
    //    close(mitm.ms);
    //    close(mitm.cs);
    //    fclose(mitm.output);
    //    LH_ERROR(0, "Failed to open the debug trace %s for writing", fname);
    //}
    //setvbuf(mitm.dbg, NULL, _IONBF, 0);

    // handle_server was able to accept the client connection and
    // also open the server-side connection, we need to add these
    // new sockets to the groups cg and mg respectively
    mitm.cs = cs;
    mitm.ms = ms;
    mitm.cs_conn = lh_conn_add(&pa, cs, G_PROXY, (void*)1);
    mitm.ms_conn = lh_conn_add(&pa, ms, G_PROXY, (void*)0);

    // disable compression state
    mitm.comptr = -1;

    // from now on, all data arriving from the server or client will be
    // handled by handle_proxy called from the event loop

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Main loop

int proxy_pump(uint32_t ip, uint16_t port) {
    CLEAR(pa);

    CLEAR(mitm);
    //DISABLED clear_autobuild();
    mitm.cs = mitm.ms = -1;

    // Minecraft proxy server
    int ss = lh_listen_tcp4_any(port);
    if (ss<0) return -1;
    lh_poll_add(&pa, ss, POLLIN, G_MCSERVER, NULL);

    // fake session.minecraft.com web server
    int ws = lh_listen_tcp4_any(WEBSERVER_PORT);
    if (ws<0) return -1;
    lh_poll_add(&pa, ws, POLLIN, G_WEBSERVER, NULL);

    // prepare signal handling
    signal_caught = 0;
    struct sigaction sa;
    CLEAR(sa);
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL))
        LH_ERROR(1,"Failed to set sigaction\n");

    // main event loop
    int i;
    while(!signal_caught) {
        lh_poll(&pa, 1000); // poll all sockets

        lh_polldata *pd;

        // handle connection requests on the web server socket
        if ( pd=lh_poll_getfirst(&pa, G_WEBSERVER, POLLIN))
            handle_session_server(pd->fd);

        // handle connection requests on the MC server socket
        if ( pd=lh_poll_getfirst(&pa, G_MCSERVER, POLLIN))
            handle_server(pd->fd, ip, port);

        // handle client- and server-side connection
        lh_conn_process(&pa, G_PROXY, handle_proxy);

#if 0
        //DISABLED
        // handle asynchronous events (timers etc.)
        if (mitm.state == STATE_PLAY)
            handle_async(&mitm.ms_tx, &mitm.cs_tx);
#endif
    }

    printf("Terminating...\n");

    // flush MCP saved file
    if (mitm.output) {
        fflush(mitm.output);
        fclose(mitm.output);
        mitm.output = NULL;
    }

    // flush debug log if active
    if (mitm.dbg) {
        fclose(mitm.dbg);
        mitm.dbg = NULL;
    }

    // free buffers
    lh_free(P(mitm.cs_rx.data));
    lh_free(P(mitm.cs_tx.data));
    lh_free(P(mitm.ms_rx.data));
    lh_free(P(mitm.ms_tx.data));
}

int main(int ac, char **av) {
#if DEBUG_MEMORY
    mtrace();
#endif

    // if an argument is specified - it's the server address we want to
    // forward connections to, otherwise - 2b2t.org
    uint32_t server_ip = lh_dns_addr_ipv4(av[1]?av[1]:SERVER_ADDR);
    if (server_ip == 0xffffffff)
        LH_ERROR(-1, "Failed to obtain IP address for the server %s",SERVER_ADDR);

    // start monitoring connection events
    proxy_pump(server_ip, SERVER_PORT);

#if DEBUG_MEMORY
    muntrace();
#endif

    return 0;
}
