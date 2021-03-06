#include <mapper.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <set>
#include <bitset>
#include <xyz/openbmc_project/Sensor/Value/server.hpp>
#include <systemd/sd-bus.h>
#include "host-ipmid/ipmid-api.h"
#include <phosphor-logging/log.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include "fruread.hpp"
#include "ipmid.hpp"
#include "sensorhandler.h"
#include "types.hpp"
#include "utils.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

static constexpr uint8_t fruInventoryDevice = 0x10;
static constexpr uint8_t IPMIFruInventory = 0x02;
static constexpr uint8_t BMCSlaveAddress = 0x20;

extern int updateSensorRecordFromSSRAESC(const void *);
extern sd_bus *bus;
extern const ipmi::sensor::IdInfoMap sensors;
extern const FruMap frus;


using namespace phosphor::logging;
using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

void register_netfn_sen_functions()   __attribute__((constructor));

struct sensorTypemap_t {
    uint8_t number;
    uint8_t typecode;
    char dbusname[32];
} ;

sensorTypemap_t g_SensorTypeMap[] = {

    {0x01, 0x6F, "Temp"},
    {0x0C, 0x6F, "DIMM"},
    {0x0C, 0x6F, "MEMORY_BUFFER"},
    {0x07, 0x6F, "PROC"},
    {0x07, 0x6F, "CORE"},
    {0x07, 0x6F, "CPU"},
    {0x0F, 0x6F, "BootProgress"},
    {0xe9, 0x09, "OccStatus"},  // E9 is an internal mapping to handle sensor type code os 0x09
    {0xC3, 0x6F, "BootCount"},
    {0x1F, 0x6F, "OperatingSystemStatus"},
    {0x12, 0x6F, "SYSTEM_EVENT"},
    {0xC7, 0x03, "SYSTEM"},
    {0xC7, 0x03, "MAIN_PLANAR"},
    {0xC2, 0x6F, "PowerCap"},
    {0x0b, 0xCA, "PowerSupplyRedundancy"},
    {0xDA, 0x03, "TurboAllowed"},
    {0xD8, 0xC8, "PowerSupplyDerating"},
    {0xFF, 0x00, ""},
};


struct sensor_data_t {
    uint8_t sennum;
}  __attribute__ ((packed)) ;

struct sensorreadingresp_t {
    uint8_t value;
    uint8_t operation;
    uint8_t indication[2];
}  __attribute__ ((packed)) ;

int get_bus_for_path(const char *path, char **busname) {
    return mapper_get_service(bus, path, busname);
}

int legacy_dbus_openbmc_path(const char *type, const uint8_t num, dbus_interface_t *interface) {
    char  *busname = NULL;
    const char  *iface = "org.openbmc.managers.System";
    const char  *objname = "/org/openbmc/managers/System";
    char  *str1 = NULL, *str2, *str3;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;


    int r;
    r = get_bus_for_path(objname, &busname);
    if (r < 0) {
        fprintf(stderr, "Failed to get %s busname: %s\n",
                objname, strerror(-r));
        goto final;
    }

    r = sd_bus_call_method(bus,busname,objname,iface, "getObjectFromByteId",
                           &error, &reply, "sy", type, num);
    if (r < 0) {
        fprintf(stderr, "Failed to create a method call: %s", strerror(-r));
        goto final;
    }

    r = sd_bus_message_read(reply, "(ss)", &str2, &str3);
    if (r < 0) {
        fprintf(stderr, "Failed to get a response: %s", strerror(-r));
        goto final;
    }

    if (strlen(str2) == 0)
    {
        // Path being empty occurs when the sensor id is not in SystemManager
        r = -EINVAL;
        goto final;
    }

    r = get_bus_for_path(str2, &str1);
    if (r < 0) {
        fprintf(stderr, "Failed to get %s busname: %s\n",
                str2, strerror(-r));
        goto final;
    }

    strncpy(interface->bus, str1, MAX_DBUS_PATH);
    strncpy(interface->path, str2, MAX_DBUS_PATH);
    strncpy(interface->interface, str3, MAX_DBUS_PATH);

    interface->sensornumber = num;
    // Make sure we know that the type hasn't been set, as newer codebase will
    // set it automatically from the YAML at this step.
    interface->sensortype = 0;

final:

    sd_bus_error_free(&error);
    reply = sd_bus_message_unref(reply);
    free(busname);
    free(str1);

    return r;
}

// Use a lookup table to find the interface name of a specific sensor
// This will be used until an alternative is found.  this is the first
// step for mapping IPMI
int find_openbmc_path(uint8_t num, dbus_interface_t *interface) {
    int rc;

    // When the sensor map does not contain the sensor requested,
    // fall back to the legacy DBus lookup (deprecated)
    const auto& sensor_it = sensors.find(num);
    if (sensor_it == sensors.end())
    {
        return legacy_dbus_openbmc_path("SENSOR", num, interface);
    }

    const auto& info = sensor_it->second;

    char* busname = nullptr;
    rc = get_bus_for_path(info.sensorPath.c_str(), &busname);
    if (rc < 0) {
        fprintf(stderr, "Failed to get %s busname: %s\n",
                info.sensorPath.c_str(),
                busname);
        goto final;
    }

    interface->sensortype = info.sensorType;
    strcpy(interface->bus, busname);
    strcpy(interface->path, info.sensorPath.c_str());
    // Take the interface name from the beginning of the DbusInterfaceMap. This
    // works for the Value interface but may not suffice for more complex
    // sensors.
    // tracked https://github.com/openbmc/phosphor-host-ipmid/issues/103
    strcpy(interface->interface, info.propertyInterfaces.begin()->first.c_str());
    interface->sensornumber = num;

final:
    free(busname);
    return rc;
}


/////////////////////////////////////////////////////////////////////
//
// Routines used by ipmi commands wanting to interact on the dbus
//
/////////////////////////////////////////////////////////////////////
int set_sensor_dbus_state_s(uint8_t number, const char *method, const char *value) {


    dbus_interface_t a;
    int r;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m=NULL;

    fprintf(ipmidbus, "Attempting to set a dbus Variant Sensor 0x%02x via %s with a value of %s\n",
        number, method, value);

    r = find_openbmc_path(number, &a);

    if (r < 0) {
        fprintf(stderr, "Failed to find Sensor 0x%02x\n", number);
        return 0;
    }

    r = sd_bus_message_new_method_call(bus,&m,a.bus,a.path,a.interface,method);
    if (r < 0) {
        fprintf(stderr, "Failed to create a method call: %s", strerror(-r));
        goto final;
    }

    r = sd_bus_message_append(m, "v", "s", value);
    if (r < 0) {
        fprintf(stderr, "Failed to create a input parameter: %s", strerror(-r));
        goto final;
    }


    r = sd_bus_call(bus, m, 0, &error, NULL);
    if (r < 0) {
        fprintf(stderr, "Failed to call the method: %s", strerror(-r));
    }

final:
    sd_bus_error_free(&error);
    m = sd_bus_message_unref(m);

    return 0;
}
int set_sensor_dbus_state_y(uint8_t number, const char *method, const uint8_t value) {


    dbus_interface_t a;
    int r;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m=NULL;

    fprintf(ipmidbus, "Attempting to set a dbus Variant Sensor 0x%02x via %s with a value of 0x%02x\n",
        number, method, value);

    r = find_openbmc_path(number, &a);

    if (r < 0) {
        fprintf(stderr, "Failed to find Sensor 0x%02x\n", number);
        return 0;
    }

    r = sd_bus_message_new_method_call(bus,&m,a.bus,a.path,a.interface,method);
    if (r < 0) {
        fprintf(stderr, "Failed to create a method call: %s", strerror(-r));
        goto final;
    }

    r = sd_bus_message_append(m, "v", "i", value);
    if (r < 0) {
        fprintf(stderr, "Failed to create a input parameter: %s", strerror(-r));
        goto final;
    }


    r = sd_bus_call(bus, m, 0, &error, NULL);
    if (r < 0) {
        fprintf(stderr, "12 Failed to call the method: %s", strerror(-r));
    }

final:
    sd_bus_error_free(&error);
    m = sd_bus_message_unref(m);

    return 0;
}

uint8_t dbus_to_sensor_type(char *p) {

    sensorTypemap_t *s = g_SensorTypeMap;
    char r=0;
    while (s->number != 0xFF) {
        if (!strcmp(s->dbusname,p)) {
            r = s->typecode;
             break;
        }
        s++;
    }

    if (s->number == 0xFF)
        printf("Failed to find Sensor Type %s\n", p);

    return r;
}


uint8_t get_type_from_interface(dbus_interface_t dbus_if) {

    char *p;
    uint8_t type;

    // This is where sensors that do not exist in dbus but do
    // exist in the host code stop.  This should indicate it
    // is not a supported sensor
    if (dbus_if.interface[0] == 0) { return 0;}

    // Fetch type from interface itself.
    if (dbus_if.sensortype != 0)
    {
        type = dbus_if.sensortype;
    } else {
        // Non InventoryItems
        p = strrchr (dbus_if.path, '/');
        type = dbus_to_sensor_type(p+1);
    }

    return type;
 }

// Replaces find_sensor
uint8_t find_type_for_sensor_number(uint8_t num) {
    int r;
    dbus_interface_t dbus_if;
    r = find_openbmc_path(num, &dbus_if);
    if (r < 0) {
        fprintf(stderr, "Could not find sensor %d\n", num);
        return 0;
    }
    return get_type_from_interface(dbus_if);
}





ipmi_ret_t ipmi_sen_get_sensor_type(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                             ipmi_request_t request, ipmi_response_t response,
                             ipmi_data_len_t data_len, ipmi_context_t context)
{
    sensor_data_t *reqptr = (sensor_data_t*)request;
    ipmi_ret_t rc = IPMI_CC_OK;

    printf("IPMI GET_SENSOR_TYPE [0x%02X]\n",reqptr->sennum);

    // TODO Not sure what the System-event-sensor is suppose to return
    // need to ask Hostboot team
    unsigned char buf[] = {0x00,0x6F};

    buf[0] = find_type_for_sensor_number(reqptr->sennum);

    // HACK UNTIL Dbus gets updated or we find a better way
    if (buf[0] == 0) {
        rc = IPMI_CC_SENSOR_INVALID;
    }


    *data_len = sizeof(buf);
    memcpy(response, &buf, *data_len);

    return rc;
}

const std::set<std::string> analogSensorInterfaces =
{
    "xyz.openbmc_project.Sensor.Value",
    "xyz.openbmc_project.Control.FanPwm",
};

bool isAnalogSensor(const std::string& interface)
{
    return (analogSensorInterfaces.count(interface));
}

ipmi_ret_t setSensorReading(void *request)
{
    ipmi::sensor::SetSensorReadingReq cmdData =
            *(static_cast<ipmi::sensor::SetSensorReadingReq *>(request));

    // Check if the Sensor Number is present
    const auto iter = sensors.find(cmdData.number);
    if (iter == sensors.end())
    {
        return IPMI_CC_SENSOR_INVALID;
    }

    try
    {
        if (ipmi::sensor::Mutability::Write !=
              (iter->second.mutability & ipmi::sensor::Mutability::Write))
        {
            log<level::ERR>("Sensor Set operation is not allowed",
                            entry("SENSOR_NUM=%d", cmdData.number));
            return IPMI_CC_ILLEGAL_COMMAND;
        }
        return iter->second.updateFunc(cmdData, iter->second);
    }
    catch (InternalFailure& e)
    {
         log<level::ERR>("Set sensor failed",
                         entry("SENSOR_NUM=%d", cmdData.number));
         commit<InternalFailure>();
    }
    catch (const std::runtime_error& e)
    {
        log<level::ERR>(e.what());
    }

    return IPMI_CC_UNSPECIFIED_ERROR;
}

ipmi_ret_t ipmi_sen_set_sensor(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                             ipmi_request_t request, ipmi_response_t response,
                             ipmi_data_len_t data_len, ipmi_context_t context)
{
    sensor_data_t *reqptr = (sensor_data_t*)request;

    log<level::DEBUG>("IPMI SET_SENSOR",
                      entry("SENSOR_NUM=0x%02x", reqptr->sennum));

    /*
     * This would support the Set Sensor Reading command for the presence
     * and functional state of Processor, Core & DIMM. For the remaining
     * sensors the existing support is invoked.
     */
    auto ipmiRC = setSensorReading(request);

    if(ipmiRC == IPMI_CC_SENSOR_INVALID)
    {
        updateSensorRecordFromSSRAESC(reqptr);
        ipmiRC = IPMI_CC_OK;
    }

    *data_len=0;
    return ipmiRC;
}

ipmi_ret_t legacyGetSensorReading(uint8_t sensorNum,
                                  ipmi_response_t response,
                                  ipmi_data_len_t data_len)
{
    int r;
    dbus_interface_t a;
    sd_bus *bus = ipmid_get_sd_bus_connection();
    ipmi_ret_t rc = IPMI_CC_SENSOR_INVALID;
    uint8_t type = 0;
    sd_bus_message *reply = NULL;
    int reading = 0;
    char* assertion = NULL;
    sensorreadingresp_t *resp = (sensorreadingresp_t*) response;
    *data_len=0;

    r = find_openbmc_path(sensorNum, &a);
    if (r < 0)
    {
        sd_journal_print(LOG_ERR, "Failed to find Sensor 0x%02x\n", sensorNum);
        return IPMI_CC_SENSOR_INVALID;
    }

    type = get_type_from_interface(a);
    if (type == 0)
    {
        sd_journal_print(LOG_ERR, "Failed to find Sensor 0x%02x\n",
                         sensorNum);
        return IPMI_CC_SENSOR_INVALID;
    }

    switch(type) {
        case 0xC2:
            r = sd_bus_get_property(bus,a.bus, a.path, a.interface,
                                    "value", NULL, &reply, "i");
            if (r < 0)
            {
                sd_journal_print(LOG_ERR, "Failed to call sd_bus_get_property:"
                                 " %d, %s\n", r, strerror(-r));
                sd_journal_print(LOG_ERR, "Bus: %s, Path: %s, Interface: %s\n",
                                 a.bus, a.path, a.interface);
                break;
            }

            r = sd_bus_message_read(reply, "i", &reading);
            if (r < 0) {
                sd_journal_print(LOG_ERR, "Failed to read sensor: %s\n",
                                 strerror(-r));
                break;
            }

            rc = IPMI_CC_OK;
            *data_len=sizeof(sensorreadingresp_t);

            resp->value         = (uint8_t)reading;
            resp->operation     = 0;
            resp->indication[0] = 0;
            resp->indication[1] = 0;
            break;

        case 0xC8:
            r = sd_bus_get_property(bus,a.bus, a.path, a.interface,
                                    "value", NULL, &reply, "i");
            if (r < 0)
            {
                sd_journal_print(LOG_ERR, "Failed to call sd_bus_get_property:"
                                 " %d, %s\n", r, strerror(-r));
                sd_journal_print(LOG_ERR, "Bus: %s, Path: %s, Interface: %s\n",
                                 a.bus, a.path, a.interface);
                break;
            }

            r = sd_bus_message_read(reply, "i", &reading);
            if (r < 0) {
                sd_journal_print(LOG_ERR, "Failed to read sensor: %s\n",
                                 strerror(-r));
                break;
            }

            rc = IPMI_CC_OK;
            *data_len=sizeof(sensorreadingresp_t);

            resp->value         = 0;
            resp->operation     = 0;
            resp->indication[0] = (uint8_t)reading;
            resp->indication[1] = 0;
            break;

        //TODO openbmc/openbmc#2154 Move this sensor to right place.
        case 0xCA:
            r = sd_bus_get_property(bus,a.bus, a.path, a.interface, "value",
                                    NULL, &reply, "s");
            if (r < 0)
            {
                sd_journal_print(LOG_ERR, "Failed to call sd_bus_get_property:"
                                 " %d, %s\n", r, strerror(-r));
                sd_journal_print(LOG_ERR, "Bus: %s, Path: %s, Interface: %s\n",
                                 a.bus, a.path, a.interface);
                break;
            }

            r = sd_bus_message_read(reply, "s", &assertion);
            if (r < 0)
            {
                sd_journal_print(LOG_ERR, "Failed to read sensor: %s\n",
                                 strerror(-r));
                break;
            }

            rc = IPMI_CC_OK;
            *data_len=sizeof(sensorreadingresp_t);

            resp->value         = 0;
            resp->operation     = 0;
            if (strcmp(assertion,"Enabled") == 0)
            {
                resp->indication[0] = 0x02;
            }
            else
            {
                resp->indication[0] = 0x1;
            }
            resp->indication[1] = 0;
            break;

        default:
        {
            return IPMI_CC_SENSOR_INVALID;
        }
    }

    reply = sd_bus_message_unref(reply);

    return rc;
}

ipmi_ret_t ipmi_sen_get_sensor_reading(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                             ipmi_request_t request, ipmi_response_t response,
                             ipmi_data_len_t data_len, ipmi_context_t context)
{
    sensor_data_t *reqptr = (sensor_data_t*)request;
    sensorreadingresp_t *resp = (sensorreadingresp_t*) response;
    ipmi::sensor::GetSensorResponse getResponse {};
    static constexpr auto scanningEnabledBit = 6;

    const auto iter = sensors.find(reqptr->sennum);
    if (iter == sensors.end())
    {
        return legacyGetSensorReading(reqptr->sennum, response, data_len);
    }
    if (ipmi::sensor::Mutability::Read !=
          (iter->second.mutability & ipmi::sensor::Mutability::Read))
    {
        return IPMI_CC_ILLEGAL_COMMAND;
    }

    try
    {
        getResponse =  iter->second.getFunc(iter->second);
        *data_len = getResponse.size();
        memcpy(resp, getResponse.data(), *data_len);
        resp->operation = 1 << scanningEnabledBit;
        return IPMI_CC_OK;
    }
    catch (const std::exception& e)
    {
        *data_len = getResponse.size();
        memcpy(resp, getResponse.data(), *data_len);
        return IPMI_CC_OK;
    }
}

void getSensorThresholds(uint8_t sensorNum,
                         get_sdr::GetSensorThresholdsResponse* response)
{
    constexpr auto warningThreshIntf =
        "xyz.openbmc_project.Sensor.Threshold.Warning";
    constexpr auto criticalThreshIntf =
        "xyz.openbmc_project.Sensor.Threshold.Critical";

    sdbusplus::bus::bus bus{ipmid_get_sd_bus_connection()};

    const auto iter = sensors.find(sensorNum);
    const auto info = iter->second;

    auto service = ipmi::getService(bus, info.sensorInterface, info.sensorPath);

    auto warnThresholds = ipmi::getAllDbusProperties(bus,
                                                     service,
                                                     info.sensorPath,
                                                     warningThreshIntf);

    double warnLow = warnThresholds["WarningLow"].get<int64_t>();
    double warnHigh = warnThresholds["WarningHigh"].get<int64_t>();

    if (warnLow != 0)
    {
        warnLow *= pow(10, info.scale - info.exponentR);
        response->lowerNonCritical = static_cast<uint8_t>((
            warnLow - info.scaledOffset) / info.coefficientM);
        response->validMask |= static_cast<uint8_t>(
                ipmi::sensor::ThresholdMask::NON_CRITICAL_LOW_MASK);
    }

    if (warnHigh != 0)
    {
        warnHigh *= pow(10, info.scale - info.exponentR);
        response->upperNonCritical = static_cast<uint8_t>((
            warnHigh - info.scaledOffset) / info.coefficientM);
        response->validMask |= static_cast<uint8_t>(
                ipmi::sensor::ThresholdMask::NON_CRITICAL_HIGH_MASK);
    }

    auto critThresholds = ipmi::getAllDbusProperties(bus,
                                                     service,
                                                     info.sensorPath,
                                                     criticalThreshIntf);
    double critLow = critThresholds["CriticalLow"].get<int64_t>();
    double critHigh = critThresholds["CriticalHigh"].get<int64_t>();

    if (critLow != 0)
    {
        critLow *= pow(10, info.scale - info.exponentR);
        response->lowerCritical = static_cast<uint8_t>((
            critLow - info.scaledOffset) / info.coefficientM);
        response->validMask |= static_cast<uint8_t>(
                ipmi::sensor::ThresholdMask::CRITICAL_LOW_MASK);
    }

    if (critHigh != 0)
    {
        critHigh *= pow(10, info.scale - info.exponentR);
        response->upperCritical = static_cast<uint8_t>((
            critHigh - info.scaledOffset)/ info.coefficientM);
        response->validMask |= static_cast<uint8_t>(
                ipmi::sensor::ThresholdMask::CRITICAL_HIGH_MASK);
    }
}

ipmi_ret_t ipmi_sen_get_sensor_thresholds(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                             ipmi_request_t request, ipmi_response_t response,
                             ipmi_data_len_t data_len, ipmi_context_t context)
{
    constexpr auto valueInterface = "xyz.openbmc_project.Sensor.Value";

    if (*data_len != sizeof(uint8_t))
    {
        *data_len = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }

    auto sensorNum = *(reinterpret_cast<const uint8_t *>(request));
    *data_len = 0;

    const auto iter = sensors.find(sensorNum);
    if (iter == sensors.end())
    {
        return IPMI_CC_SENSOR_INVALID;
    }

    const auto info = iter->second;

    //Proceed only if the sensor value interface is implemented.
    if (info.propertyInterfaces.find(valueInterface) ==
        info.propertyInterfaces.end())
    {
        //return with valid mask as 0
        return IPMI_CC_OK;
    }

    auto responseData =
        reinterpret_cast<get_sdr::GetSensorThresholdsResponse*>(response);

    try
    {
        getSensorThresholds(sensorNum, responseData);
    }
    catch (std::exception& e)
    {
        //Mask if the property is not present
        responseData->validMask = 0;
    }

    *data_len = sizeof(get_sdr::GetSensorThresholdsResponse);
    return IPMI_CC_OK;
}

ipmi_ret_t ipmi_sen_wildcard(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                             ipmi_request_t request, ipmi_response_t response,
                             ipmi_data_len_t data_len, ipmi_context_t context)
{
    ipmi_ret_t rc = IPMI_CC_INVALID;

    printf("IPMI S/E Wildcard Netfn:[0x%X], Cmd:[0x%X]\n",netfn,cmd);
    *data_len = 0;

    return rc;
}

ipmi_ret_t ipmi_sen_get_sdr_info(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                 ipmi_request_t request,
                                 ipmi_response_t response,
                                 ipmi_data_len_t data_len,
                                 ipmi_context_t context)
{
    auto resp = static_cast<get_sdr_info::GetSdrInfoResp*>(response);
    if (request == nullptr ||
        get_sdr_info::request::get_count(request) == false)
    {
        // Get Sensor Count
        resp->count = sensors.size() + frus.size();
    }
    else
    {
        resp->count = 1;
    }

    // Multiple LUNs not supported.
    namespace response = get_sdr_info::response;
    response::set_lun_present(0, &(resp->luns_and_dynamic_population));
    response::set_lun_not_present(1, &(resp->luns_and_dynamic_population));
    response::set_lun_not_present(2, &(resp->luns_and_dynamic_population));
    response::set_lun_not_present(3, &(resp->luns_and_dynamic_population));
    response::set_static_population(&(resp->luns_and_dynamic_population));

    *data_len = SDR_INFO_RESP_SIZE;

    return IPMI_CC_OK;
}

ipmi_ret_t ipmi_sen_reserve_sdr(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                ipmi_request_t request,
                                ipmi_response_t response,
                                ipmi_data_len_t data_len,
                                ipmi_context_t context)
{
    // A constant reservation ID is okay until we implement add/remove SDR.
    const uint16_t reservation_id = 1;
    *(uint16_t*)response = reservation_id;
    *data_len = sizeof(uint16_t);

    printf("Created new IPMI SDR reservation ID %d\n", *(uint16_t*)response);
    return IPMI_CC_OK;
}

void setUnitFieldsForObject(const ipmi::sensor::Info *info,
                            get_sdr::SensorDataFullRecordBody *body)
{
    namespace server = sdbusplus::xyz::openbmc_project::Sensor::server;
    try
    {
        auto unit = server::Value::convertUnitFromString(info->unit);
        // Unit strings defined in
        // phosphor-dbus-interfaces/xyz/openbmc_project/Sensor/Value.interface.yaml
        switch (unit)
        {
            case server::Value::Unit::DegreesC:
                body->sensor_units_2_base = get_sdr::SENSOR_UNIT_DEGREES_C;
                break;
            case server::Value::Unit::RPMS:
                body->sensor_units_2_base = get_sdr::SENSOR_UNIT_REVOLUTIONS; // revolutions
                get_sdr::body::set_rate_unit(0b100, body); // per minute
                break;
            case server::Value::Unit::Volts:
                body->sensor_units_2_base = get_sdr::SENSOR_UNIT_VOLTS;
                break;
            case server::Value::Unit::Meters:
                body->sensor_units_2_base = get_sdr::SENSOR_UNIT_METERS;
                break;
            case server::Value::Unit::Amperes:
                body->sensor_units_2_base = get_sdr::SENSOR_UNIT_AMPERES;
                break;
            case server::Value::Unit::Joules:
                body->sensor_units_2_base = get_sdr::SENSOR_UNIT_JOULES;
                break;
            case server::Value::Unit::Watts:
                body->sensor_units_2_base = get_sdr::SENSOR_UNIT_WATTS;
                break;
            default:
                // Cannot be hit.
                fprintf(stderr, "Unknown value unit type: = %s\n",
                        info->unit.c_str());
        }
    }
    catch (sdbusplus::exception::InvalidEnumString e)
    {
        log<level::WARNING>("Warning: no unit provided for sensor!");
    }
}

ipmi_ret_t populate_record_from_dbus(get_sdr::SensorDataFullRecordBody *body,
                                     const ipmi::sensor::Info *info,
                                     ipmi_data_len_t data_len)
{
    /* Functional sensor case */
    if (isAnalogSensor(info->propertyInterfaces.begin()->first))
    {

        body->sensor_units_1 = 0; // unsigned, no rate, no modifier, not a %

        /* Unit info */
        setUnitFieldsForObject(info, body);

        get_sdr::body::set_b(info->coefficientB, body);
        get_sdr::body::set_m(info->coefficientM, body);
        get_sdr::body::set_b_exp(info->exponentB, body);
        get_sdr::body::set_r_exp(info->exponentR, body);

        get_sdr::body::set_id_type(0b00, body); // 00 = unicode
    }

    /* ID string */
    auto id_string = info->sensorNameFunc(*info);

    if (id_string.length() > FULL_RECORD_ID_STR_MAX_LENGTH)
    {
        get_sdr::body::set_id_strlen(FULL_RECORD_ID_STR_MAX_LENGTH, body);
    }
    else
    {
        get_sdr::body::set_id_strlen(id_string.length(), body);
    }
    strncpy(body->id_string, id_string.c_str(),
            get_sdr::body::get_id_strlen(body));

    return IPMI_CC_OK;
};

ipmi_ret_t ipmi_fru_get_sdr(ipmi_request_t request, ipmi_response_t response,
                            ipmi_data_len_t data_len)
{
    auto req = reinterpret_cast<get_sdr::GetSdrReq*>(request);
    auto resp = reinterpret_cast<get_sdr::GetSdrResp*>(response);
    get_sdr::SensorDataFruRecord record {};
    auto dataLength = 0;

    auto fru = frus.begin();
    uint8_t fruID {};
    auto recordID = get_sdr::request::get_record_id(req);

    fruID = recordID - FRU_RECORD_ID_START;
    fru = frus.find(fruID);
    if (fru == frus.end())
    {
        return IPMI_CC_SENSOR_INVALID;
    }

    /* Header */
    get_sdr::header::set_record_id(recordID, &(record.header));
    record.header.sdr_version = SDR_VERSION; // Based on IPMI Spec v2.0 rev 1.1
    record.header.record_type = get_sdr::SENSOR_DATA_FRU_RECORD;
    record.header.record_length = sizeof(record.key) + sizeof(record.body);

    /* Key */
    record.key.fruID = fruID;
    record.key.accessLun |= IPMI_LOGICAL_FRU;
    record.key.deviceAddress = BMCSlaveAddress;

    /* Body */
    record.body.entityID = fru->second[0].entityID;
    record.body.entityInstance = fru->second[0].entityInstance;
    record.body.deviceType = fruInventoryDevice;
    record.body.deviceTypeModifier = IPMIFruInventory;

    /* Device ID string */
    auto deviceID = fru->second[0].path.substr(
            fru->second[0].path.find_last_of('/') + 1,
            fru->second[0].path.length());


    if (deviceID.length() > get_sdr::FRU_RECORD_DEVICE_ID_MAX_LENGTH)
    {
        get_sdr::body::set_device_id_strlen(
                get_sdr::FRU_RECORD_DEVICE_ID_MAX_LENGTH,
                &(record.body));
    }
    else
    {
        get_sdr::body::set_device_id_strlen(deviceID.length(),
                &(record.body));
    }

    strncpy(record.body.deviceID, deviceID.c_str(),
            get_sdr::body::get_device_id_strlen(&(record.body)));

    if (++fru == frus.end())
    {
        get_sdr::response::set_next_record_id(END_OF_RECORD, resp); // last record
    }
    else
    {
        get_sdr::response::set_next_record_id(
                (FRU_RECORD_ID_START + fru->first), resp);
    }

    if (req->bytes_to_read > (sizeof(*resp) - req->offset))
    {
        dataLength = (sizeof(*resp) - req->offset);
    }
    else
    {
        dataLength =  req->bytes_to_read;
    }

    if (dataLength <= 0)
    {
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }

    memcpy(resp->record_data,
            reinterpret_cast<uint8_t*>(&record) + req->offset,
            (dataLength));

    *data_len = dataLength;
    *data_len += 2; // additional 2 bytes for next record ID

    return IPMI_CC_OK;
}

ipmi_ret_t ipmi_sen_get_sdr(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                            ipmi_request_t request, ipmi_response_t response,
                            ipmi_data_len_t data_len, ipmi_context_t context)
{
    ipmi_ret_t ret = IPMI_CC_OK;
    get_sdr::GetSdrReq *req = (get_sdr::GetSdrReq*)request;
    get_sdr::GetSdrResp *resp = (get_sdr::GetSdrResp*)response;
    get_sdr::SensorDataFullRecord record = {0};
    if (req != NULL)
    {
        // Note: we use an iterator so we can provide the next ID at the end of
        // the call.
        auto sensor = sensors.begin();
        auto recordID = get_sdr::request::get_record_id(req);

        // At the beginning of a scan, the host side will send us id=0.
        if (recordID != 0)
        {
            // recordID greater then 255,it means it is a FRU record.
            // Currently we are supporting two record types either FULL record
            // or FRU record.
            if (recordID >= FRU_RECORD_ID_START)
            {
                return ipmi_fru_get_sdr(request, response, data_len);
            }
            else
            {
                sensor = sensors.find(recordID);
                if (sensor == sensors.end())
                {
                    return IPMI_CC_SENSOR_INVALID;
                }
            }
        }

        uint8_t sensor_id = sensor->first;

        /* Header */
        get_sdr::header::set_record_id(sensor_id, &(record.header));
        record.header.sdr_version = 0x51; // Based on IPMI Spec v2.0 rev 1.1
        record.header.record_type = get_sdr::SENSOR_DATA_FULL_RECORD;
        record.header.record_length = sizeof(get_sdr::SensorDataFullRecord);

        /* Key */
        get_sdr::key::set_owner_id_bmc(&(record.key));
        record.key.sensor_number = sensor_id;

        /* Body */
        record.body.entity_id = sensor->second.entityType;
        record.body.sensor_type = sensor->second.sensorType;
        record.body.event_reading_type = sensor->second.sensorReadingType;
        record.body.entity_instance = sensor->second.instance;

        // Set the type-specific details given the DBus interface
        ret = populate_record_from_dbus(&(record.body), &(sensor->second),
                                        data_len);

        if (++sensor == sensors.end())
        {
            // we have reached till end of sensor, so assign the next record id
            // to 256(Max Sensor ID = 255) + FRU ID(may start with 0).
            auto next_record_id = (frus.size()) ?
                frus.begin()->first + FRU_RECORD_ID_START :
                END_OF_RECORD;

            get_sdr::response::set_next_record_id(next_record_id, resp);
        }
        else
        {
            get_sdr::response::set_next_record_id(sensor->first, resp);
        }

        *data_len = sizeof(get_sdr::GetSdrResp) - req->offset;
        memcpy(resp->record_data, (char*)&record + req->offset,
               sizeof(get_sdr::SensorDataFullRecord) - req->offset);
    }

    return ret;
}


void register_netfn_sen_functions()
{
    // <Wildcard Command>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_WILDCARD,
                           nullptr, ipmi_sen_wildcard,
                           PRIVILEGE_USER);

    // <Get Sensor Type>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_GET_SENSOR_TYPE,
                           nullptr, ipmi_sen_get_sensor_type,
                           PRIVILEGE_USER);

    // <Set Sensor Reading and Event Status>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_SET_SENSOR,
                           nullptr, ipmi_sen_set_sensor,
                           PRIVILEGE_OPERATOR);

    // <Get Sensor Reading>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_GET_SENSOR_READING,
                           nullptr, ipmi_sen_get_sensor_reading,
                           PRIVILEGE_USER);

    // <Reserve Device SDR Repository>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_RESERVE_DEVICE_SDR_REPO,
                           nullptr, ipmi_sen_reserve_sdr,
                           PRIVILEGE_USER);

    // <Get Device SDR Info>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_GET_DEVICE_SDR_INFO,
                           nullptr, ipmi_sen_get_sdr_info,
                           PRIVILEGE_USER);

    // <Get Device SDR>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_GET_DEVICE_SDR,
                           nullptr, ipmi_sen_get_sdr,
                           PRIVILEGE_USER);

    // <Get Sensor Thresholds>
    ipmi_register_callback(NETFUN_SENSOR, IPMI_CMD_GET_SENSOR_THRESHOLDS,
                           nullptr, ipmi_sen_get_sensor_thresholds,
                           PRIVILEGE_USER);

    return;
}
