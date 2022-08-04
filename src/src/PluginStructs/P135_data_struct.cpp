#include "../PluginStructs/P135_data_struct.h"

#ifdef USES_P135

/**************************************************************************
* Constructor
**************************************************************************/
P135_data_struct::P135_data_struct(taskIndex_t taskIndex,
                                   uint8_t     sensorType,
                                   uint16_t    altitude,
                                   float       tempOffset,
                                   bool        autoCalibrate,
                                   bool        lowPowerMeasurement,
                                   bool        useSingleShot)
  : _sensorType(sensorType), _altitude(altitude), _tempOffset(tempOffset), _autoCalibrate(autoCalibrate),
  _lowPowerMeasurement(lowPowerMeasurement), _useSingleShot(useSingleShot), initialized(false) {
  scd4x = new (std::nothrow) SCD4x(static_cast<scd4x_sensor_type_e>(_sensorType)); // Don't start measurement, we want to set arguments

  if (scd4x != nullptr) {
    if (scd4x->begin(false, _autoCalibrate)) {
      const uint16_t orgAltitude = scd4x->getSensorAltitude();

      if (_altitude != 0) {
        scd4x->setSensorAltitude(_altitude);
      }
      const float orgTempOffset = scd4x->getTemperatureOffset();

      if (!essentiallyEqual(_tempOffset, 0.0f)) {
        scd4x->setTemperatureOffset(_tempOffset);
      }
      const bool hasSerial = scd4x->getSerialNumber(serialNumber); // Not yet reading, get serial

      initialized = startPeriodicMeasurements();                   // Start the desired periodic measurement

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("SCD4x: Init ");

        if (initialized) {
          log += F("success, serial number: ");

          if (hasSerial) {
            log += String(serialNumber);
          } else {
            log += F("(unknown)");
          }
          log += F(", org.alt.comp.: ");
          log += orgAltitude;
          log += F(" m, org.temp.offs.: ");
          log += toString(orgTempOffset, 2);
          log += 'C';
        } else {
          log += F("error");
        }
        addLogMove(LOG_LEVEL_INFO, log);
      }
    } else {
      addLog(LOG_LEVEL_ERROR, F("SDC4x: Sensor not detected."));
    }
  }
}

/*****************************************************
* startPeriodicMeasurements
*****************************************************/
bool P135_data_struct::startPeriodicMeasurements() {
  if (_useSingleShot) {
    return true;                                      // Start measurment at first PLUGIN_READ
  } else if (_lowPowerMeasurement) {
    return scd4x->startLowPowerPeriodicMeasurement(); // Reports every 30 seconds
  }
  return scd4x->startPeriodicMeasurement();           // Reports every 5 seconds
}

/*****************************************************
* Destructor
*****************************************************/
P135_data_struct::~P135_data_struct() {
  delete scd4x;
  scd4x = nullptr;
}

/*****************************************************
* plugin_read
*****************************************************/
bool P135_data_struct::plugin_read(struct EventStruct *event)           {
  bool success = false;

  if (isInitialized()) {
    bool getMeasure     = true;
    uint16_t timerDelay = 0;

    if ((_useSingleShot) &&
        !singleShotStarted &&
        scd4x->measureSingleShot()) {
      getMeasure        = false;
      singleShotStarted = true;

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        addLog(LOG_LEVEL_INFO, F("SCD4x: SingleShot measurement started."));
      }
    }

    if (getMeasure && scd4x->readMeasurement()) {
      UserVar[event->BaseVarIndex]     = scd4x->getCO2();
      UserVar[event->BaseVarIndex + 1] = scd4x->getHumidity();
      UserVar[event->BaseVarIndex + 2] = scd4x->getTemperature();

      success = !firstRead;           // Discard first measurement

      singleShotStarted = false;
      firstRead         = false;      // No longer first read
    } else {
      if (getMeasure && !firstRead) { // We got delayed somehow, let's wait a little more
        timerDelay = P135_EXTEND_MEASURE_TIME;
      } else {
        if (_useSingleShot) {         // Single-shot started?
          timerDelay = P135_SINGLE_SHOT_MEASURE_TIME;
        } else if (_lowPowerMeasurement) {
          timerDelay = P135_LOW_POWER_MEASURE_TIME;
        } else {
          timerDelay = P135_NORMAL_MEASURE_TIME;
        }
      }
    }

    if (timerDelay != 0) { // Schedule next PLUGIN_READ
      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("SCD4x: READ Scheduler started: +");
        log += timerDelay;
        log += F(" ms.");
        addLog(LOG_LEVEL_INFO, log);
      }
      Scheduler.schedule_task_device_timer(event->TaskIndex, millis() + timerDelay);
    }
  } else {
    # if P135_FEATURE_RESET_COMMANDS

    if (mustRunFactoryReset) {
      success = scd4x->performFactoryReset();

      if (success) {
        initialized = startPeriodicMeasurements(); // Select the correct periodic measurement, and start a READ
        Scheduler.schedule_task_device_timer(event->TaskIndex, millis() + P135_STOP_MEASUREMENT_DELAY);
        addLog(LOG_LEVEL_INFO, F("SCD4x: Factory reset success."));
      } else {
        addLog(LOG_LEVEL_ERROR, F("SCD4x: Factory reset failed!"));
      }
    }

    if (mustRunSelfTest) {
      success = scd4x->performSelfTest();

      if (success) {
        initialized = startPeriodicMeasurements(); // Select the correct periodic measurement, and start a READ
        Scheduler.schedule_task_device_timer(event->TaskIndex, millis() + P135_STOP_MEASUREMENT_DELAY);
        addLog(LOG_LEVEL_INFO, F("SCD4x: Sensor self-test success."));
      } else {
        addLog(LOG_LEVEL_ERROR, F("SCD4x: Sensor self-test failed!"));
      }
    }
    # endif // if P135_FEATURE_RESET_COMMANDS
  }
  return success;
}

/*****************************************************
* plugin_write
*****************************************************/
bool P135_data_struct::plugin_write(struct EventStruct *event,
                                    String            & string) {
  bool success = false;

  const String command = parseString(string, 1);

  if (command == F("scd4x")) {
    const String sub = parseString(string, 2);
    # if P135_FEATURE_RESET_COMMANDS
    const bool doSelftest = sub.equals(F("selftest"));
    # endif // if P135_FEATURE_RESET_COMMANDS

    // scd4x,storesettings : SLOW! Store current Altitude and temperatureCorrection in on-sensor EEPROM
    if (sub == F("storesettings")) {
      success = scd4x->persistSettings(); // This may take up to 800 mSec
    # if P135_FEATURE_RESET_COMMANDS

      // scd4x,factoryreset[,code] : SLOWER! Restore factory settings for sensor, code logged at ERROR level
      // scd4x,selftest[,code] : SLOWEST! Self-test for sensor, code logged at ERROR level
    } else if ((sub == F("factoryreset")) || doSelftest) {
      if (factoryResetCode.isEmpty()) {
        factoryResetCode = F("Scd4x");

        if (serialNumber[10] != 0x0) {
          if (doSelftest) {
            factoryResetCode += char(serialNumber[3]);
            factoryResetCode += char(serialNumber[1]);
            factoryResetCode += char(serialNumber[10]);
            factoryResetCode += char(serialNumber[6]);
          } else {
            factoryResetCode += char(serialNumber[1]);
            factoryResetCode += char(serialNumber[3]);
            factoryResetCode += char(serialNumber[7]);
            factoryResetCode += char(serialNumber[10]);
          }
        } else {
          factoryResetCode += F("2022");
        }

        if (doSelftest) {
          factoryResetCode += F("SelF");
        } else {
          factoryResetCode += F("reseT");
        }
        {
          String log = F("SCD4x: ");

          if (doSelftest) {
            log += F("Selftest code: ");
          } else {
            log += F("Factory reset code: ");
          }
          log += factoryResetCode;
          addLog(LOG_LEVEL_ERROR, log);
        }
        success = true;
      } else {
        String code = parseStringKeepCase(string, 3); // Case sensitive!

        if (code.equals(factoryResetCode)) {
          if (doSelftest) {
            addLog(LOG_LEVEL_ERROR, F("SCD4x: Selftest starting... (may take up to 11 seconds!)"));
          } else {
            addLog(LOG_LEVEL_ERROR, F("SCD4x: Factory reset starting... (may take up to 2.5 seconds!)"));
          }

          if (scd4x->stopPeriodicMeasurement()) {
            initialized = false;
            firstRead   = true; // To discard first measurement results

            if (doSelftest) {
              mustRunSelfTest = true;
            } else {
              mustRunFactoryReset = true;
            }
            Scheduler.schedule_task_device_timer(event->TaskIndex, millis() + P135_STOP_MEASUREMENT_DELAY);
            success = true;
          }
        }
        factoryResetCode.clear();
      }
    # endif // if P135_FEATURE_RESET_COMMANDS
    }
  }
  return success;
}

/*****************************************************
* plugin_get_config_value
*****************************************************/
bool P135_data_struct::plugin_get_config_value(struct EventStruct *event,
                                               String            & string) {
  bool success = false;

  const String var = parseString(string, 1);

  if (var == F("getaltitude")) {               // [<taskname>#getaltitude] = get sensor altitude
    string  = scd4x->getSensorAltitude();
    success = true;
  } else if (var == F("gettempoffset")) {      // [<taskname>#gettempoffset] = get sensor temperature offset
    string  = toString(scd4x->getTemperatureOffset(), 2);
    success = true;
  } else if (var == F("getdataready")) {       // [<taskname>#getdataready] = is data ready? (1/0)
    string  = scd4x->getDataReadyStatus();
    success = true;
  } else if (var == F("getselfcalibration")) { // [<taskname>#getselfcalibration] = is self-calibration enabled? (1/0)
    string  = scd4x->getAutomaticSelfCalibrationEnabled();
    success = true;
  }
  return success;
}

#endif // ifdef USES_P135
