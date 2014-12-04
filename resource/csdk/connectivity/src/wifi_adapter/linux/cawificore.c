#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>

#include "cawificore.h"
#include "logger.h"
#include "uthreadpool.h" /* for thread pool */
#include "umutex.h"
#include "caqueueingthread.h"
#include "oic_malloc.h"

#define TAG PCF("CA")

#define CA_MAX_BUFFER_SIZE 512  // Max length of buffer#define CA_UNICAST_PORT 5383 // The port on which to listen for incoming data#define CA_MULTICAST_ADDR "224.0.1.187"
#define CA_MULTICAST_PORT 5683

typedef enum
{
    CA_UNICAST = 1, CA_MULTICAST
} CATransmissionType_t;

typedef struct
{
    CATransmissionType_t transmissionType; // 0: none, 1: unicast, 2: multicast
    char* address;
    int port;
    void* data;
} CAThreadData_t;

typedef struct
{
    u_mutex threadMutex;
    u_cond threadCond;
    int32_t isStop;
    int32_t status; // 0: stopped, 1: running
} CATask_t;

int32_t unicast_receive_socket;
struct sockaddr_in multicast_send_interface_addr;
int32_t multicast_receive_socket;
struct sockaddr_in multicast_receive_interface_addr;

static CAPacketReceiveCallback gPacketReceiveCallback = NULL;

static u_thread_pool_t gThreadPoolHandle = NULL;

// message handler main thread
static CAQueueingThread_t gSendThread;
static CAQueueingThread_t gReceiveThread;

CATask_t unicastListenTask;
CATask_t multicastListenTask;

static void CASendProcess(void* threadData)
{
    OIC_LOG(DEBUG, TAG, "CASendThreadProcess");

    CAThreadData_t* data = (CAThreadData_t*) threadData;
    if (data == NULL)
    {
        OIC_LOG(DEBUG, TAG, "thread data is error!");
        return;
    }

    if (data->transmissionType == CA_UNICAST)
    {
        // unicast
        CASendUnicastMessageImpl(data->address, (char*) (data->data));
    }
    else if (data->transmissionType == CA_MULTICAST)
    {
        // multicast
        CASendMulticastMessageImpl((char*) (data->data));
    }
}

static void CAReceiveProcess(void* threadData)
{
    OIC_LOG(DEBUG, TAG, "CAReceiveProcess");

    CAThreadData_t* data = (CAThreadData_t*) threadData;
    if (data == NULL)
    {
        OIC_LOG(DEBUG, TAG, "thread data is error!");
        return;
    }

    if (gPacketReceiveCallback != NULL)
    {
        gPacketReceiveCallback(data->address, (char*) (data->data));
    }
}

static void CAUnicastListenThread(void* threadData)
{
    OIC_LOG(DEBUG, TAG, "CAUnicastListenThread");

    char buf[CA_MAX_BUFFER_SIZE];
    int32_t recv_len;

    struct sockaddr_in si_other;
    int32_t slen = sizeof(si_other);

    while (!unicastListenTask.isStop)
    {
        OIC_LOG(DEBUG, TAG, "CAUnicastListenThread, Waiting for data...");
        fflush(stdout);

        memset(buf, 0, sizeof(char) * CA_MAX_BUFFER_SIZE);

        // try to receive some data, this is a blocking call
        if ((recv_len = recvfrom(unicast_receive_socket, buf, CA_MAX_BUFFER_SIZE, 0,
                (struct sockaddr *) &si_other, (socklen_t *) &slen)) == -1)
        {
            OIC_LOG(DEBUG, TAG, "CAUnicastListenThread, recv_len() error");
            continue;
        }

        // print details of the client/peer and the data received
        OIC_LOG_V(DEBUG, TAG, "CAUnicastListenThread, Received packet from %s:%d",
                inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));
        OIC_LOG_V(DEBUG, TAG, "CAUnicastListenThread, Data: %s", buf);

        // store the data at queue.
        CAThreadData_t* td = NULL;
        td = (CAThreadData_t*) OICMalloc(sizeof(CAThreadData_t));
        memset(td, 0, sizeof(CAThreadData_t));
        td->transmissionType = 1; // unicast

        char* _address = inet_ntoa(si_other.sin_addr);
        int len = strlen(_address);
        td->address = (char*) OICMalloc(sizeof(char) * (len + 1));
        memset(td->address, 0, len + 1);
        memcpy(td->address, _address, len);
        td->port = ntohs(si_other.sin_port);

        td->data = (void*) OICMalloc(sizeof(void) * CA_MAX_BUFFER_SIZE);
        memset(td->data, 0, CA_MAX_BUFFER_SIZE);
        memcpy(td->data, buf, sizeof(buf));

        CAQueueingThreadAddData(&gReceiveThread, td, sizeof(CAThreadData_t));
    }

    OIC_LOG(DEBUG, TAG, "end of CAUnicastListenThread");
}

static void CAMulticastListenThread(void* threadData)
{
    OIC_LOG(DEBUG, TAG, "CAMulticastListenThread");

    char msgbuf[CA_MAX_BUFFER_SIZE];

    struct sockaddr_in client;
    int32_t addrlen = sizeof(client);

    OIC_LOG(DEBUG, TAG, "CAMulticastListenThread, waiting for input...");

    while (!multicastListenTask.isStop)
    {
        int32_t recv_bytes = recvfrom(multicast_receive_socket, msgbuf, CA_MAX_BUFFER_SIZE, 0,
                (struct sockaddr *) &client, (socklen_t *) &addrlen);
        if (recv_bytes < 0)
        {
            if (errno != EAGAIN)
            {
                OIC_LOG(DEBUG, TAG, "CAMulticastListenThread, error recvfrom");

                return;
            }

            continue;
        }

        msgbuf[recv_bytes] = 0;

        OIC_LOG_V(DEBUG, TAG, "Received msg: %s, size: %d", msgbuf, recv_bytes);

        char* sender = inet_ntoa(client.sin_addr);
        char local[INET_ADDRSTRLEN];
        CAGetLocalAddress(local);
        if (strcmp(sender, local) == 0)
        {
            OIC_LOG_V(DEBUG, TAG, "skip the local request (via multicast)");
        }
        else
        {
            // store the data at queue.
            CAThreadData_t* td = NULL;
            td = (CAThreadData_t*) OICMalloc(sizeof(CAThreadData_t));
            memset(td, 0, sizeof(CAThreadData_t));
            td->transmissionType = 2; // multicast

            char* _address = inet_ntoa(client.sin_addr);
            int len = strlen(_address);
            td->address = (char*) OICMalloc(sizeof(char) * (len + 1));
            memset(td->address, 0, len + 1);
            memcpy(td->address, _address, len);
            td->port = ntohs(client.sin_port);

            td->data = (void*) OICMalloc(sizeof(void) * CA_MAX_BUFFER_SIZE);
            memset(td->data, 0, CA_MAX_BUFFER_SIZE);
            memcpy(td->data, msgbuf, sizeof(msgbuf));

            CAQueueingThreadAddData(&gReceiveThread, td, sizeof(CAThreadData_t));
        }

    }

    OIC_LOG(DEBUG, TAG, "end of CAMulticastListenThread");
}

void CAWiFiInitialize(u_thread_pool_t handle)
{
    OIC_LOG(DEBUG, TAG, "CAWiFiInitialize");

    gThreadPoolHandle = handle;

    // unicast/multicast send queue
    CAQueueingThreadInitialize(&gSendThread, gThreadPoolHandle, CASendProcess);

    // start send thread
    CAResult_t res = CAQueueingThreadStart(&gSendThread);
    if (res != CA_STATUS_OK)
    {
        OIC_LOG(DEBUG, TAG, "thread start is error (send thread)");
        // return res;
        return;
    }

    // unicast/multicast receive queue
    CAQueueingThreadInitialize(&gReceiveThread, gThreadPoolHandle, CAReceiveProcess);

    // start send thread
    res = CAQueueingThreadStart(&gReceiveThread);
    if (res != CA_STATUS_OK)
    {
        OIC_LOG(DEBUG, TAG, "thread start is error (receive thread)");
        // return res;
        return;
    }

    unicastListenTask.threadMutex = u_mutex_new();
    unicastListenTask.threadCond = u_cond_new();
    unicastListenTask.isStop = FALSE;
    unicastListenTask.status = 0; // stopped

    multicastListenTask.threadMutex = u_mutex_new();
    multicastListenTask.threadCond = u_cond_new();
    multicastListenTask.isStop = FALSE;
    multicastListenTask.status = 0; // stopped

    // [UDP Server]
    struct sockaddr_in si_me;

    // create a UDP socket
    if ((unicast_receive_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        OIC_LOG_V(DEBUG, TAG, "CAWiFiInit, creating socket failed");

        return;
    }

    OIC_LOG_V(DEBUG, TAG, "CAWiFiInit, socket created");

    // [multicast sender]
    uint32_t multiTTL = 1;

    // zero out the structure
    memset((char *) &si_me, 0, sizeof(si_me));

    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(CA_UNICAST_PORT);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);

    int32_t ret_val = setsockopt(unicast_receive_socket, SOL_SOCKET, SO_REUSEADDR, &multiTTL,
            sizeof(multiTTL));
    if (ret_val < 0)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiInit, Failed to set REUSEADDR");
    }

    // bind socket to port
    if (bind(unicast_receive_socket, (struct sockaddr*) &si_me, sizeof(si_me)) == -1)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiInit, binding socket failed");

        return;
    }

    OIC_LOG(DEBUG, TAG, "CAWiFiInit, socket binded");

    memset(&multicast_send_interface_addr, 0, sizeof(multicast_send_interface_addr));
    multicast_send_interface_addr.sin_family = AF_INET;
    multicast_send_interface_addr.sin_addr.s_addr = inet_addr(CA_MULTICAST_ADDR);
    multicast_send_interface_addr.sin_port = htons(CA_MULTICAST_PORT);

    // [multicast receiver]
    // 1. Create a typical UDP socket and set Non-blocking for reading
    multicast_receive_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (multicast_receive_socket < 0)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiInit, Socket error");

        return;
    }

    // 2. Allow multiple sockets to use the same port number
    ret_val = setsockopt(multicast_receive_socket, SOL_SOCKET, SO_REUSEADDR, &multiTTL,
            sizeof(multiTTL));
    if (ret_val < 0)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiInit, Failed to set REUSEADDR");
    }

    // 3. Set up the interface
    memset(&multicast_receive_interface_addr, 0, sizeof(multicast_receive_interface_addr));
    multicast_receive_interface_addr.sin_family = AF_INET;
    multicast_receive_interface_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    multicast_receive_interface_addr.sin_port = htons(CA_MULTICAST_PORT);

    // 4. Bind to the interface
    ret_val = bind(multicast_receive_socket, (struct sockaddr *) &multicast_receive_interface_addr,
            sizeof(multicast_receive_interface_addr));
    if (ret_val < 0)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiInit, Failed to bind socket");

        return;
    }

    // 5. Join the multicast group
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(CA_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    ret_val = setsockopt(multicast_receive_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
            sizeof(mreq));
    if (ret_val < 0)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiInit, Failed to join multicast group");

        return;
    }
}

void CAWiFiTerminate()
{
    OIC_LOG(DEBUG, TAG, "CAWiFiTerminate");

    close(unicast_receive_socket);
    close(multicast_receive_socket);

    shutdown(unicast_receive_socket, 2);
    shutdown(multicast_receive_socket, 2);

    CAWiFiStopUnicastServer(0);

    CAWiFiStopMulticastServer(0);

    // stop thread
    CAQueueingThreadStop(&gSendThread);
    // delete thread data
    CAQueueingThreadDestroy(&gSendThread);

    // stop thread
    CAQueueingThreadStop(&gReceiveThread);
    // delete thread data
    CAQueueingThreadDestroy(&gReceiveThread);

    u_mutex_free(unicastListenTask.threadMutex);
    u_cond_free(unicastListenTask.threadCond);

    u_mutex_free(multicastListenTask.threadMutex);
    u_cond_free(multicastListenTask.threadCond);

}

int32_t CAWiFiSendUnicastMessage(const char* address, const char* data, int lengh)
{
    // store the data at queue.
    CAThreadData_t* td = NULL;
    td = (CAThreadData_t*) OICMalloc(sizeof(CAThreadData_t));
    if (td == NULL)
    {
        return 0;
    }
    memset(td, 0, sizeof(CAThreadData_t));
    td->transmissionType = CA_UNICAST; // unicast type
    int len = strlen(address);
    td->address = (char*) OICMalloc(sizeof(char) * (len + 1));
    if (td->address != NULL)
    {
        memset(td->address, 0, len + 1);
        memcpy(td->address, address, len);
    }
    else
    {
        OIC_LOG_V(DEBUG, TAG, "Memory Full");
        OICFree(td);
        return 0;
    }

    td->data = data;

    CAQueueingThreadAddData(&gSendThread, td, sizeof(CAThreadData_t));

    return 0;
}

int32_t CAWiFiSendMulticastMessage(const char* m_address, const char* data)
{
    // store the data at queue.
    CAThreadData_t* td = NULL;
    td = (CAThreadData_t*) OICMalloc(sizeof(CAThreadData_t));
    if (td == NULL)
    {
        OICFree(data);
        return 0;
    }
    memset(td, 0, sizeof(CAThreadData_t));
    td->transmissionType = CA_MULTICAST; // multicast type
    td->address = NULL;
    td->data = data;

    CAQueueingThreadAddData(&gSendThread, td, sizeof(CAThreadData_t));

    return 0;
}

int32_t CAWiFiStartUnicastServer()
{
    OIC_LOG_V(DEBUG, TAG, "CAWiFiStartUnicastServer(%s, %d)", "0.0.0.0", CA_UNICAST_PORT);

    // check the server status
    if (unicastListenTask.status == 1)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiStartUnicastServer, already running");

        return 0;
    }

    // unicast listen thread
    CAResult_t res = u_thread_pool_add_task(gThreadPoolHandle, CAUnicastListenThread, NULL);
    if (res != CA_STATUS_OK)
    {
        OIC_LOG(DEBUG, TAG, "adding task to thread pool is error (unicast listen thread)");
        return res;
    }

    unicastListenTask.status = 1; // running

    return 0;
}

int32_t CAWiFiStartMulticastServer()
{
    OIC_LOG_V(DEBUG, TAG, "CAWiFiStartMulticastServer(%s, %d)", "0.0.0.0", CA_MULTICAST_PORT);

    // check the server status
    if (multicastListenTask.status == 1)
    {
        OIC_LOG(DEBUG, TAG, "CAWiFiStartMulticastServer, already running");

        return 0;
    }

    // multicast listen thread
    CAResult_t res = u_thread_pool_add_task(gThreadPoolHandle, CAMulticastListenThread, NULL);
    if (res != CA_STATUS_OK)
    {
        OIC_LOG(DEBUG, TAG, "adding task to thread pool is error (multicast listen thread)");
        return res;
    }

    multicastListenTask.status = 1;

    return 0;
}

int32_t CAWiFiStopUnicastServer()
{
    OIC_LOG(DEBUG, TAG, "CAWiFiStopUnicastServer");

    // mutex lock
    u_mutex_lock(unicastListenTask.threadMutex);

    // set stop flag
    unicastListenTask.isStop = TRUE;

    // notity the thread
    u_cond_signal(unicastListenTask.threadCond);

    // mutex unlock
    u_mutex_unlock(unicastListenTask.threadMutex);

    unicastListenTask.status = 0; // stopped

    return 0;
}

int32_t CAWiFiStopMulticastServer()
{
    OIC_LOG(DEBUG, TAG, "CAWiFiStopMulticastServer");

    // mutex lock
    u_mutex_lock(multicastListenTask.threadMutex);

    // set stop flag
    multicastListenTask.isStop = TRUE;

    // notity the thread
    u_cond_signal(multicastListenTask.threadCond);

    // mutex unlock
    u_mutex_unlock(multicastListenTask.threadMutex);

    multicastListenTask.status = 0; // stopped

    return 0;
}

void CAWiFiSetCallback(CAPacketReceiveCallback callback)
{
    gPacketReceiveCallback = callback;
}

void CAGetLocalAddress(char* addressBuffer)
{
    //char addressBuffer[INET_ADDRSTRLEN];
    memset(addressBuffer, 0, INET_ADDRSTRLEN);

    struct ifaddrs* ifAddrStruct = NULL;
    struct ifaddrs* ifa = NULL;
    void* tmpAddrPtr = NULL;

    getifaddrs(&ifAddrStruct);

    for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr)
        {
            continue;
        }

        if (ifa->ifa_addr->sa_family == AF_INET)
        { // check it is IP4
          // is a valid IP4 Address
            tmpAddrPtr = &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr;

            memset(addressBuffer, 0, INET_ADDRSTRLEN);
            inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);

            if (strcmp(addressBuffer, "127.0.0.1") == 0)
                continue;
        }
    }

    if (ifAddrStruct != NULL)
        freeifaddrs(ifAddrStruct);
}

int32_t CASendUnicastMessageImpl(const char* address, const char* data)
{
    OIC_LOG_V(DEBUG, TAG, "CASendUnicastMessageImpl, address: %s, data: %s", address, data);

    // [UDP Client]

    struct sockaddr_in si_other;
    int32_t slen = sizeof(si_other);

    memset((char *) &si_other, 0, sizeof(si_other));

    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(CA_UNICAST_PORT);
    if (inet_aton(address, &si_other.sin_addr) == 0)
    {
        OIC_LOG(DEBUG, TAG, "CASendUnicastMessageImpl, inet_aton, error...");
        return 0;
    }

    OIC_LOG_V(DEBUG, TAG, "CASendUnicastMessageImpl, sendto, to: %s, data: %s", address, data);
    if (sendto(unicast_receive_socket, data, strlen(data), 0, (struct sockaddr *) &si_other, slen)
            == -1)
    {
        OIC_LOG(DEBUG, TAG, "CASendUnicastMessageImpl, sendto, error...");

        return 0;
    }

    return 0;
}

int32_t CASendMulticastMessageImpl(const char* msg)
{
    OIC_LOG_V(DEBUG, TAG, "CASendMulticastMessageImpl, sendto, data: %s", msg);

    int32_t result = sendto(unicast_receive_socket, msg, strlen(msg), 0,
            (struct sockaddr *) &multicast_send_interface_addr,
            sizeof(multicast_send_interface_addr));
    if (result < 0)
    {
        OIC_LOG(DEBUG, TAG, "CASendMulticastMessageImpl, sending message error...");

        return -1;
    }

    return 0;
}
