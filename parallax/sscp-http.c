/*
    sscp-http.c - Simple Serial Command Protocol HTTP support

	Copyright (c) 2016 Parallax Inc.
    See the file LICENSE.txt for licensing information.
*/

#include "esp8266.h"
#include "sscp.h"
#include "config.h"
#include "httpd.h"

static void send_connect_event(sscp_connection *connection, int prefix);
static void send_disconnect_event(sscp_connection *connection, int prefix);
static void send_reconnect_event(sscp_connection *connection, int prefix);
static void send_data_event(sscp_connection *connection, int prefix);
static void send_txdone_event(sscp_connection *connection, int prefix);
static int checkForEvents_handler(sscp_hdr *hdr);
static void path_handler(sscp_hdr *hdr); 
static void send_handler(sscp_hdr *hdr, int size);
static void recv_handler(sscp_hdr *hdr, int size);
static void close_handler(sscp_hdr *hdr);

static sscp_dispatch httpDispatch = {
    .checkForEvents = checkForEvents_handler,
    .path = path_handler,
    .send = send_handler,
    .recv = recv_handler,
    .close = close_handler
};

/*
    This function is called when a request matches the path or path fragment registered
    by a LISTEN command from the MCU. It is called in several contexts:
    
    1) with connData->conn == NULL to cleanup after a connection is terminated
    2) with connData->cgiData == NULL when a request first arrives
    3) with connData->cgiData != NULL when additional request data arrives
    4) with connData->cgiData != NULL when a send completes
*/

int ICACHE_FLASH_ATTR cgiSSCPHandleRequest(HttpdConnData *connData)
{
    sscp_connection *connection = (sscp_connection *)connData->cgiData;
    sscp_listener *listener;
    
    // check for the cleanup call (CGI_CB_DISCONNECT or CGI_CB_RECONNECT)
    if (connData->conn == NULL) {
        if (connection) {
            if (connData->cgiReason == CGI_CB_DISCONNECT) {
sscp_log("sscp: disconnecting %d", connection->hdr.handle);
                connection->flags |= CONNECTION_TERM;
                if (flashConfig.sscp_events)
                    send_disconnect_event(connection, '!');
            }
            else {
sscp_log("sscp: disconnecting after failure %d", connection->hdr.handle);
                connection->flags |= CONNECTION_FAIL;
                connection->error = connData->cgiValue;
                if (flashConfig.sscp_events)
                    send_reconnect_event(connection, '!');
            }
        }
        return HTTPD_CGI_DONE;
    }
    
    // check to see if this request is already in progress (CGI_CB_RECV or CGI_CB_SENT)
    if (connection) {
        int ret;
        
sscp_log("REPLY send complete");
       connection->flags |= CONNECTION_TXDONE; 

        if ((connection->txIndex += connection->d.http.count) < connection->txCount)
            ret = HTTPD_CGI_MORE;
        else {
sscp_log("REPLY complete");
            connection->flags |= CONNECTION_TXFREE; 
            ret = HTTPD_CGI_DONE;
        }

        if (flashConfig.sscp_events)
            send_txdone_event(connection, '!');

        return ret;
    }

    // find a matching listener (first CGI_CB_RECV)
    if (!(listener = sscp_find_listener(connData->url, TYPE_HTTP_LISTENER)))
        return HTTPD_CGI_NOTFOUND;

    // allocate a connection
    if (!(connection = sscp_allocate_connection(TYPE_HTTP_CONNECTION, &httpDispatch))) {
        httpdStartResponse(connData, 400);
        httpdEndHeaders(connData);
os_printf("sscp: no connections available for %s request\n", connData->url);
        httpdSend(connData, "No connections available", -1);
        return HTTPD_CGI_DONE;
    }
    connection->listenerHandle = listener->hdr.handle;
    connData->cgiData = connection;
    connection->d.http.conn = connData;

sscp_log("sscp: %d handling %s request", connection->hdr.handle, connData->url);
    if (flashConfig.sscp_events)
        send_connect_event(connection, '!');
        
    return HTTPD_CGI_MORE;
}

// ARG,chan
void ICACHE_FLASH_ATTR http_do_arg(int argc, char *argv[])
{
    char buf[128];
    sscp_connection *connection;
    HttpdConnData *connData;
    
    if (argc != 3) {
        sscp_sendResponse("E,%d", SSCP_ERROR_WRONG_ARGUMENT_COUNT);
        return;
    }
    
    if (!(connection = sscp_get_connection(atoi(argv[1])))) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_ARGUMENT);
        return;
    }

    if (connection->hdr.type != TYPE_HTTP_CONNECTION) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_ARGUMENT);
        return;
    }
    
    if (!(connData = (HttpdConnData *)connection->d.http.conn) || connData->conn == NULL) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_STATE);
        return;
    }
    
    if (httpdFindArg(connData->getArgs, argv[2], buf, sizeof(buf)) >= 0) {
        sscp_sendResponse("S,%s", buf);
        return;
    }

    if (!connData->post->buff) {
        sscp_sendResponse("N,0");
        return;
    }
    
    if (httpdFindArg(connData->post->buff, argv[2], buf, sizeof(buf)) >= 0) {
        sscp_sendResponse("S,%s", buf);
        return;
    }

    sscp_sendResponse("N,0");
}

#define MAX_SENDBUFF_LEN 1024

// this is called after all of the data for a REPLY has been received from the MCU
static void ICACHE_FLASH_ATTR reply_cb(void *data, int count)
{
    sscp_connection *connection = (sscp_connection *)data;
    HttpdConnData *connData = connection->d.http.conn;
    
    char sendBuff[MAX_SENDBUFF_LEN];
    httpdSetSendBuffer(connData, sendBuff, sizeof(sendBuff));
    
sscp_log("REPLY payload callback: %d bytes of %d", count, connection->txCount);
    char buf[20];
    os_sprintf(buf, "%d", connection->txCount);
    
    httpdStartResponse(connData, connection->d.http.code);
    httpdHeader(connData, "Content-Length", buf);
    httpdEndHeaders(connData);
    httpdSend(connData, connection->txBuffer, count);
    httpdFlushSendBuffer(connData);
    
    connection->flags &= ~CONNECTION_TXFULL;
    sscp_sendResponse("S,%d", connection->d.http.count);
    
    connection->d.http.count = count;
}

// REPLY,chan,code[,payload-size]
void ICACHE_FLASH_ATTR http_do_reply(int argc, char *argv[])
{
    sscp_connection *connection;
    HttpdConnData *connData;
    int count;

    if (argc < 3 || argc > 5) {
        sscp_sendResponse("E,%d", SSCP_ERROR_WRONG_ARGUMENT_COUNT);
        return;
    }
    
    if (!(connection = sscp_get_connection(atoi(argv[1])))) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_ARGUMENT);
        return;
    }

    if (connection->hdr.type != TYPE_HTTP_CONNECTION) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_ARGUMENT);
        return;
    }
        
    if (!(connData = (HttpdConnData *)connection->d.http.conn) || connData->conn == NULL) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_STATE);
        return;
    }
    
    connection->d.http.code = atoi(argv[2]);
    
    connection->txCount = (argc > 3 ? atoi(argv[3]) : 0);
    count = (argc > 4 ? atoi(argv[4]) : connection->txCount);
    connection->txIndex = 0;
sscp_log("REPLY: total %d, this %d", connection->txCount, count);

    if (connection->txCount < 0
    ||  count < 0
    ||  connection->txCount < count
    ||  count > SSCP_TX_BUFFER_MAX) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_SIZE);
        return;
    }
    
    // response is sent by reply_cb
    if (connection->txCount == 0)
        reply_cb(connection, 0);
    else {
        sscp_capturePayload(connection->txBuffer, count, reply_cb, connection);
        connection->flags |= CONNECTION_TXFULL;
    }
}

static void ICACHE_FLASH_ATTR send_connect_event(sscp_connection *connection, int prefix)
{
    connection->flags &= ~CONNECTION_INIT;
    HttpdConnData *connData = (HttpdConnData *)connection->d.http.conn;
    if (connData) {
        switch (connData->requestType) {
        case HTTPD_METHOD_GET:
            sscp_send(prefix, "G,%d,%d", connection->hdr.handle, connection->listenerHandle);
            break;
        case HTTPD_METHOD_POST:
            sscp_send(prefix, "P,%d,%d", connection->hdr.handle, connection->listenerHandle);
            break;
        default:
            sscp_send(prefix, "E,%d,%d", SSCP_ERROR_INVALID_METHOD, connData->requestType);
            break;
        }
    }
}

static void ICACHE_FLASH_ATTR send_disconnect_event(sscp_connection *connection, int prefix)
{
    sscp_send(prefix, "X,%d,0", connection->hdr.handle);
    sscp_close_connection(connection);
}

static void ICACHE_FLASH_ATTR send_reconnect_event(sscp_connection *connection, int prefix)
{
    sscp_send(prefix, "E,%d,%d", connection->hdr.handle, connection->error);
    sscp_close_connection(connection);
}

static void ICACHE_FLASH_ATTR send_data_event(sscp_connection *connection, int prefix)
{
    connection->flags &= ~CONNECTION_RXFULL;
    sscp_send(prefix, "D,%d,%d", connection->hdr.handle, connection->listenerHandle);
}

static void ICACHE_FLASH_ATTR send_txdone_event(sscp_connection *connection, int prefix)
{
    connection->flags &= ~CONNECTION_TXDONE;
    sscp_send(prefix, "S,%d,0", connection->hdr.handle);
    if (connection->flags & CONNECTION_TXFREE)
        connection->hdr.type = TYPE_UNUSED;
}

static int ICACHE_FLASH_ATTR checkForEvents_handler(sscp_hdr *hdr)
{
    sscp_connection *connection = (sscp_connection *)hdr;
    
    if (!connection->d.http.conn)
        return 0;
        
    if (connection->flags & CONNECTION_TXDONE) {
        send_txdone_event(connection, '=');
        return 1;
    }
    
    else if (connection->flags & CONNECTION_TERM) {
        send_disconnect_event(connection, '=');
        return 1;
    }
    
    else if (connection->flags & CONNECTION_FAIL) {
        send_reconnect_event(connection, '=');
        return 1;
    }
    
    else if (connection->flags & CONNECTION_INIT) {
        send_connect_event(connection, '=');
        return 1;
    }
    
    else if (connection->flags & CONNECTION_RXFULL) {
        send_data_event(connection, '=');
        return 1;
    }
    
    return 0;
}

static void ICACHE_FLASH_ATTR path_handler(sscp_hdr *hdr)
{
    sscp_connection *connection = (sscp_connection *)hdr;
    HttpdConnData *connData = (HttpdConnData *)connection->d.http.conn;
    
    if (!connData || !connData->conn) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_STATE);
        return;
    }
    
    sscp_sendResponse("S,%s", connData->url);
}

// this is called after all of the data for a SEND has been received from the MCU
static void ICACHE_FLASH_ATTR send_cb(void *data, int count)
{
    sscp_connection *connection = (sscp_connection *)data;
    HttpdConnData *connData = connection->d.http.conn;
sscp_log("  captured %d bytes", count);
    
    httpdUnbufferedSend(connData, connection->txBuffer, count);
    
    connection->flags &= ~CONNECTION_TXFULL;
    sscp_sendResponse("S,%d", connection->d.http.count);

    connection->d.http.count = count;
}

static void ICACHE_FLASH_ATTR send_handler(sscp_hdr *hdr, int size)
{
    sscp_connection *connection = (sscp_connection *)hdr;
    
    if (connection->txIndex + size > connection->txCount) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_SIZE);
        return;
    }
    
    if (size == 0)
        sscp_sendResponse("S,0");
    else {
        // response is sent by send_cb
        sscp_capturePayload(connection->txBuffer, size, send_cb, connection);
        connection->flags |= CONNECTION_TXFULL;
    }
}

static void ICACHE_FLASH_ATTR recv_handler(sscp_hdr *hdr, int size)
{
    sscp_connection *connection = (sscp_connection *)hdr;
    HttpdConnData *connData;
    int rxCount;

    if (!(connData = (HttpdConnData *)connection->d.http.conn) || connData->conn == NULL) {
        sscp_sendResponse("E,%d", SSCP_ERROR_INVALID_STATE);
        return;
    }
    
    rxCount = connData->post->buff ? connData->post->len : 0;

    if (connection->rxIndex + size > rxCount)
        size = rxCount - connection->rxIndex;

    sscp_sendResponse("S,%d", size);
    if (size > 0) {
        sscp_sendPayload(connData->post->buff + connection->rxIndex, size);
        connection->rxIndex += size;
    }
}

static void ICACHE_FLASH_ATTR close_handler(sscp_hdr *hdr)
{
    sscp_connection *connection = (sscp_connection *)hdr;
    HttpdConnData *connData = connection->d.http.conn;
    if (connData)
        connData->cgi = NULL;
}
