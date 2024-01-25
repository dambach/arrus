#ifndef ARRUS_CORE_DEVICES_US4R_US4ROUTPUTBUFFER_H
#define ARRUS_CORE_DEVICES_US4R_US4ROUTPUTBUFFER_H

#include "Us4RBuffer.h"

#include <chrono>
#include <condition_variable>
#include <gsl/span>
#include <iostream>
#include <mutex>

#include "arrus/common/asserts.h"
#include "arrus/common/format.h"
#include "arrus/core/api/common/exceptions.h"
#include "arrus/core/api/common/types.h"
#include "arrus/core/api/framework/DataBuffer.h"
#include "arrus/core/common/logging.h"

namespace arrus::devices {

using ::arrus::framework::Buffer;
using ::arrus::framework::BufferElement;

class Us4ROutputBuffer;

/**
 * This class defines the layout of each output array.
 */
class Us4ROutputBufferArrayDef {
public:
    Us4ROutputBufferArrayDef(framework::NdArrayDef definition, size_t address, std::vector<size_t> oemSizes)
        : definition(std::move(definition)), address(address), oemSizes(std::move(oemSizes)) {}

    size_t getAddress() const { return address; }
    const framework::NdArrayDef &getDefinition() const { return definition; }
    size_t getSize() { return definition.getSize(); }
    /*** Returns address of data produced by the given OEM, relative to the beginning of the element. */
    size_t getOEMAddress(Ordinal oem) { return address + oemAddresses[oem]; }
    /** Returns the size of this array data produced by the given OEM */
    size_t getOEMSize(Ordinal oem) {
        ARRUS_REQUIRES_TRUE(oem < oemSizes.size(), "OEM outside of range");
        return oemSizes.at(oem);
    }

private:
    framework::NdArrayDef definition;
    /** Array address, relative to the beginning of the parent element */
    size_t address;
    std::vector<size_t> oemSizes;
    /** The part of array the given OEM, relative to the beginning of the array. */
    std::vector<size_t> oemAddresses;
};

/**
 * Buffer element owns the data arrrays, which then are returned to user.
 */
class Us4ROutputBufferElement : public BufferElement {
public:
    using Accumulator = uint16;
    using SharedHandle = std::shared_ptr<Us4ROutputBufferElement>;

    Us4ROutputBufferElement(size_t position, Tuple<framework::NdArray> arrays, Accumulator filledAccumulator)
        : position(position), arrays(arrays), filledAccumulator(filledAccumulator) {}

    void release() override {
        std::unique_lock<std::mutex> guard(mutex);
        this->accumulator = 0;
        releaseFunction();
        this->state = State::FREE;
    }

    int16 *getAddress(ArrayId id) {
        validateState();
        return arrays.getMutable(id).get<int16>();
    }

    /** TODO Deprecated, use getAddress(arrayId) */
    int16 *getAddress() { return getAddress(0); }

    /**
     * This method allows to read element's address regardless of it's state.
     * This method can be used e.g. in a clean-up procedures, that may
     * be called even after some buffer overflow.
     * TODO deprecated, use getAddressUnsafe(arrayId)
     */
    int16 *getAddressUnsafe(ArrayId id) { return arrays.getMutable(id).get<int16>(); }

    int16 *getAddressUnsafe() { return getAddressUnsafe(0); }

    framework::NdArray &getData(ArrayId id) override {
        validateState();
        return arrays.getMutable(id);
    }

    framework::NdArray &getData() override { return getData(0); }

    size_t getSize() override { return size; }

    size_t getPosition() override { return position; }

    void registerReleaseFunction(std::function<void()> &f) { releaseFunction = f; }

    [[nodiscard]] bool isElementReady() {
        std::unique_lock<std::mutex> guard(mutex);
        return state == State::READY;
    }

    void signal(Ordinal n) {
        std::unique_lock<std::mutex> guard(mutex);
        Accumulator us4oemPattern = 1ul << n;
        if ((accumulator & us4oemPattern) != 0) {
            throw IllegalStateException("Detected data overflow, buffer is in invalid state.");
        }
        accumulator |= us4oemPattern;
        if (accumulator == filledAccumulator) {
            this->state = State::READY;
        }
    }

    void resetState() {
        accumulator = 0;
        this->state = State::FREE;
    }

    void markAsInvalid() { this->state = State::INVALID; }

    void validateState() const {
        if (getState() == State::INVALID) {
            throw IllegalStateException(
                "The buffer is in invalid state (probably some data transfer overflow happened).");
        }
    }

    [[nodiscard]] State getState() const override { return this->state; }

private:
    std::mutex mutex;
    size_t position;
    Tuple<framework::NdArray> arrays;
    /** A pattern of the filled accumulator, which indicates that the hole element is ready. */
    Accumulator filledAccumulator;
    /** Size of the whole element (i.e. the sum of all arrays). */
    size_t size;
    // How many times given element was signaled by i-th us4OEM.
    std::vector<int> signalCounter;
    // How many times given element should be signaled by i-th us4OEM, to consider it as ready.
    std::vector<int> elementReadyCounters;
    Accumulator accumulator;
    std::function<void()> releaseFunction;
    State state{State::FREE};
};

/**
 * Us4R system's output circular FIFO buffer.
 *
 * The buffer has the following relationships:
 * - buffer contains **elements**
 * - the **element** is filled by many us4oems
 *
 * A single element is the output of a single data transfer (the result of running a complete sequence once).
 *
 * The state of each buffer element is determined by the field accumulators:
 * - accumulators[element] == 0 means that the buffer element was processed and is ready for new data from the producer.
 * - accumulators[element] > 0 && accumulators[element] != filledAccumulator means that the buffer element is partially
 *   confirmed by some of us4oems
 * - accumulators[element] == filledAccumulator means that the buffer element is ready to be processed by a consumer.
 *
 * The assumption is here that each element of the buffer has the same size (and the same us4oem offsets).
 */
class Us4ROutputBuffer : public framework::DataBuffer {
public:
    static constexpr size_t ALIGNMENT = 4096;
    using DataType = int16;
    using Accumulator = Us4ROutputBufferElement::Accumulator;

    /**
     * Buffer's constructor.
     *
     * @param noems: the total number of OEMs, regardless of whether that OEM produces data or not
     *
     */
    Us4ROutputBuffer(const Tuple<Us4ROutputBufferArrayDef> &arrays, const unsigned nElements, bool stopOnOverflow,
                     size_t noems)
        : elementSize(0), stopOnOverflow(stopOnOverflow) {

        ARRUS_REQUIRES_TRUE(noems <= 16, "Currently Us4R data buffer supports up to 16 OEMs.");

        Accumulator elementReadyPattern = createElementReadyPattern(arrays, noems);
        size_t elementSize = calculateElementSize(arrays);
        try {
            size_t totalSize = elementSize * nElements;
            getDefaultLogger()->log(
                LogSeverity::DEBUG,
                format("Allocating {} ({}, {}) bytes of memory", totalSize, elementSize, nElements));
            dataBuffer = reinterpret_cast<DataType *>(operator new[](totalSize, std::align_val_t(ALIGNMENT)));
            getDefaultLogger()->log(LogSeverity::DEBUG, format("Allocated address: {}", (size_t) dataBuffer));
            elements = createElements(dataBuffer, arrays, elementReadyPattern, nElements, elementSize);
        } catch (...) {
            ::operator delete[](dataBuffer, std::align_val_t(ALIGNMENT));
            getDefaultLogger()->log(LogSeverity::DEBUG, "Released the output buffer.");
        }
        this->initialize();
    }

    ~Us4ROutputBuffer() override {
        ::operator delete[](dataBuffer, std::align_val_t(ALIGNMENT));
        getDefaultLogger()->log(LogSeverity::DEBUG, "Released the output buffer.");
    }

    void registerOnNewDataCallback(framework::OnNewDataCallback &callback) override {
        this->onNewDataCallback = callback;
    }

    [[nodiscard]] const framework::OnNewDataCallback &getOnNewDataCallback() const { return this->onNewDataCallback; }

    void registerOnOverflowCallback(framework::OnOverflowCallback &callback) override {
        this->onOverflowCallback = callback;
    }

    void registerShutdownCallback(framework::OnShutdownCallback &callback) override {
        this->onShutdownCallback = callback;
    }

    [[nodiscard]] size_t getNumberOfElements() const override { return elements.size(); }

    BufferElement::SharedHandle getElement(size_t i) override {
        return std::static_pointer_cast<BufferElement>(elements[i]);
    }

    /**
     * Return address (beginning) of the given buffer element.
     */
    uint8 *getAddress(uint16 bufferElementId) {
        return reinterpret_cast<uint8 *>(this->elements[bufferElementId]->getAddress());
    }

    uint8 *getAddressUnsafe(uint16 elementNumber) {
        return reinterpret_cast<uint8 *>(this->elements[elementNumber]->getAddressUnsafe());
    }

    /**
     * Returns a total size of the buffer, the number of bytes.
     */
    [[nodiscard]] size_t getElementSize() const override { return elementSize; }

    /**
     * Signals the readiness of new data acquired by the n-th Us4OEM module.
     *
     * This function should be called by us4oem interrupt callbacks.
     *
     * @param n us4oem ordinal number
     *
     *  @return true if the buffer signal was successful, false otherwise (e.g. the queue was shut down).
     */
    bool signal(Ordinal n, uint16 elementNr) {
        std::unique_lock<std::mutex> guard(mutex);
        if (this->state != State::RUNNING) {
            getDefaultLogger()->log(LogSeverity::DEBUG, "Signal queue shutdown.");
            return false;
        }
        this->validateState();
        auto &element = this->elements[elementNr];
        try {
            element->signal(n);
        } catch (const IllegalArgumentException &e) {
            this->markAsInvalid();
            throw e;
        }
        if (element->isElementReady()) {
            guard.unlock();
            onNewDataCallback(elements[elementNr]);
        } else {
            guard.unlock();
        }
        return true;
    }

    void markAsInvalid() {
        std::unique_lock<std::mutex> guard(mutex);
        if (this->state != State::INVALID) {
            this->state = State::INVALID;
            for (auto &element : elements) {
                element->markAsInvalid();
            }
            this->onOverflowCallback();
        }
    }

    void shutdown() {
        std::unique_lock<std::mutex> guard(mutex);
        this->onShutdownCallback();
        this->state = State::SHUTDOWN;
        guard.unlock();
    }

    void resetState() {
        this->state = State::INVALID;
        this->initialize();
        this->state = State::RUNNING;
    }

    void initialize() {
        for (auto &element : elements) {
            element->resetState();
        }
    }

    void registerReleaseFunction(size_t element, std::function<void()> &releaseFunction) {
        this->elements[element]->registerReleaseFunction(releaseFunction);
    }

    bool isStopOnOverflow() { return this->stopOnOverflow; }

    size_t getNumberOfElementsInState(BufferElement::State s) const override {
        size_t result = 0;
        for (size_t i = 0; i < getNumberOfElements(); ++i) {
            if (elements[i]->getState() == s) {
                ++result;
            }
        }
        return result;
    }
    /**
     * Returns relatative address of the element area dedicated for the given array, given OEM.
     * The addres is relative to the beginning of the whole element (i.e. array 0, oem 0, where
     * 0 is the first non-empty array).
     */
    size_t getArrayAddressRelative(uint16 arrayId, Ordinal oem) const {}

private:
    /**
     * Throws IllegalStateException when the buffer is in invalid state.
     *
     * @return true if the queue execution should continue, false otherwise.
     */
    void validateState() {
        if (this->state == State::INVALID) {
            throw ::arrus::IllegalStateException("The buffer is in invalid state "
                                                 "(probably some data transfer overflow happened).");
        } else if (this->state == State::SHUTDOWN) {
            throw ::arrus::IllegalStateException("The data buffer has been turned off.");
        }
    }

    /**
     * Creates the expected value of the pattern when all the data was properly transferred to this buffer.
     */
    static Accumulator createElementReadyPattern(const Tuple<Us4ROutputBufferArrayDef> &arrays, size_t noems) {
        // accumulator for each array
        std::vector<Accumulator> accumulators;
        for (auto &array : arrays) {
            Accumulator accumulator((1ul << noems) - 1);
            for (size_t oem = 0; oem < noems; ++oem) {
                if (array.getOEMSize(oem) == 0) {
                    accumulator &= ~(1ul << oem);
                }
            }
            accumulators.push_back(accumulator);
        }
        // OEM is active when at least array is produced by this OEM.
        Accumulator result = 0;
        for (const auto &a : accumulators) {
            result = result | a;
        }
        return result;
    }

    /**
     * Returns the size of the whole element, i.e. the sum of the sizes of all arrays (the number of bytes).
     */
    static size_t calculateElementSize(const Tuple<Us4ROutputBufferArrayDef> &arrays) {
        size_t result = 0;
        for (auto &array : arrays) {
            result += array.getSize();
        }
        return result;
    }

    std::vector<Us4ROutputBufferElement::SharedHandle> createElements(int16 *baseAddress,
                                                                      const Tuple<Us4ROutputBufferArrayDef> &arrayDefs,
                                                                      uint16 elementReadyPattern, unsigned nElements,
                                                                      size_t elementSize) {
        for (unsigned i = 0; i < nElements; ++i) {
            std::vector<framework::NdArray> arraysVector;
            for (const Us4ROutputBufferArrayDef &arrayDef : arrayDefs) {
                size_t elementOffset = i * elementSize;
                size_t arrayOffset = elementOffset + arrayDef.getAddress();
                auto arrayAddress = reinterpret_cast<DataType *>(reinterpret_cast<int8 *>(dataBuffer) + arrayOffset);
                auto def = arrayDef.getDefinition();
                DeviceId deviceId(DeviceType::Us4R, 0);
                framework::NdArray array{arrayAddress, def.getShape(), def.getDataType(), deviceId};
                arraysVector.emplace_back(std::move(array));
            }
            Tuple<framework::NdArray> arrays = Tuple<framework::NdArray>{arraysVector};
            elements.push_back(std::make_shared<Us4ROutputBufferElement>(i, arrays, elementReadyPattern));
        }
    }

    std::mutex mutex;
    /** A size of a single element IN number of BYTES. */
    size_t elementSize;
    /**  Total size in the number of elements. */
    int16 *dataBuffer;
    /** Host buffer elements */
    std::vector<Us4ROutputBufferElement::SharedHandle> elements;
    /** Array offsets, in bytes. The is an offset relative to the beginning of each element. */
    std::vector<size_t> arrayOffsets;
    /** OEM data offset, relative to the beginning of array, in bytes. */
    std::vector<size_t> arrayOEMOffsets;
    // Callback that should be called once new data arrive.
    framework::OnNewDataCallback onNewDataCallback;
    framework::OnOverflowCallback onOverflowCallback{[]() {}};
    framework::OnShutdownCallback onShutdownCallback{[]() {}};

    // State management
    enum class State { RUNNING, SHUTDOWN, INVALID };
    State state{State::RUNNING};
    bool stopOnOverflow{true};
};

class Us4ROutputBufferBuilder {
public:
    void setNumberOfElements(unsigned value) { nElements = value; }

    void setStopOnOverflow(bool value) { stopOnOverflow = value; }

    void setLayout(const Us4RBuffer &src) {
        std::vector<Us4ROutputBufferArrayDef> result;
        // Calculate shape of each array.
        std::vector<framework::NdArrayDef> arrayDefs = getArrayDefs(src);

        // Array -> OEM -> size
        std::vector<std::vector<size_t>> oemSizes;

        for (const auto &arrayDef : src.getArrayDefs().getValues()) {
            framework::NdArrayDef def = arrayDef.getDefinition();
            size_t adddress = arrayDef.getAddress();
            std::vector<size_t> oemSizes = arrayDef.getOEMSizes();
            defs.emplace_back(def, adddress, oemSizes);
        }
        arrayDefs = Tuple<Us4ROutputBufferArrayDef>{result};
    }

    Us4ROutputBuffer::Handle build() {
        return std::make_unique<Us4ROutputBuffer>(arrayDefs, nElements, stopOnOverflow, noems);
    }

private:
    std::vector<framework::NdArrayDef> getArrayDefs(const Us4RBuffer &src) {
        std::vector<framework::NdArrayDef::Shape> shapes;

        for (Ordinal oem = 0; oem < (Ordinal) (src.getNumberOfOEMs()); ++oem) {
            const auto &oemBuffer = src.getUs4OEMBuffer(oem);
            // For each OEM array
            std::vector<size_t> shapeInternal = this->elements[0].getElementShape().getValues();
            // It's always the last axis, regardless IQ vs RF data.
            size_t channelAxis = shapeInternal.size() - 1;

            auto nChannels = static_cast<unsigned>(shapeInternal[channelAxis]);
            unsigned nSamples = 0;
            framework::NdArray::DataType dataType = this->elements[0].getDataType();

            // Sum buffer us4oem component number of samples to determine buffer element shape.
            for (auto &component : this->elements) {
                auto &componentShape = component.getElementShape();
                // Verify if we have the same number of channels for each component
                if (nChannels != componentShape.get(channelAxis)) {
                    throw IllegalArgumentException(
                        "Each us4OEM buffer element should have the same number of channels.");
                }
                if (dataType != component.getDataType()) {
                    throw IllegalArgumentException(
                        "Each us4OEM buffer element component should have the same data type.");
                }
                nSamples += static_cast<unsigned>(componentShape.get(0));
            }
            shapeInternal[0] = nSamples;
            // Possibly another dimension: 2 (DDC I/Q)
            shapeInternal[channelAxis] = nChannels;
            elementShape = framework::NdArray::Shape{shapeInternal};
            elementDataType = dataType;
        }
    }

    Tuple<Us4ROutputBufferArrayDef> arrayDefs;
    unsigned noems{0};
    unsigned nElements{0};
    bool stopOnOverflow{false};
};

}// namespace arrus::devices

#endif//ARRUS_CORE_DEVICES_US4R_US4ROUTPUTBUFFER_H
