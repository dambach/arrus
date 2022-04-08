#ifndef ARRUS_CORE_API_DEVICES_US4R_US4OEM_H
#define ARRUS_CORE_API_DEVICES_US4R_US4OEM_H

#include <memory>
#include "arrus/core/api/devices/Device.h"
#include "arrus/core/api/common/types.h"
#include "arrus/core/api/devices/TriggerGenerator.h"

namespace arrus::devices {

class Us4OEM : public Device, public TriggerGenerator {
public:
    using Handle = std::unique_ptr<Us4OEM>;
    using RawHandle = PtrHandle<Us4OEM>;

   /**
    * Us4OEM ADC test pattern state.
    */
    enum class RxTestPattern {
        OFF,
        /** Ramp (sawtooth data pattern). */
        RAMP,
    };

    ~Us4OEM() override = default;

    /**
     * Returns nominal sampling frequency on the us4OEM device.
     */
    virtual float getSamplingFrequency() = 0;

    /**
     * Returns temperature measured by Us4OEM's FPGA [Celsius].
     */
    virtual float getFPGATemperature() = 0;

	virtual uint16_t getAfe(uint8_t address) = 0;
	virtual void setAfe(uint8_t address, uint16_t value) = 0;
    //virtual void setAfeFir(uint8_t address, uint16_t* coeffs, uint8_t length) = 0;
    virtual void enableAfeDemod(void) = 0;
    virtual void disableAfeDemod(void) = 0;
    virtual void setAfeDemodDefault(void) = 0;
    virtual void setAfeDemodDecimationFactor(uint8_t integer) = 0;
    virtual void setAfeDemodDecimationFactor(uint8_t integer, uint8_t quarters) = 0;
    virtual void setAfeDemodFrequency(double startFrequency) = 0;
    virtual void setAfeDemodFrequency(double startFrequency, double stopFrequency) = 0;
    virtual double getAfeDemodStartFrequency(void) = 0;
    virtual double getAfeDemodStopFrequency(void) = 0;
    virtual void setAfeDemodFsweepROI(uint16_t startSample, uint16_t stopSample) = 0;
    virtual void writeAfeFIRCoeffs(int16_t* coeffs, uint16_t length) = 0;
    virtual void resetAfe(void) = 0;

    /**
     * Checks if the firmware version on the Us4OEM module is correct.
     *
     * @throws ::arrus::IllegalStateException when the incorrect version was detected.
     */
    virtual void checkFirmwareVersion() = 0;

    /**
     * Checks if the us4OEM is in the correct state (as seen by host PC).
     *
     * Note: currently only the firmware version is checked (to verify if the us4OEM module
     * memory space is still available for the us4OEM module).
     *
     * @throws arrus::IllegalStateException when the incorrect version was detected.
     */
    virtual void checkState() = 0;

    /**
     * Returns firmware version installed on the us4OEM module.
     */
    virtual uint32 getFirmwareVersion() = 0;

    /**
     * Returns Tx component firmware version installed on this us4OEM module.
     */
    virtual uint32 getTxFirmwareVersion() = 0;

    Us4OEM(Us4OEM const&) = delete;
    Us4OEM(Us4OEM const&&) = delete;
    void operator=(Us4OEM const&) = delete;
    void operator=(Us4OEM const&&) = delete;
protected:
    explicit Us4OEM(const DeviceId &id): Device(id) {}
};


}

#endif // ARRUS_CORE_API_DEVICES_US4R_US4OEM_H
