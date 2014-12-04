/******************************************************************
 *
 * Copyright 2014 Samsung Electronics All Rights Reserved.
 *
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************/

/**
 * @file    cabtmanager.c
 * @brief   This    file provides the APIs to control Bluetooth transport
 */

#include "cabtmanager.h"
#include "cabtclient.h"
#include "cabtserver.h"
#include "cabtendpoint.h"
#include "cabtdevicelist.h"
#include "cabtutils.h"
#include "caadapterutils.h"
#include "camessagequeue.h"


typedef struct
{
    CALocalConnectivity_t *info;
    CANetworkStatus_t status;
} CABTNetworkEvent;

typedef struct
{
    void *data;
    int32_t dataLen;
    CARemoteEndpoint_t *remoteEndpoint;
} CABTMessage;

/**
 * @var gNetworkPacketReceivedCallback
 * @brief Maintains the callback to be notified on receival of network packets from other
 *          Bluetooth devices.
 */
static CANetworkPacketReceivedCallback gNetworkPacketReceivedCallback = NULL;

/**
 * @var gNetworkChangeCallback
 * @brief Maintains the callback to be notified on local bluetooth adapter status change.
 */
static CANetworkChangeCallback gNetworkChangeCallback = NULL;

/**
 * @var gBTDeviceListMutex
 * @brief Mutex to synchronize the access to Bluetooth device information list.
 */
static u_mutex gBTDeviceListMutex = NULL;

/**
 * @var gBTDeviceList
 * @brief Peer Bluetooth device information list.
 */
static BTDeviceList *gBTDeviceList = NULL;

/**
 * @var gLocalConnectivity
 * @brief Information of local Bluetooth adapter.
 */
static CALocalConnectivity_t *gLocalConnectivity = NULL;

/**
 * @var gBTThreadPool
 * @brief Reference to threadpool.
 */
static u_thread_pool_t gBTThreadPool = NULL;

/**
 * @var gSendDataQueue
 * @brief Queue to maintain data to be send to remote Bluetooth devices.
 */
static CAAdapterMessageQueue_t *gSendDataQueue = NULL;

/**
 * @var gSendDataMutex
 * @brief Mutex to synchronize access to data send queue.
 */
static u_mutex gSendDataMutex = NULL;

/**
 * @var gSendDataCond
 * @brief Condition used for notifying handler the presence of data in send queue.
 */
static u_cond gSendDataCond = NULL;

/**
 * @var gDataSendHandlerState
 * @brief Stop condition of sendhandler.
 */
static CABool_t gDataSendHandlerState = CA_FALSE;

/**
 * @fn CABTAdapterStateChangeCallback
 * @brief This callback is registered to receive bluetooth adapter state changes.
 */
static void CABTAdapterStateChangeCallback(int result, bt_adapter_state_e adapterState,
        void *userData);

/**
 * @fn CABTSocketConnectionStateCallback
 * @brief This callback is registered to receive bluetooth RFCOMM connection state changes.
 */
static void CABTSocketConnectionStateCallback(int result,
        bt_socket_connection_state_e connectionState,
        bt_socket_connection_s *connection, void *userData);

/**
 * @fn CABTDataRecvCallback
 * @brief This callback is registered to recieve data on any open RFCOMM connection.
 */
static void CABTDataRecvCallback(bt_socket_received_data_s *data, void *userData);

/**
 * @fn CABTDeviceDiscoveryCallback
 * @brief This callback is registered to recieve all bluetooth nearby devices when device
 *           scan is initiated.
 */
static void CABTDeviceDiscoveryCallback(int result,
                                        bt_adapter_device_discovery_state_e discoveryState,
                                        bt_adapter_device_discovery_info_s *discoveryInfo, void *userData);

/**
 * @fn CABTServiceSearchedCallback
 * @brief This callback is registered to recieve all the services remote bluetooth device supports
 *           when service search initiated.
 */
static void CABTServiceSearchedCallback(int result, bt_device_sdp_info_s *sdpInfo, void *userData);


/**
 * @fn CABTManagerInitializeQueues
 * @brief This function creates send and receive message queues.
 */
static CAResult_t CABTManagerInitializeQueues(void);

/**
 * @fn CABTManagerTerminateQueues
 * @brief This function releases send and receive message queues.
 */
static void CABTManagerTerminateQueues(void);

/**
 * @fn CABTManagerInitializeMutex
 * @brief This function creates mutex.
 */
static void CABTManagerInitializeMutex(void);

/**
 * @fn CABTManagerTerminateMutex
 * @brief This function frees mutex.
 */
static void CABTManagerTerminateMutex(void);

/**
 * @fn CABTManagerDataSendHandler
 * @brief This function handles message from send queue.
 */
static void CABTManagerDataSendHandler(void *context);

/**
 * @fn CABTManagerSendUnicastData
 * @brief This function send data to specified remote bluetooth device.
 */
static CAResult_t CABTManagerSendUnicastData(const char *remoteAddress, const char *serviceUUID,
        void *data, uint32_t dataLength, uint32_t *sentLength);

/**
 * @fn CABTManagerSendMulticastData
 * @brief This function send data to all bluetooth devices running OIC service.
 */
static CAResult_t CABTManagerSendMulticastData(const char *serviceUUID, void *data,
        uint32_t dataLength,
        uint32_t *sentLength);

/**
 * @fn CABTStartServiceSearch
 * @brief This function search for OIC service for remote Bluetooth device.
 */
static CAResult_t CABTStartServiceSearch(const char *remoteAddress);

/**
 * @fn CABTNotifyNetworkStauts
 * @brief This function creates notification task for network adapter status and add it to thread pool.
 */
static CAResult_t CABTNotifyNetworkStauts(CANetworkStatus_t status);

/**
 * @fn CABTOnNetworkStautsChanged
 * @brief This is task callback function for notifying network adapter status to upper layer.
 */
static void CABTOnNetworkStautsChanged(void *context);

/**
 * @fn CABTCreateNetworkEvent
 * @brief Creates instance of CABTNetworkEvent.
 */
static CABTNetworkEvent *CABTCreateNetworkEvent(CALocalConnectivity_t *connectivity,
        CANetworkStatus_t status);

/**
 * @fn CABTFreeNetworkEvent
 * @brief destroy instance of CABTNetworkEvent.
 */
static void CABTFreeNetworkEvent(CABTNetworkEvent *event);


CAResult_t CABTManagerIntialize(u_thread_pool_t threadPool)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    int err = BT_ERROR_NONE;

    //Initialize Bluetooth service
    if (BT_ERROR_NONE != (err = bt_initialize()))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Bluetooth initialization failed!, error num [%x]",
                  err);
        return CA_STATUS_FAILED;
    }

    //Set bluetooth adapter sate change callback
    if (BT_ERROR_NONE != (err = bt_adapter_set_state_changed_cb(CABTAdapterStateChangeCallback, NULL)))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG,
                  "Setting bluetooth state change callback failed!, error num [%x]", err);

        //Deinitialize the Bluetooth stack
        bt_deinitialize();
        return CA_STATUS_FAILED;
    }

    //Get Bluetooth adapter state
    bt_adapter_state_e adapterState;
    if (BT_ERROR_NONE != (err = bt_adapter_get_state(&adapterState)))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Bluetooth get state failed!, error num [%x]",
                  err);

        //Reset the adapter state change callback
        bt_adapter_unset_state_changed_cb();

        //Deinitialize the Bluetooth stack
        bt_deinitialize();
        return CA_STATUS_FAILED;
    }

    //Initialize Send/Receive data message queues
    if (CA_STATUS_OK != CABTManagerInitializeQueues())
    {
        //Reset the adapter state change callback
        bt_adapter_unset_state_changed_cb();

        //Deinitialize the Bluetooth stack
        bt_deinitialize();
        return CA_STATUS_FAILED;
    }

    //Create and initialize the mutex
    CABTManagerInitializeMutex();

    if (NULL == gBTThreadPool)
    {
        gBTThreadPool = threadPool;
    }

    if (BT_ADAPTER_DISABLED == adapterState)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Bluetooth adapter is disabled!");
        return CA_ADAPTER_NOT_ENABLED;
    }

    //Notity to upper layer
    CABTNotifyNetworkStauts(CA_INTERFACE_UP);

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

void CABTManagerTerminate(void)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    gNetworkPacketReceivedCallback = NULL;
    gNetworkChangeCallback = NULL;

    //Stop the adpater
    CABTManagerStop();

    //Unset bluetooth adapter callbacks
    bt_adapter_unset_state_changed_cb();

    //Terminate Bluetooth service
    bt_deinitialize();

    //Terminate thread pool
    gBTThreadPool = NULL;

    //Free LocalConnectivity information
    CAAdapterFreeLocalEndpoint(gLocalConnectivity);
    gLocalConnectivity = NULL;

    //Free BTDevices list
    if (gBTDeviceListMutex)
    {
        u_mutex_lock(gBTDeviceListMutex);
        CAFreeBTDeviceList(gBTDeviceList);
        gBTDeviceList = NULL;
        u_mutex_unlock(gBTDeviceListMutex);
    }

    //Free the mutex
    CABTManagerTerminateMutex();

    //Terminate Send/Receive data messages queues
    CABTManagerTerminateQueues();

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

CAResult_t CABTManagerStart(void)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    int err = BT_ERROR_NONE;
    bool isDiscoveryStarted = false;

    //Get Bluetooth adapter state
    bt_adapter_state_e adapterState;
    if (BT_ERROR_NONE != (err = bt_adapter_get_state(&adapterState)))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Bluetooth get state failed!, error num [%x]",
                  err);
        return CA_STATUS_FAILED;
    }

    if (BT_ADAPTER_DISABLED == adapterState)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Bluetooth adapter is disabled!");
        return CA_ADAPTER_NOT_ENABLED;
    }

    //Register for discovery and rfcomm socket connection callbacks
    bt_adapter_set_device_discovery_state_changed_cb(CABTDeviceDiscoveryCallback, NULL);
    bt_device_set_service_searched_cb(CABTServiceSearchedCallback, NULL);
    bt_socket_set_connection_state_changed_cb(CABTSocketConnectionStateCallback, NULL);
    bt_socket_set_data_received_cb(CABTDataRecvCallback, NULL);

    if (BT_ERROR_NONE != (err = bt_adapter_is_discovering(&isDiscoveryStarted)))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to get discovery state!, error num [%x]",
                  err);
        return CA_STATUS_FAILED;
    }

    //Start device discovery if its not started
    if (false == isDiscoveryStarted )
    {
        if (BT_ERROR_NONE != (err = bt_adapter_start_device_discovery()))
        {
            OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Device discovery failed!, error num [%x]",
                      err);
            return CA_STATUS_FAILED;
        }
    }

    //Start data send and receive handlers
    gDataSendHandlerState = CA_TRUE;
    if (CA_STATUS_OK != u_thread_pool_add_task(gBTThreadPool, CABTManagerDataSendHandler, NULL))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to start data send handler!");
        return CA_STATUS_FAILED;
    }

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

void CABTManagerStop(void)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    int err = BT_ERROR_NONE;
    bool isDiscoveryStarted = false;

    //Stop data send and receive handlers
    if (gSendDataMutex && gSendDataCond && gDataSendHandlerState)
    {
        u_mutex_lock(gSendDataMutex);
        gDataSendHandlerState = CA_FALSE;
        u_cond_signal(gSendDataCond);
        u_mutex_unlock(gSendDataMutex);
    }

    //Check discovery status
    if (BT_ERROR_NONE != (err = bt_adapter_is_discovering(&isDiscoveryStarted)))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to get discovery state!, error num [%x]",
                  err);
        return;
    }

    //stop the device discovery process
    if (true == isDiscoveryStarted)
    {
        OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Stopping the device search process");
        if (BT_ERROR_NONE != (err = bt_adapter_stop_device_discovery()))
        {
            OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to stop device discovery!, error num [%x]",
                      err);
        }
    }

    //reset bluetooth adapter callbacks
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Resetting the callbacks");
    bt_adapter_unset_device_discovery_state_changed_cb();
    bt_device_unset_service_searched_cb();
    bt_socket_unset_connection_state_changed_cb();
    bt_socket_unset_data_received_cb();

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

void CABTManagerSetPacketReceivedCallback(CANetworkPacketReceivedCallback packetReceivedCallback)
{
    gNetworkPacketReceivedCallback = packetReceivedCallback;
}

void CABTManagerSetNetworkChangeCallback(CANetworkChangeCallback networkChangeCallback)
{
    gNetworkChangeCallback = networkChangeCallback;
}

CAResult_t CABTManagerSendData(const char *remoteAddress, const char *serviceUUID,
                               void *data, uint32_t dataLength, uint32_t *sentLength)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    //Input validation
    VERIFY_NON_NULL(serviceUUID, BLUETOOTH_ADAPTER_TAG, "service UUID is null");
    VERIFY_NON_NULL(data, BLUETOOTH_ADAPTER_TAG, "Data is null");
    VERIFY_NON_NULL(sentLength, BLUETOOTH_ADAPTER_TAG, "Sent data length holder is null");

    VERIFY_NON_NULL_RET(gSendDataQueue, BLUETOOTH_ADAPTER_TAG, "Send data queue is NULL",
                        CA_STATUS_FAILED);
    VERIFY_NON_NULL_RET(gSendDataMutex, BLUETOOTH_ADAPTER_TAG, "Send data queue mutex is NULL",
                        CA_STATUS_FAILED);
    VERIFY_NON_NULL_RET(gSendDataCond, BLUETOOTH_ADAPTER_TAG, "Send data queue condition is NULL",
                        CA_STATUS_FAILED);

    //Add message to data queue
    CARemoteEndpoint_t *remoteEndpoint = CAAdapterCreateRemoteEndpoint(CA_EDR, remoteAddress,
                                         serviceUUID);
    if (NULL == remoteEndpoint)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to create remote endpoint !");
        return CA_STATUS_FAILED;
    }

    if (CA_STATUS_OK != CAAdapterEnqueueMessage(gSendDataQueue, remoteEndpoint, data, dataLength))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to add message to queue !");
        return CA_STATUS_FAILED;
    }

    CAAdapterFreeRemoteEndpoint(remoteEndpoint);
    *sentLength = dataLength;

    //Signal message handler for processing data for sending
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Signalling message send handler");
    u_mutex_lock(gSendDataMutex);
    u_cond_signal(gSendDataCond);
    u_mutex_unlock(gSendDataMutex);

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CABTManagerStartServer(const char *serviceUUID, int32_t *serverID)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    return CABTServerStart(serviceUUID, serverID);
}

CAResult_t CABTManagerStopServer(const int32_t serverID)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    return CABTServerStop(serverID);
}

CAResult_t CABTManagerGetInterface(CALocalConnectivity_t **info)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    int err = BT_ERROR_NONE;
    char *localAddress = NULL;

    //Input validation
    VERIFY_NON_NULL(info, BLUETOOTH_ADAPTER_TAG, "LocalConnectivity info is null");

    //Get the bluetooth adapter local address
    if (BT_ERROR_NONE != (err = bt_adapter_get_address(&localAddress)))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG,
                  "Getting local adapter address failed!, error num [%x]",
                  err);
        return CA_STATUS_FAILED;
    }

    //Create network info
    *info = CAAdapterCreateLocalEndpoint(CA_EDR, localAddress, NULL);
    if (NULL == *info)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to create LocalConnectivity instance!");

        OICFree(localAddress);
        return CA_MEMORY_ALLOC_FAILED;
    }

    OICFree(localAddress);

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CABTManagerReadData(void)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_NOT_SUPPORTED;
}

CAResult_t CABTManagerInitializeQueues(void)
{
    if (NULL == gSendDataQueue)
    {
        if (CA_STATUS_OK != CAAdapterInitializeMessageQueue(&gSendDataQueue))
        {
            return CA_STATUS_FAILED;
        }
    }
}

void CABTManagerTerminateQueues(void)
{
    if (gSendDataQueue)
    {
        CAAdapterTerminateMessageQueue(gSendDataQueue);
        gSendDataQueue = NULL;
    }
}

void CABTManagerInitializeMutex(void)
{
    u_mutex_init();
    if (NULL == gBTDeviceListMutex)
    {
        gBTDeviceListMutex = u_mutex_new();
    }

    if (NULL == gSendDataMutex)
    {
        gSendDataMutex = u_mutex_new();
    }

    if (NULL == gSendDataCond)
    {
        gSendDataCond = u_cond_new();
    }
}

void CABTManagerTerminateMutex(void)
{
    if (gBTDeviceListMutex)
    {
        u_mutex_free(gBTDeviceListMutex);
        gBTDeviceListMutex = NULL;
    }

    if (gSendDataMutex)
    {
        u_mutex_free(gSendDataMutex);
        gSendDataMutex = NULL;
    }

    if (gSendDataCond)
    {
        u_cond_free(gSendDataCond);
        gSendDataCond = NULL;
    }
}

void CABTManagerDataSendHandler(void *context)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    u_mutex_lock(gSendDataMutex);
    while (gDataSendHandlerState)
    {
        CAAdapterMessage_t *message = NULL;
        const char *remoteAddress = NULL;
        const char *serviceUUID = NULL;
        uint32_t sentLength = 0;

        //Extract the message from queue and send to remote bluetooth device
        while (CA_STATUS_OK == CAAdapterDequeueMessage(gSendDataQueue, &message))
        {
            remoteAddress = message->remoteEndpoint->addressInfo.BT.btMacAddress;
            serviceUUID = message->remoteEndpoint->resourceUri;
            if (strlen(remoteAddress)) //Unicast data
            {
                if (CA_STATUS_OK != CABTManagerSendUnicastData(remoteAddress, serviceUUID,
                        message->data, message->dataLen, &sentLength))
                {
                    OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to send unicast data !");
                }
            }
            else //Multicast data
            {
                if (CA_STATUS_OK != CABTManagerSendMulticastData(serviceUUID, message->data,
                        message->dataLen, &sentLength))
                {
                    OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to send multicast data !");
                }
            }

            //Free message
            CAAdapterFreeMessage(message);
        }

        //Wait for the data to be send
        OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Waitiing for data");
        u_cond_wait(gSendDataCond, gSendDataMutex);
        OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Got the signal that data is pending");

        if (CA_FALSE == gDataSendHandlerState)
        {
            break;
        }
    }

    u_mutex_unlock(gSendDataMutex);
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

CAResult_t CABTManagerSendUnicastData(const char *remoteAddress, const char *serviceUUID,
                                      void *data, uint32_t dataLength, uint32_t *sentLength)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    BTDevice *device = NULL;

    //Input validation
    VERIFY_NON_NULL(remoteAddress, BLUETOOTH_ADAPTER_TAG, "Remote address is null");
    VERIFY_NON_NULL(serviceUUID, BLUETOOTH_ADAPTER_TAG, "service UUID is null");
    VERIFY_NON_NULL(data, BLUETOOTH_ADAPTER_TAG, "Data is null");
    VERIFY_NON_NULL(sentLength, BLUETOOTH_ADAPTER_TAG, "Sent data length holder is null");

    if (0 >= dataLength)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Invalid input: Negative data length!");
        return CA_STATUS_INVALID_PARAM;
    }

    //Check the connection existence with remote device
    u_mutex_lock(gBTDeviceListMutex);
    if (CA_STATUS_OK != CAGetBTDevice(gBTDeviceList, remoteAddress, &device))
    {
        //Create new device and add to list
        if (CA_STATUS_OK != CACreateAndAddToDeviceList(&gBTDeviceList, remoteAddress,
                OIC_BT_SERVICE_ID, &device))
        {
            OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed create device and add to list!");

            u_mutex_unlock(gBTDeviceListMutex);
            return CA_STATUS_FAILED;
        }

        //Start the OIC service search newly created device
        if (CA_STATUS_OK != CABTStartServiceSearch(remoteAddress))
        {
            OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to initiate service search!");

            //Remove device from list
            CARemoveBTDeviceFromList(&gBTDeviceList, remoteAddress);

            u_mutex_unlock(gBTDeviceListMutex);
            return CA_STATUS_FAILED;
        }
    }
    u_mutex_unlock(gBTDeviceListMutex);

    if (-1 == device->socketFD)
    {
        //Adding to pending list
        if (CA_STATUS_OK != CAAddDataToDevicePendingList(&device->pendingDataList, data,
                dataLength))
        {
            OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to add data to pending list!");

            //Remove device from list
            CARemoveBTDeviceFromList(&gBTDeviceList, remoteAddress);
            return CA_STATUS_FAILED;
        }

        //Make a rfcomm connection with remote BT Device
        if (1 == device->serviceSearched &&
            CA_STATUS_OK != CABTClientConnect(remoteAddress, serviceUUID))
        {
            OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to make RFCOMM connection!");

            //Remove device from list
            CARemoveBTDeviceFromList(&gBTDeviceList, remoteAddress);
            return CA_STATUS_FAILED;
        }
        *sentLength = dataLength;
    }
    else
    {
        if (CA_STATUS_OK != CABTSendData(device->socketFD, data, dataLength, sentLength))
        {
            OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to send data!");
            return CA_STATUS_FAILED;
        }
    }

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CABTManagerSendMulticastData(const char *serviceUUID, void *data, uint32_t dataLength,
                                        uint32_t *sentLength)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    BTDeviceList *cur = NULL;

    //Input validation
    VERIFY_NON_NULL(serviceUUID, BLUETOOTH_ADAPTER_TAG, "service UUID is null");
    VERIFY_NON_NULL(data, BLUETOOTH_ADAPTER_TAG, "Data is null");
    VERIFY_NON_NULL(sentLength, BLUETOOTH_ADAPTER_TAG, "Sent data length holder is null");

    if (0 >= dataLength)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Invalid input: Negative data length!");
        return CA_STATUS_INVALID_PARAM;
    }

    *sentLength = dataLength;

    //Send the packet to all OIC devices
    u_mutex_lock(gBTDeviceListMutex);
    cur = gBTDeviceList;
    while (cur != NULL)
    {
        BTDevice *device = cur->device;
        cur = cur->next;

        if (-1 == device->socketFD)
        {
            //Check if the device service search is finished
            if (0 == device->serviceSearched)
            {
                OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Device services are still unknown!");
                continue;
            }

            //Adding to pendding list
            if (CA_STATUS_OK != CAAddDataToDevicePendingList(&device->pendingDataList, data,
                    dataLength))
            {
                OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to add data to pending list !");
                continue;
            }

            //Make a rfcomm connection with remote BT Device
            if (CA_STATUS_OK != CABTClientConnect(device->remoteAddress, device->serviceUUID))
            {
                OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to make RFCOMM connection !");

                //Remove the data which added to pending list
                CARemoveDataFromDevicePendingList(&device->pendingDataList);
                continue;
            }
        }
        else
        {
            if (CA_STATUS_OK != CABTSendData(device->socketFD, data, dataLength, sentLength))
            {
                OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to send data to [%s] !",
                          device->remoteAddress);
            }
        }
    }
    u_mutex_unlock(gBTDeviceListMutex);

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CABTStartServiceSearch(const char *remoteAddress)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    int err = BT_ERROR_NONE;

    //Input validation
    VERIFY_NON_NULL(remoteAddress, BLUETOOTH_ADAPTER_TAG, "Remote address is null");
    if (0 == strlen(remoteAddress))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Remote address is empty!");
        return CA_STATUS_INVALID_PARAM;
    }

    //Start searching for OIC service
    if (BT_ERROR_NONE != (err = bt_device_start_service_search(remoteAddress)))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Get bonded device failed!, error num [%x]",
                  err);
        return CA_STATUS_FAILED;
    }

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

void CABTAdapterStateChangeCallback(int result, bt_adapter_state_e adapterState, void *userData)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    if (BT_ADAPTER_ENABLED == adapterState)
    {
        //Notity to upper layer
        CABTNotifyNetworkStauts(CA_INTERFACE_UP);
    }
    else if (BT_ADAPTER_DISABLED == adapterState)
    {
        //Notity to upper layer
        CABTNotifyNetworkStauts(CA_INTERFACE_DOWN);
    }

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

void CABTSocketConnectionStateCallback(int result, bt_socket_connection_state_e connectionState,
                                       bt_socket_connection_s *connection, void *userData)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    BTDevice *device = NULL;

    if (BT_ERROR_NONE != result || NULL == connection)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Invalid connection state!, error num [%x]",
                  result);
        return;
    }

    switch (connectionState)
    {
        case BT_SOCKET_CONNECTED:
            {
                u_mutex_lock(gBTDeviceListMutex);
                if (CA_STATUS_OK != CAGetBTDevice(gBTDeviceList, connection->remote_address,
                                                  &device))
                {
                    //Create the deviceinfo and add to list
                    if (CA_STATUS_OK != CACreateAndAddToDeviceList(&gBTDeviceList,
                            connection->remote_address, OIC_BT_SERVICE_ID, &device))
                    {
                        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed add device to list!");
                        u_mutex_unlock(gBTDeviceListMutex);
                        return;
                    }

                    device->socketFD = connection->socket_fd;
                    u_mutex_unlock(gBTDeviceListMutex);
                    return;
                }

                device->socketFD = connection->socket_fd;
                while (device->pendingDataList)
                {
                    uint32_t sentData = 0;
                    BTData *btData = device->pendingDataList->data;
                    if (CA_STATUS_OK != CABTSendData(device->socketFD, btData->data,
                                                     btData->dataLength, &sentData))
                    {
                        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to send pending data [%s]",
                                  device->remoteAddress);

                        //Remove all the data from pending list
                        CARemoveAllDataFromDevicePendingList(&device->pendingDataList);
                        break;
                    }

                    //Remove the data which send from pending list
                    CARemoveDataFromDevicePendingList(&device->pendingDataList);
                }
                u_mutex_unlock(gBTDeviceListMutex);
            }
            break;

        case BT_SOCKET_DISCONNECTED:
            {
                u_mutex_lock(gBTDeviceListMutex);
                CARemoveBTDeviceFromList(&gBTDeviceList, connection->remote_address);
                u_mutex_unlock(gBTDeviceListMutex);
            }
            break;
    }

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

void CABTDataRecvCallback(bt_socket_received_data_s *data, void *userData)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    BTDevice *device = NULL;

    if (NULL == data || 0 >= data->data_size)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Data is null!");
        return;
    }

    if (NULL == gNetworkPacketReceivedCallback)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Callback is not registered!");
        return;
    }

    //Get BT device from list
    u_mutex_lock(gBTDeviceListMutex);
    if (CA_STATUS_OK != CAGetBTDeviceBySocketId(gBTDeviceList, data->socket_fd, &device))
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Could not find the device!");

        u_mutex_unlock(gBTDeviceListMutex);
        return;
    }
    u_mutex_unlock(gBTDeviceListMutex);

    //Create RemoteEndPoint
    CARemoteEndpoint_t *remoteEndpoint = NULL;
    remoteEndpoint = CAAdapterCreateRemoteEndpoint(CA_EDR, device->remoteAddress,
                     OIC_BT_SERVICE_ID);
    if (NULL == remoteEndpoint)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to crate remote endpoint!");
        return;
    }

    void *copyData = OICMalloc(data->data_size);
    if (NULL == copyData)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed allocate memory!");
        CAAdapterFreeRemoteEndpoint(remoteEndpoint);
        return;
    }
    memcpy(copyData, data->data, data->data_size);

    gNetworkPacketReceivedCallback(remoteEndpoint, copyData, (uint32_t)data->data_size);

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

void CABTDeviceDiscoveryCallback(int result, bt_adapter_device_discovery_state_e discoveryState,
                                 bt_adapter_device_discovery_info_s *discoveryInfo, void *userData)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    BTDevice *device = NULL;

    if (BT_ERROR_NONE != result)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Received bad state!, error num [%x]",
                  result);
        return;
    }

    switch (discoveryState)
    {
        case BT_ADAPTER_DEVICE_DISCOVERY_STARTED:
            {
                OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Discovery started!");
            }
            break;

        case BT_ADAPTER_DEVICE_DISCOVERY_FINISHED:
            {
                OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Discovery finished!");
            }
            break;

        case BT_ADAPTER_DEVICE_DISCOVERY_FOUND:
            {
                OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Device discovered [%s]!",
                          discoveryInfo->remote_name);
                if (CA_TRUE == CABTIsServiceSupported((const char **)discoveryInfo->service_uuid,
                                                      discoveryInfo->service_count,
                                                      OIC_BT_SERVICE_ID))
                {
                    //Check if the deivce is already in the list
                    u_mutex_lock(gBTDeviceListMutex);
                    if (CA_STATUS_OK == CAGetBTDevice(gBTDeviceList, discoveryInfo->remote_address,
                                                      &device))
                    {
                        device->serviceSearched = 1;
                        u_mutex_unlock(gBTDeviceListMutex);
                        return;
                    }

                    //Create the deviceinfo and add to list
                    if (CA_STATUS_OK != CACreateAndAddToDeviceList(&gBTDeviceList,
                            discoveryInfo->remote_address, OIC_BT_SERVICE_ID, &device))
                    {
                        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to add device to list!");
                        u_mutex_unlock(gBTDeviceListMutex);
                        return;
                    }

                    device->serviceSearched = 1;
                    u_mutex_unlock(gBTDeviceListMutex);
                }
                else
                {
                    OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Device does not support OIC service!");
                }
            }
            break;
    }

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

void CABTServiceSearchedCallback(int result, bt_device_sdp_info_s *sdpInfo, void *userData)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    if (NULL == sdpInfo)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "SDP info is null!");
        return;
    }

    u_mutex_lock(gBTDeviceListMutex);

    BTDevice *device = NULL;
    if (CA_STATUS_OK == CAGetBTDevice(gBTDeviceList, sdpInfo->remote_address, &device)
        && NULL != device)
    {
        if (1 == device->serviceSearched)
        {
            OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Service is already searched for this device!");
            u_mutex_unlock(gBTDeviceListMutex);
            return;
        }

        if (CA_TRUE == CABTIsServiceSupported((const char **)sdpInfo->service_uuid,
                                              sdpInfo->service_count, OIC_BT_SERVICE_ID))
        {
            device->serviceSearched = 1;
            if (CA_STATUS_OK != CABTClientConnect(sdpInfo->remote_address, OIC_BT_SERVICE_ID))
            {
                OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to make rfcomm connection!");

                //Remove the device from device list
                CARemoveBTDeviceFromList(&gBTDeviceList, sdpInfo->remote_address);
            }
        }
        else
        {
            OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "Device does not contain OIC service!");

            //Remove device from list as it does not support OIC service
            CARemoveBTDeviceFromList(&gBTDeviceList, sdpInfo->remote_address);
        }
    }

    u_mutex_unlock(gBTDeviceListMutex);

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

CAResult_t CABTNotifyNetworkStauts(CANetworkStatus_t status)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    //Create localconnectivity
    if (NULL == gLocalConnectivity)
    {
        CABTManagerGetInterface(&gLocalConnectivity);
    }

    //Notity to upper layer
    if (gNetworkChangeCallback && gLocalConnectivity && gBTThreadPool)
    {
        //Add notification task to thread pool
        CABTNetworkEvent *event = CABTCreateNetworkEvent(gLocalConnectivity, status);
        if (NULL != event)
        {
            if (CA_STATUS_OK != u_thread_pool_add_task(gBTThreadPool, CABTOnNetworkStautsChanged,
                    event))
            {
                OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to create threadpool!");
                return CA_STATUS_FAILED;
            }
        }
    }

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
    return CA_STATUS_OK;
}

void CABTOnNetworkStautsChanged(void *context)
{
    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "IN");

    if (NULL == context)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "context is NULL!");
        return;
    }

    CABTNetworkEvent *networkEvent = (CABTNetworkEvent *) context;

    //Notity to upper layer
    if (gNetworkChangeCallback)
    {
        gNetworkChangeCallback(networkEvent->info, networkEvent->status);
    }

    //Free the created Network event
    CABTFreeNetworkEvent(networkEvent);

    OIC_LOG_V(DEBUG, BLUETOOTH_ADAPTER_TAG, "OUT");
}

CABTNetworkEvent *CABTCreateNetworkEvent(CALocalConnectivity_t *connectivity,
        CANetworkStatus_t status)
{
    VERIFY_NON_NULL_RET(connectivity, BLUETOOTH_ADAPTER_TAG, "connectivity is NULL", NULL);

    //Create CABTNetworkEvent
    CABTNetworkEvent *event = (CABTNetworkEvent *) OICMalloc(sizeof(CABTNetworkEvent));
    if (NULL == event)
    {
        OIC_LOG_V(ERROR, BLUETOOTH_ADAPTER_TAG, "Failed to allocate memory to network event!");
        return NULL;
    }

    //Create duplicate of Local connectivity
    event->info = CAAdapterCopyLocalEndpoint(connectivity);
    event->status = status;
    return event;
}

void CABTFreeNetworkEvent(CABTNetworkEvent *event)
{
    if (event)
    {
        if (event->info)
        {
            CAAdapterFreeLocalEndpoint(event->info);
        }

        OICFree(event);
    }
}


