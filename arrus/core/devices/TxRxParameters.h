#ifndef ARRUS_CORE_DEVICES_TXRXPARAMETERS_H
#define ARRUS_CORE_DEVICES_TXRXPARAMETERS_H

#include <gsl/gsl>
#include <utility>
#include <ostream>

#include "arrus/core/api/common/Interval.h"
#include "arrus/core/api/common/Tuple.h"
#include "arrus/core/api/common/types.h"
#include "arrus/common/format.h"
#include "arrus/core/common/collections.h"
#include "arrus/core/api/ops/us4r/Pulse.h"

namespace arrus::devices {

class TxRxParameters {
public:
    static const TxRxParameters US4OEM_NOP;

    static TxRxParameters createRxNOPCopy(const TxRxParameters& op) {
        return TxRxParameters(
            op.txAperture,
            op.txDelays,
            op.txPulse,
            BitMask(op.rxAperture.size(), false),
            op.rxSampleRange,
            op.rxDecimationFactor,
            op.pri,
            op.rxPadding,
            op.rxDelay,
            op.bitstreamId
        );
    }

    /**
     *
     * ** tx aperture, tx delays and rx aperture should have the same size
     * (tx delays is NOT limited to the tx aperture active elements -
     * the whole array must be provided).**
     *
     * @param txAperture
     * @param txDelays
     * @param txPulse
     * @param rxAperture
     * @param rxSampleRange [start, end) range of samples to acquire, starts from 0
     * @param rxDecimationFactor
     * @param pri
     * @param rxPadding how many 0-channels padd from the left and right
     */
    TxRxParameters(std::vector<bool> txAperture,
                   std::vector<float> txDelays,
                   const ops::us4r::Pulse &txPulse,
                   std::vector<bool> rxAperture,
                   Interval<uint32> rxSampleRange,
                   uint32 rxDecimationFactor, float pri,
                   Tuple<ChannelIdx> rxPadding = {0, 0},
                   float rxDelay = 0.0f,
                   std::optional<BitstreamId> bitstreamId = std::nullopt
                   )
        : txAperture(std::move(txAperture)), txDelays(std::move(txDelays)),
          txPulse(txPulse),
          rxAperture(std::move(rxAperture)), rxSampleRange(std::move(rxSampleRange)),
          rxDecimationFactor(rxDecimationFactor), pri(pri),
          rxPadding(std::move(rxPadding)), rxDelay(rxDelay), bitstreamId(bitstreamId) {}

    [[nodiscard]] const std::vector<bool> &getTxAperture() const {
        return txAperture;
    }

    [[nodiscard]] const std::vector<float> &getTxDelays() const {
        return txDelays;
    }

    [[nodiscard]] const ops::us4r::Pulse &getTxPulse() const {
        return txPulse;
    }

    [[nodiscard]] const std::vector<bool> &getRxAperture() const {
        return rxAperture;
    }

    [[nodiscard]] const Interval<uint32> &getRxSampleRange() const {
        return rxSampleRange;
    }

    [[nodiscard]] uint32 getNumberOfSamples() const {
        return rxSampleRange.end() - rxSampleRange.start();
    }

    [[nodiscard]] int32 getRxDecimationFactor() const {
        return rxDecimationFactor;
    }

    [[nodiscard]] float getPri() const {
        return pri;
    }

    [[nodiscard]] const Tuple<ChannelIdx> &getRxPadding() const {
        return rxPadding;
    }

    [[nodiscard]] bool isNOP() const  {
        auto atLeastOneTxActive = ::arrus::reduce(
            std::begin(txAperture),
            std::end(txAperture),
            false, [](auto a, auto b) {return a | b;});
        auto atLeastOneRxActive = ::arrus::reduce(
            std::begin(rxAperture),
            std::end(rxAperture),
            false, [](auto a, auto b) {return a | b;});
        return !atLeastOneTxActive && !atLeastOneRxActive;
    }

    [[nodiscard]] bool isRxNOP() const {
        auto atLeastOneRxActive = ::arrus::reduce(
            std::begin(rxAperture),
            std::end(rxAperture),
            false, [](auto a, auto b) {return a | b;});
        return !atLeastOneRxActive;
    }

    float getRxDelay() const { return rxDelay; }

    const std::optional<BitstreamId> &getBitstreamId() const { return bitstreamId; }

    // TODO(pjarosik) consider removing the below setter (keep this class immutable).
    void setRxDelay(float delay) { this->rxDelay = delay; }

    friend std::ostream &
    operator<<(std::ostream &os, const TxRxParameters &parameters) {
        os << "Tx/Rx: ";
        os << "TX: ";
        os << "aperture: " << ::arrus::toString(parameters.getTxAperture())
           << ", delays: " << ::arrus::toString(parameters.getTxDelays())
           << ", center frequency: " << parameters.getTxPulse().getCenterFrequency()
           << ", n. periods: " << parameters.getTxPulse().getNPeriods()
           << ", inverse: " << parameters.getTxPulse().isInverse();
        os << "; RX: ";
        os << "aperture: " << ::arrus::toString(parameters.getRxAperture());
        os << "sample range: " << parameters.getRxSampleRange().start() << ", "
           << parameters.getRxSampleRange().end();
        os << ", fs divider: " << parameters.getRxDecimationFactor()
           << ", padding: " << parameters.getRxPadding()[0] << ", " << parameters.getRxPadding()[1];
        os << ", rx delay: " << parameters.getRxDelay();
        if(parameters.getBitstreamId().has_value()) {
            os << ", bitstream id: " << parameters.getBitstreamId().value();
        }
        os << std::endl;
        return os;
    }

    bool operator==(const TxRxParameters &rhs) const {
        return txAperture == rhs.txAperture &&
               txDelays == rhs.txDelays &&
               txPulse == rhs.txPulse &&
               rxAperture == rhs.rxAperture &&
               rxSampleRange == rhs.rxSampleRange &&
               rxDecimationFactor == rhs.rxDecimationFactor &&
               pri == rhs.pri &&
               rxDelay == rhs.rxDelay &&
               bitstreamId == rhs.bitstreamId;
    }

    bool operator!=(const TxRxParameters &rhs) const {
        return !(rhs == *this);
    }
private:
    ::std::vector<bool> txAperture;
    ::std::vector<float> txDelays;
    ::arrus::ops::us4r::Pulse txPulse;
    ::std::vector<bool> rxAperture;
    // TODO change to a simple pair
    Interval<uint32> rxSampleRange;
    int32 rxDecimationFactor;
    float pri;
    Tuple<ChannelIdx> rxPadding;
    float rxDelay;
    std::optional<BitstreamId> bitstreamId;
};

class TxRxParametersBuilder {
public:

    explicit TxRxParametersBuilder(const TxRxParameters &params) {
        txAperture = params.getTxAperture();
        txDelays = params.getTxDelays();
        txPulse = params.getTxPulse();
        rxAperture = params.getRxAperture();
        rxSampleRange = params.getRxSampleRange();
        rxDecimationFactor = params.getRxDecimationFactor();
        pri = params.getPri();
        rxPadding = params.getRxPadding();
        rxDelay = params.getRxDelay();
        bitstreamId = params.getBitstreamId();
    }


    TxRxParameters build() {
        if(!txPulse.has_value()) {
            throw IllegalArgumentException("TX pulse definition is required");
        }
        return TxRxParameters(
            txAperture,
            txDelays,
            txPulse.value(),
            rxAperture,
            rxSampleRange,
            rxDecimationFactor,
            pri,
            rxPadding,
            rxDelay,
            bitstreamId
        );
    }

    void convertToNOP() {
        txAperture = BitMask(txAperture.size(), false);
        rxAperture = BitMask(rxAperture.size(), false);
        txDelays = getNTimes<float>(0.0f, txAperture.size());
    }

    void setTxAperture(const std::vector<bool> &value) { TxRxParametersBuilder::txAperture = value; }
    void setTxDelays(const std::vector<float> &value) { TxRxParametersBuilder::txDelays = value; }
    void setTxPulse(const std::optional<::arrus::ops::us4r::Pulse> &value) {
        TxRxParametersBuilder::txPulse = value;
    }
    void setRxAperture(const std::vector<bool> &value) { TxRxParametersBuilder::rxAperture = value; }
    void setRxSampleRange(const Interval<uint32> &value) {
        TxRxParametersBuilder::rxSampleRange = value;
    }
    void setRxDecimationFactor(int32 value) {
        TxRxParametersBuilder::rxDecimationFactor = value;
    }
    void setPri(float value) { TxRxParametersBuilder::pri = value; }
    void setRxPadding(const Tuple<ChannelIdx> &value) { TxRxParametersBuilder::rxPadding = value; }
    void setRxDelay(float value) { TxRxParametersBuilder::rxDelay = value; }
    void setBitstreamId(const std::optional<BitstreamId> &value) {
        TxRxParametersBuilder::bitstreamId = value;
    }

private:
    ::std::vector<bool> txAperture;
    ::std::vector<float> txDelays;
    std::optional<::arrus::ops::us4r::Pulse> txPulse;
    ::std::vector<bool> rxAperture;
    Interval<uint32> rxSampleRange;
    int32 rxDecimationFactor;
    float pri;
    Tuple<ChannelIdx> rxPadding;
    float rxDelay;
    std::optional<BitstreamId> bitstreamId;
};


using TxRxParamsSequence = std::vector<TxRxParameters>;

/**
 * Returns the number of actual ops, that is, a the number of ops excluding RxNOPs.
 */
uint16 getNumberOfNoRxNOPs(const TxRxParamsSequence &seq);

}

#endif //ARRUS_CORE_DEVICES_TXRXPARAMETERS_H
