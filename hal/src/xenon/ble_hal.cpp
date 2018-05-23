/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "ble_hal.h"

#include "nrf_ble_gatt.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "ble.h"

#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"

#include "logging.h"

LOG_SOURCE_CATEGORY("hal.ble")

namespace {

// Advertising interval in 0.625 ms units
const auto FAST_ADVERT_INTERVAL = MSEC_TO_UNITS(40, UNIT_0_625_MS); // Fast mode
const auto SLOW_ADVERT_INTERVAL = MSEC_TO_UNITS(1000, UNIT_0_625_MS); // Slow mode

// Advertising duration in 10 ms units (fast mode)
const auto FAST_ADVERT_TIMEOUT = MSEC_TO_UNITS(180000, UNIT_10_MS); // Fast mode
const auto SLOW_ADVERT_TIMEOUT = 0; // Advertise indefinitely (slow mode)

// Size of the ATT opcode field in bytes
const auto ATT_OPCODE_SIZE = 1;

// Size of the ATT handle field in bytes
const auto ATT_HANDLE_SIZE = 2;

// Maximum size of an attribute's value in bytes
const auto MAX_ATTR_VALUE_SIZE = NRF_SDH_BLE_GATT_MAX_MTU_SIZE - ATT_OPCODE_SIZE - ATT_HANDLE_SIZE;

// Minimum acceptable connection interval in 1.25 ms units
const auto MIN_CONN_INTERVAL = MSEC_TO_UNITS(20, UNIT_1_25_MS);

// Maximum acceptable connection interval in 1.25 ms units
const auto MAX_CONN_INTERVAL = MSEC_TO_UNITS(75, UNIT_1_25_MS);

// Connection supervisory timeout in 10 ms units
const auto CONN_SUP_TIMEOUT = MSEC_TO_UNITS(4000, UNIT_10_MS);

// Slave latency
const auto SLAVE_LATENCY = 0;

// Application's BLE observer priority
const auto BLE_OBSERVER_PRIO = 3;

// An application-specific tag identifying the SoftDevice BLE configuration (can't be 0)
const auto CONN_CFG_TAG = 1;

// Advertising module instance
BLE_ADVERTISING_DEF(g_advert);

// GATT module instance
NRF_BLE_GATT_DEF(g_gatt);

struct Char {
    ble_uuid_t uuid;
    ble_gatts_char_handles_t handles;
};

struct Service {
    ble_uuid_t uuid;
    uint16_t handle;
    Char chars[BLE_MAX_CHAR_COUNT];
    size_t charCount;
};

struct Profile {
    Service services[BLE_MAX_SERVICE_COUNT];
    size_t serviceCount;
    ble_event_callback callback;
    void* userData;
};

// Current profile
Profile g_profile = {};

const Char* findChar(uint16_t handle) {
    for (size_t i = 0; i < g_profile.serviceCount; ++i) {
        const Service& svc = g_profile.services[i];
        for (size_t j = 0; j < svc.charCount; ++j) {
            const Char& chr = svc.chars[j];
            if (chr.handles.value_handle == handle || chr.handles.cccd_handle == handle) {
                return &chr;
            }
        }
    }
    return nullptr;
}

void processBleEvent(const ble_evt_t* event, void* data) {
    switch (event->header.evt_id) {
    case BLE_GAP_EVT_CONNECTED: {
        LOG(TRACE, "Connected");
        ble_connected_event_data d = {};
        d.conn_handle = event->evt.gap_evt.conn_handle;
        g_profile.callback(BLE_EVENT_CONNECTED, &d, g_profile.userData);
        break;
    }
    case BLE_GAP_EVT_DISCONNECTED: {
        LOG(TRACE, "Disconnected");
        ble_disconnected_event_data d = {};
        d.conn_handle = event->evt.gap_evt.conn_handle;
        g_profile.callback(BLE_EVENT_DISCONNECTED, &d, g_profile.userData);
        break;
    }
    case BLE_GAP_EVT_SEC_PARAMS_REQUEST: {
        // Pairing is not supported
        const uint32_t ret = sd_ble_gap_sec_params_reply(event->evt.gap_evt.conn_handle,
                BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, nullptr, nullptr);
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gap_sec_params_reply() failed: %u", (unsigned)ret);
        }
        break;
    }
    case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
        LOG(TRACE, "PHY update request");
        ble_gap_phys_t phys = {};
        phys.rx_phys = BLE_GAP_PHY_AUTO;
        phys.tx_phys = BLE_GAP_PHY_AUTO;
        const uint32_t ret = sd_ble_gap_phy_update(event->evt.gap_evt.conn_handle, &phys);
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gap_phy_update() failed: %u", (unsigned)ret);
        }
        break;
    }
    case BLE_GATTS_EVT_WRITE: {
        const ble_gatts_evt_write_t& writeParam = event->evt.gatts_evt.params.write;
        const auto chr = findChar(writeParam.handle);
        if (chr) {
            if (writeParam.handle == chr->handles.value_handle) {
                ble_data_received_event_data d = {};
                d.conn_handle = event->evt.gatts_evt.conn_handle;
                d.char_handle = chr->handles.value_handle;
                d.data = (const char*)writeParam.data;
                d.size = writeParam.len;
                g_profile.callback(BLE_EVENT_DATA_RECEIVED, &d, g_profile.userData);
            } else if (writeParam.handle == chr->handles.cccd_handle && writeParam.len == 2) {
                LOG(TRACE, "CCCD changed");
                ble_char_param_changed_event_data d = {};
                d.conn_handle = event->evt.gatts_evt.conn_handle;
                d.char_handle = chr->handles.value_handle;
                g_profile.callback(BLE_EVENT_CHAR_PARAM_CHANGED, &d, g_profile.userData);
            }
        }
        break;
    }
    case BLE_GATTS_EVT_HVN_TX_COMPLETE: {
        ble_data_sent_event_data d = {};
        d.conn_handle = event->evt.gatts_evt.conn_handle;
        g_profile.callback(BLE_EVENT_DATA_SENT, &d, g_profile.userData);
        break;
    }
    case BLE_GATTS_EVT_SYS_ATTR_MISSING: {
        // No persistent system attributes
        const uint32_t ret = sd_ble_gatts_sys_attr_set(event->evt.gatts_evt.conn_handle, nullptr, 0, 0);
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gatts_sys_attr_set() failed: %u", (unsigned)ret);
        }
        break;
    }
    case BLE_GATTS_EVT_TIMEOUT: {
        // Disconnect on GATT server timeout
        const uint32_t ret = sd_ble_gap_disconnect(event->evt.gatts_evt.conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gap_disconnect() failed: %u", (unsigned)ret);
        }
        break;
    }
    case BLE_GATTC_EVT_TIMEOUT: {
        // Disconnect on GATT client timeout
        const uint32_t ret = sd_ble_gap_disconnect(event->evt.gattc_evt.conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gap_disconnect() failed: %u", (unsigned)ret);
        }
        break;
    }
    default:
        LOG_DEBUG(TRACE, "Unhandled BLE event: %u", (unsigned)event->header.evt_id);
        break;
    }
}

void processAdvertEvent(ble_adv_evt_t event) {
    switch (event) {
    case BLE_ADV_EVT_FAST:
        LOG(TRACE, "Fast advertising mode started");
        g_profile.callback(BLE_EVENT_ADVERT_STARTED, nullptr, g_profile.userData);
        break;
    case BLE_ADV_EVT_SLOW:
        LOG(TRACE, "Slow advertising mode started");
        break;
    case BLE_ADV_EVT_IDLE:
        LOG(TRACE, "Advertising stopped");
        g_profile.callback(BLE_EVENT_ADVERT_STOPPED, nullptr, g_profile.userData);
        break;
    default:
        LOG_DEBUG(TRACE, "Unhandled advertising event: %u", (unsigned)event);
        break;
    }
}

void processGattEvent(nrf_ble_gatt_t* gatt, const nrf_ble_gatt_evt_t* event) {
    if (event->evt_id == NRF_BLE_GATT_EVT_ATT_MTU_UPDATED) {
        LOG(TRACE, "MTU updated: %u", (unsigned)event->params.att_mtu_effective);
        ble_conn_param_changed_event_data d = {};
        d.conn_handle = event->conn_handle;
        g_profile.callback(BLE_EVENT_CONN_PARAM_CHANGED, &d, g_profile.userData);
    }
}

// FIXME: Move to appropriate place
void processSocEvent(uint32_t sys_evt, void *p_context) {
    // PlatformSoftdeviceSocEvtHandler(sys_evt);
}

int halError(uint32_t error) {
    switch (error) {
    case NRF_ERROR_NO_MEM:
        return BLE_ERROR_NO_MEMORY;
    default:
        return BLE_ERROR_UNKNOWN;
    }
}

int initAdvert() {
    // Service UUIDs
    ble_uuid_t svcUuids[BLE_MAX_SERVICE_COUNT] = {};
    for (size_t i = 0; i < g_profile.serviceCount; ++i) {
        svcUuids[i] = g_profile.services[i].uuid;
    }
    // Initialize the advertising module
    ble_advertising_init_t init = {};
    init.advdata.name_type = BLE_ADVDATA_FULL_NAME;
    init.advdata.include_appearance = false;
    init.advdata.flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    init.srdata.uuids_complete.uuid_cnt = g_profile.serviceCount;
    init.srdata.uuids_complete.p_uuids = svcUuids;
    init.config.ble_adv_fast_enabled = true;
    init.config.ble_adv_fast_interval = FAST_ADVERT_INTERVAL;
    init.config.ble_adv_fast_timeout = FAST_ADVERT_TIMEOUT;
    init.config.ble_adv_slow_enabled = true;
    init.config.ble_adv_slow_interval = SLOW_ADVERT_INTERVAL;
    init.config.ble_adv_slow_timeout = SLOW_ADVERT_TIMEOUT;
    init.evt_handler = processAdvertEvent;
    const uint32_t ret = ble_advertising_init(&g_advert, &init);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "ble_advertising_init() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    ble_advertising_conn_cfg_tag_set(&g_advert, CONN_CFG_TAG);
    return 0;
}

int initTxChar(uint16_t serviceHandle, ble_char* halChar, Char* chr) {
    // Characteristic UUID
    chr->uuid.type = halChar->uuid.type;
    chr->uuid.uuid = halChar->uuid.uuid;
    // CCCD metadata (Client Characteristic Configuration Descriptor)
    ble_gatts_attr_md_t cccdMd = {};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccdMd.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccdMd.write_perm);
    cccdMd.vloc = BLE_GATTS_VLOC_STACK;
    // Characteristic metadata
    ble_gatts_char_md_t charMd = {};
    charMd.char_props.notify = 1;
    charMd.p_cccd_md = &cccdMd;
    // Value attribute metadata
    ble_gatts_attr_md_t attrMd = {};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attrMd.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attrMd.write_perm);
    attrMd.vloc = BLE_GATTS_VLOC_STACK;
    attrMd.rd_auth = 0;
    attrMd.wr_auth = 0;
    attrMd.vlen = 1;
    // Value attribute
    ble_gatts_attr_t attr = {};
    attr.p_uuid = &chr->uuid;
    attr.p_attr_md = &attrMd;
    attr.init_len = 1;
    attr.init_offs = 0;
    attr.max_len = MAX_ATTR_VALUE_SIZE;
    const uint32_t ret = sd_ble_gatts_characteristic_add(serviceHandle, &charMd, &attr, &chr->handles);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "sd_ble_gatts_characteristic_add() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    halChar->handle = chr->handles.value_handle;
    return 0;
}

int initRxChar(uint16_t serviceHandle, ble_char* halChar, Char* chr) {
    // Characteristic UUID
    chr->uuid.type = halChar->uuid.type;
    chr->uuid.uuid = halChar->uuid.uuid;
    // Characteristic metadata
    ble_gatts_char_md_t charMd = {};
    charMd.char_props.write = 1;
    charMd.char_props.write_wo_resp = 1;
    // Value attribute metadata
    ble_gatts_attr_md_t attrMd = {};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attrMd.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attrMd.write_perm);
    attrMd.vloc = BLE_GATTS_VLOC_STACK;
    attrMd.rd_auth = 0;
    attrMd.wr_auth = 0;
    attrMd.vlen = 1;
    // Value attribute
    ble_gatts_attr_t attr = {};
    attr.p_uuid = &chr->uuid;
    attr.p_attr_md = &attrMd;
    attr.init_len = 1;
    attr.init_offs = 0;
    attr.max_len = MAX_ATTR_VALUE_SIZE;
    const uint32_t ret = sd_ble_gatts_characteristic_add(serviceHandle, &charMd, &attr, &chr->handles);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "sd_ble_gatts_characteristic_add() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    halChar->handle = chr->handles.value_handle;
    return 0;
}

int initChars(ble_service* halService, Service* service) {
    if (halService->char_count > BLE_MAX_CHAR_COUNT) {
        LOG(ERROR, "Maximum number of characteristics exceeded");
        return BLE_ERROR_INVALID_PARAM;
    }
    for (size_t i = 0; i < halService->char_count; ++i) {
        ble_char& halChar = halService->chars[i];
        Char& chr = service->chars[i];
        switch (halChar.type) {
        case BLE_CHAR_TYPE_TX: {
            const int ret = initTxChar(service->handle, &halChar, &chr);
            if (ret != 0) {
                LOG(ERROR, "Unable to initialize TX characteristic");
                return ret;
            }
            break;
        }
        case BLE_CHAR_TYPE_RX: {
            const int ret = initRxChar(service->handle, &halChar, &chr);
            if (ret != 0) {
                LOG(ERROR, "Unable to initialize RX characteristic");
                return ret;
            }
            break;
        }
        default:
            LOG(ERROR, "Invalid characteristic type: %d", (int)halChar.type);
            return BLE_ERROR_INVALID_PARAM;
        }
    }
    service->charCount = halService->char_count;
    return 0;
}

int initProfile(ble_profile* halProfile) {
    if (halProfile->service_count > BLE_MAX_SERVICE_COUNT) {
        LOG(ERROR, "Maximum number of services exceeded");
        return BLE_ERROR_INVALID_PARAM;
    }
    for (size_t i = 0; i < halProfile->service_count; ++i) {
        ble_service& halService = halProfile->services[i];
        Service& service = g_profile.services[i];
        service.uuid.type = halService.uuid.type;
        service.uuid.uuid = halService.uuid.uuid;
        const uint32_t ret = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &service.uuid, &service.handle);
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gatts_service_add() failed: %u", (unsigned)ret);
            return halError(ret);
        }
        const int halRet = initChars(&halService, &service);
        if (halRet != 0) {
            LOG(ERROR, "Unable to initialize service characteristics");
            return halRet;
        }
    }
    g_profile.serviceCount = halProfile->service_count;
    g_profile.callback = halProfile->callback;
    g_profile.userData = halProfile->user_data;
    return 0;
}

int initGatt() {
    // Initialize the GATT module
    ret_code_t ret = nrf_ble_gatt_init(&g_gatt, processGattEvent);
    if (ret != NRF_SUCCESS) {
        return halError(ret);
    }
    // Set MTU size
    ret = nrf_ble_gatt_att_mtu_periph_set(&g_gatt, NRF_SDH_BLE_GATT_MAX_MTU_SIZE);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "nrf_ble_gatt_att_mtu_periph_set() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    return 0;
}

int initGap(const char* deviceName) {
    ble_gap_conn_sec_mode_t secMode = {};
    // Require no protection
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&secMode);
    // Set device name
    uint32_t ret = sd_ble_gap_device_name_set(&secMode, (const uint8_t*)deviceName, strlen(deviceName));
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "sd_ble_gap_device_name_set() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    // Set PPCP (Peripheral Preferred Connection Parameters)
    ble_gap_conn_params_t connParam = {};
    connParam.min_conn_interval = MIN_CONN_INTERVAL;
    connParam.max_conn_interval = MAX_CONN_INTERVAL;
    connParam.conn_sup_timeout = CONN_SUP_TIMEOUT;
    connParam.slave_latency = SLAVE_LATENCY;
    ret = sd_ble_gap_ppcp_set(&connParam);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "sd_ble_gap_ppcp_set() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    return 0;
}

} // ::

int ble_init(void* reserved) {
    if (!nrf_sdh_is_enabled()) {
        LOG(ERROR, "SoftDevice is not enabled");
        return BLE_ERROR_INVALID_STATE;
    }
    // Configure the stack using the SDK's default settings
    uint32_t ramStart = 0; // Start address of the application RAM
    ret_code_t ret = nrf_sdh_ble_default_cfg_set(CONN_CFG_TAG, &ramStart);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "nrf_sdh_ble_default_cfg_set() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    LOG(TRACE, "RAM start: 0x%08x", (unsigned)ramStart);
    // Enable the stack
    ret = nrf_sdh_ble_enable(&ramStart);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "nrf_sdh_ble_enable() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    // Register a handler for BLE events
    NRF_SDH_BLE_OBSERVER(bleObserver, BLE_OBSERVER_PRIO, processBleEvent, nullptr);
    // Register a handler for SOC events
    NRF_SDH_SOC_OBSERVER(socObserver, NRF_SDH_SOC_STACK_OBSERVER_PRIO, processSocEvent, nullptr);
    return 0;
}

int ble_add_base_uuid(const char* uuid, uint8_t* uuid_type, void* reserved) {
    ble_uuid128_t u = {};
    static_assert(sizeof(u.uuid128) == 16, "");
    memcpy(u.uuid128, uuid, sizeof(u.uuid128));
    const uint32_t ret = sd_ble_uuid_vs_add(&u, uuid_type);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "sd_ble_uuid_vs_add() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    return 0;
}

int ble_init_profile(ble_profile* profile, void* reserved) {
    if (profile->version > BLE_API_VERSION) {
        LOG(ERROR, "Unsupported API version");
        return BLE_ERROR_INVALID_PARAM;
    }
    if (!profile->device_name || !profile->callback) {
        LOG(ERROR, "Invalid profile parameters");
        return BLE_ERROR_INVALID_PARAM;
    }
    int ret = initGap(profile->device_name);
    if (ret != 0) {
        LOG(ERROR, "Unable to configure GAP");
        return ret;
    }
    ret = initGatt();
    if (ret != 0) {
        LOG(ERROR, "Unable to initialize GATT");
        return ret;
    }
    ret = initProfile(profile);
    if (ret != 0) {
        LOG(ERROR, "Unable to initialize profile");
        return ret;
    }
    ret = initAdvert();
    if (ret != 0) {
        LOG(ERROR, "Unable to initialize advertising module");
        return ret;
    }
    return 0;
}

int ble_start_advert(void* reserved) {
    const uint32_t ret = ble_advertising_start(&g_advert, BLE_ADV_MODE_FAST);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "ble_advertising_start() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    return 0;
}

void ble_stop_advert(void* reserved) {
    if (g_advert.initialized) {
        const uint32_t ret = sd_ble_gap_adv_stop(g_advert.adv_handle);
        if (ret != NRF_SUCCESS) {
            LOG(WARN, "sd_ble_gap_adv_stop() failed: %u", (unsigned)ret);
        }
    }
}

int ble_get_char_param(uint16_t conn_handle, uint16_t char_handle, ble_char_param* param, void* reserved) {
    if (param->version > BLE_API_VERSION) {
        LOG(ERROR, "Unsupported API version");
        return BLE_ERROR_INVALID_PARAM;
    }
    const auto chr = findChar(char_handle);
    if (!chr) {
        LOG(ERROR, "Unknown characteristic");
        return BLE_ERROR_INVALID_PARAM;
    }
    // Check if the client has enabled notifications
    uint8_t buf[2] = {};
    ble_gatts_value_t val = {};
    val.p_value = buf;
    val.len = sizeof(buf);
    val.offset = 0;
    const uint32_t ret = sd_ble_gatts_value_get(conn_handle, chr->handles.cccd_handle, &val);
    if (ret != NRF_SUCCESS) {
        LOG(ERROR, "sd_ble_gatts_value_get() failed: %u", (unsigned)ret);
        return halError(ret);
    }
    param->notif_enabled = ble_srv_is_notification_enabled(val.p_value);
    return 0;
}

int ble_set_char_value(uint16_t conn_handle, uint16_t char_handle, const char* data, size_t size, unsigned flags, void* reserved) {
    if (flags & BLE_SET_CHAR_VALUE_FLAG_NOTIFY) {
        // Update value of the attribute and send a notification
        uint16_t n = size;
        ble_gatts_hvx_params_t param = {};
        param.handle = char_handle;
        param.p_data = (const uint8_t*)data;
        param.p_len = &n;
        param.type = BLE_GATT_HVX_NOTIFICATION;
        const uint32_t ret = sd_ble_gatts_hvx(conn_handle, &param);
        if (ret == NRF_ERROR_RESOURCES) {
            return BLE_ERROR_BUSY;
        }
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gatts_hvx() failed: %u", (unsigned)ret);
            return halError(ret);
        }
        return n;
    } else {
        ble_gatts_value_t val = {};
        val.p_value = (uint8_t*)data;
        val.len = size;
        const uint32_t ret = sd_ble_gatts_value_set(conn_handle, char_handle, &val);
        if (ret != NRF_SUCCESS) {
            LOG(ERROR, "sd_ble_gatts_value_set() failed: %u", (unsigned)ret);
            return halError(ret);
        }
        return val.len;
    }
}

int ble_get_conn_param(uint16_t conn_handle, ble_conn_param* param, void* reserved) {
    if (param->version > BLE_API_VERSION) {
        LOG(ERROR, "Unsupported API version");
        return BLE_ERROR_INVALID_PARAM;
    }
    const uint16_t mtu = nrf_ble_gatt_eff_mtu_get(&g_gatt, conn_handle);
    if (mtu == 0 || mtu <= ATT_OPCODE_SIZE + ATT_HANDLE_SIZE) {
        LOG(ERROR, "nrf_ble_gatt_eff_mtu_get() failed");
        return BLE_ERROR_UNKNOWN;
    }
    param->max_char_value_size = mtu - ATT_OPCODE_SIZE - ATT_HANDLE_SIZE;
    return 0;
}

void ble_disconnect(uint16_t conn_handle, void* reserved) {
    const uint32_t ret = sd_ble_gap_disconnect(conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    if (ret != NRF_SUCCESS) {
        LOG(WARN, "sd_ble_gap_disconnect() failed: %u", (unsigned)ret);
    }
}
