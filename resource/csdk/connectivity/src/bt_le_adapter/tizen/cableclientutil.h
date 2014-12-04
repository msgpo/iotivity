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
#ifndef _BLE_CLIENT_UTIL
#define _BLE_CLIENT_UTIL

#include <bluetooth.h>

#include "cacommon.h"
#include "logger.h"


/**
 * @struct BLEServiceInfo
 * @brief Info regarding the GATTServer
 *
 * This structure holds the infomation about the GATTServer
*  in the service and the characteristic level
 */
typedef struct
{
    bt_gatt_attribute_h service_clone;         /**< gatt_attribute handler for the OIC service. */
    bt_gatt_attribute_h
    read_char;               /**< gatt_attribute handler for the OIC read characteristic. */
    bt_gatt_attribute_h
    write_char;              /**< gatt_attribute handler for the OIC write characteristic. */
    char *bdAddress;                                /**< BD address where OIC service is running. */
} BLEServiceInfo;

/**
 * @struct BLEServiceList
 * @brief List of the BLEServiceInfo structures.
 *
 * A list of BLEServiceInfo and gives the info about all the
 * the registered services from the client side.
 */
typedef struct _BLEServiceList
{
    BLEServiceInfo *serviceInfo;                    /**< BLEServiceInfo strucutre from an OIC Server */
    struct _BLEServiceList *next;                    /**< next pointer */
} BLEServiceList;

/**
 * @ENUM CHAR_TYPE
 * @brief different characteristics types.
 *
 *  This ENUM provides information of different characteristics
 *  which will be added to OIC service.
 */
typedef enum
{
    WRITE_CHAR,                                  /**< write_char This will be used to get the unicast response */
    READ_CHAR,                                    /**< read_char This will be used update value to OIC server */
    NOTIFY_CHAR                                  /**< reserved char for the time being. */
} CHAR_TYPE;

/**
 * @ENUM TRANSFER_TYPE
 * @brief Provide info about different mode of data transfer
 *
 *  This enum is used to differentiate between unicast and multicast data transfer.
 */
typedef enum
{
    MULTICAST,                                     /**< When this enum is selected, data will be updated to all OIC servers. */
    UNICAST                                          /**< When this enum is selected, data will be updated to desired OIC Server. */
} TRANSFER_TYPE;

typedef struct gattCharDescriptor
{
    bt_gatt_attribute_h descriptor;
    bt_gatt_attribute_h characteristic;
    int total;
} stGattCharDescriptor_t;

/**
* @fn  CAIncrementRegisteredServiceCount
* @brief  Used to increment the registered service count.
*
* @return  void.
*
*/
void CAIncrementRegisteredServiceCount();

/**
* @fn  CADecrementRegisteredServiceCount
* @brief  Used to decrement the registered service count.
*
* @return  void.
*
*/
void CADecrementRegisteredServiceCount();

/**
* @fn  CAResetRegisteredServiceCount
* @brief  Used to reset the registered service count.
*
* @return  void.
*
*/
void CAResetRegisteredServiceCount();

/**
* @fn  CAGetRegisteredServiceCount
* @brief  Used to get the total  registered service count.
*
* @return  total registered service count.
*
*/
int32_t  CAGetRegisteredServiceCount();

/**
* @fn  CACreateBLEServiceInfo
* @brief  Used to create BLEServiceInfo structure with server handler and BD address will be created.
*
* @param[in] bdAddress - BD address of the device where GATTServer is running.
* @param[in] service - service attribute handler.
* @param[in] bleServiceInfo - Pointer where serviceInfo structure needs to be stored.
*                                          Memory will be allocated here and needs to be cleared by user.
*
* @return  0 on success otherwise a positive error value.
* @retval  CA_STATUS_OK  Successful
* @retval  CA_STATUS_INVALID_PARAM  Invalid input argumets
* @retval  CA_STATUS_FAILED Operation failed
*
*/
CAResult_t CACreateBLEServiceInfo(const char *bdAddress, bt_gatt_attribute_h service,
                                  BLEServiceInfo **bleServiceInfo);

/**
* @fn  CAAppendBLECharInfo
* @brief  Used to append the characteristic info to the already created serviceInfo structure.
*
* @param[in] characteristic  charecteristic attribute handler.
* @param[in] type specifies whether its READ_CHAR or WRITE_CHAR
* @param[in] bleServiceInfo Pointer where serviceInfo structure needs to be appended with char info.
*
* @return  0 on success otherwise a positive error value.
* @retval  CA_STATUS_OK  Successful
* @retval  CA_STATUS_INVALID_PARAM  Invalid input argumets
* @retval  CA_STATUS_FAILED Operation failed
*
*/
CAResult_t CAAppendBLECharInfo(bt_gatt_attribute_h characteristic, CHAR_TYPE type,
                               BLEServiceInfo *bleServiceInfo);

/**
* @fn  CAAddBLEServiceInfoToList
* @brief  Used to add the ServiceInfo structure to the Service List.
*
* @param[in] serviceList Pointer to the ble service list which holds the info of list of service registered from client.
* @param[in] bleServiceInfo Pointer where serviceInfo structure needs to be appended with char info.
*
* @return  0 on success otherwise a positive error value.
* @retval  CA_STATUS_OK  Successful
* @retval  CA_STATUS_INVALID_PARAM  Invalid input argumets
* @retval  CA_STATUS_FAILED Operation failed
*
*/
CAResult_t CAAddBLEServiceInfoToList(BLEServiceList **serviceList, BLEServiceInfo *bleServiceInfo);

/**
* @fn  CARemoveBLEServiceInfoToList
* @brief  Used to remove the ServiceInfo structure from the Service List.
*
* @param[in] serviceList Pointer to the ble service list which holds the info of list of service registered from client.
* @param[in] bleServiceInfo Pointer where serviceInfo structure needs to be appended with char info.
* @param[in] bdAddress BD address of the device where GATTServer is disconnected.
*
* @return  0 on success otherwise a positive error value.
* @retval  CA_STATUS_OK  Successful
* @retval  CA_STATUS_INVALID_PARAM  Invalid input argumets
* @retval  CA_STATUS_FAILED Operation failed
*
*/
CAResult_t CARemoveBLEServiceInfoToList(BLEServiceList **serviceList,
                                        BLEServiceInfo *bleServiceInfo,
                                        const char *bdAddress);

/**
* @fn  CAGetBLEServiceInfo
* @brief  Used to get the serviceInfo from the list.
*
* @param[in] serviceList Pointer to the ble service list which holds the info of list of service registered from client.
* @param[in] bdAddress BD address of the device where GATTServer information is required.
* @param[out] bleServiceInfo Pointer where serviceInfo structure needs to provide the service and char info.
*
* @return  0 on success otherwise a positive error value.
* @retval  CA_STATUS_OK  Successful
* @retval  CA_STATUS_INVALID_PARAM  Invalid input argumets
* @retval  CA_STATUS_FAILED Operation failed
*
*/
CAResult_t CAGetBLEServiceInfo(BLEServiceList *serviceList, const char *bdAddress,
                               BLEServiceInfo **bleServiceInfo);

/**
* @fn  CAGetBLEServiceInfoByPosition
* @brief  Used to get the serviceInfo from the list by position.
*
* @param[in] serviceList Pointer to the ble service list which holds the info of list of service registered from client.
* @param[in] position The service information of particular position in the list.
* @param[out] bleServiceInfo Pointer where serviceInfo structure needs to provide the service and char info.
*
* @return  0 on success otherwise a positive error value.
* @retval  CA_STATUS_OK  Successful
* @retval  CA_STATUS_INVALID_PARAM  Invalid input argumets
* @retval  CA_STATUS_FAILED Operation failed
*
*/
CAResult_t CAGetBLEServiceInfoByPosition(BLEServiceList *serviceList, int32_t position,
        BLEServiceInfo **bleServiceInfo);

/**
* @fn  CAFreeBLEServiceList
* @brief  Used to get clear ble service list
*
* @param[in] serviceList Pointer to the ble service list which holds the info of list of service registered from client.
*
* @return  void
*
*/
void CAFreeBLEServiceList(BLEServiceList *serviceList);

/**
* @fn  CAFreeBLEServiceInfo
* @brief  Used to get remove particular ble service info from list
*
* @param[in] serviceinfo Pointer to the structure which needs to be cleared.
*
* @return  void
*
*/
void CAFreeBLEServiceInfo(BLEServiceInfo *bleServiceInfo);

/**
* @fn  CAVerifyOICService
* @brief  Used to check whether found handle is OIC service handle or not.
*
* @param[in] serviceHandle - Discovered service handle(unique identifier for service)
*
* @return  0 on success otherwise a positive error value.
* @retval  CA_STATUS_OK  Successful
* @retval  CA_STATUS_INVALID_PARAM  Invalid input argumets
* @retval  CA_STATUS_FAILED Operation failed
*
*/
CAResult_t CAVerifyOICService(bt_gatt_attribute_h serviceHandle);

#endif //#ifndef _BLE_CLIENT_UTIL
