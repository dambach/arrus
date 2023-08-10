import abc
import queue

import numpy as np
import importlib
import importlib.util
import dataclasses

import arrus.core
import arrus.exceptions
import arrus.devices.us4r
import arrus.devices.file
import arrus.medium
import arrus.metadata
import arrus.params
import arrus.devices.cpu
import arrus.devices.gpu
import arrus.ops.us4r
import arrus.ops.imaging
import arrus.ops.tgc
import arrus.kernels.kernel
import arrus.utils
import arrus.utils.imaging
import arrus.framework
from typing import Sequence, Dict
from numbers import Number

from arrus.devices.ultrasound import Ultrasound


class AbstractSession(abc.ABC):
    """
    An abstract class of session.

    This class is not intended to be instantiated.
    """

    def __init__(self):
        pass

    @abc.abstractmethod
    def get_device(self, id: str):
        """
        Returns a device located at given path.

        :param id: a path to a device, for example '/Us4R:0'
        :return: a device located in a given path.
        """
        raise ValueError("Tried to access an abstract method.")


@dataclasses.dataclass(frozen=True)
class SessionContext:
    medium: arrus.medium.Medium


class Session(AbstractSession):
    """
    A communication session with the ultrasound system.

    Currently, only localhost session is available.

    This class is a context manager. All the processing to be done on the device
    on the devices should be done withing the session context.
    """

    def __init__(self, cfg_path: str = "us4r.prototxt",
                 medium: arrus.medium.Medium = None):
        """
        Session constructor.

        :param cfg_path: a path to configuration file
        :param medium: medium description to set in context
        """
        super().__init__()
        self._session_handle = arrus.core.createSessionSharedHandle(cfg_path)
        self._context = SessionContext(medium=medium)
        self._py_devices = self._create_py_devices()
        self._current_processing: arrus.utils.imaging.Processing = None

    def upload(self, scheme: arrus.ops.us4r.Scheme):
        """
        Uploads a given sequence on devices.

        :param scheme: scheme to upload
        :raises: ValueError when some of the input parameters are invalid
        :return: a data buffer and constant metadata
        """
        # Verify the input parameters.
        # Prepare sequence to load
        us_device: Ultrasound = self.get_device("/Ultrasound:0")
        us_device_dto = us_device.get_dto()
        medium = self._context.medium
        seq = scheme.tx_rx_sequence
        processing = scheme.processing

        kernel_context = self._create_kernel_context(
            seq,
            us_device_dto,
            medium,
            scheme.digital_down_conversion)
        raw_seq = arrus.kernels.get_kernel(type(seq))(kernel_context)

        actual_scheme = dataclasses.replace(scheme, tx_rx_sequence=raw_seq)
        core_scheme = arrus.utils.core.convert_to_core_scheme(actual_scheme)
        upload_result = self._session_handle.upload(core_scheme)

        us_device.set_kernel_context(kernel_context)
        data_description = us_device.get_data_description(upload_result, raw_seq)

        # Output buffer
        buffer_handle = arrus.core.getFifoLockFreeBuffer(upload_result)
        ###
        # -- Constant metadata
        # --- Frame acquisition context
        fac = self._create_frame_acquisition_context(
            seq, raw_seq, us_device_dto, medium)

        buffer = arrus.framework.DataBuffer(buffer_handle)
        input_shape = buffer.elements[0].data.shape

        is_iq_data = scheme.digital_down_conversion is not None
        const_metadata = arrus.metadata.ConstMetadata(
            context=fac, data_desc=data_description,
            input_shape=input_shape, is_iq_data=is_iq_data, dtype="int16",
            version=arrus.__version__
        )

        # numpy/cupy processing initialization
        if processing is not None:
            # setup processing
            import arrus.utils.imaging as _imaging

            if isinstance(processing, _imaging.Pipeline):
                # Wrap Pipeline into the Processing object.
                processing = _imaging.Processing(
                    pipeline=processing,
                    callback=None,
                    extract_metadata=False
                )
            if isinstance(processing, _imaging.Processing):
                processing = arrus.utils.imaging.ProcessingRunner(
                    buffer, const_metadata, processing)
                outputs = processing.outputs
            else:
                raise ValueError("Unsupported type of processing: "
                                 f"{type(processing)}")
            self._current_processing = processing
        else:
            # Device buffer and const_metadata
            outputs = buffer, const_metadata
        return outputs

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.stop_scheme()
        self.close()

    def start_scheme(self):
        """
        Starts the execution of the uploaded scheme.
        """
        arrus.core.arrusSessionStartScheme(self._session_handle)

    def stop_scheme(self):
        """
        Stops execution of the scheme.
        """
        arrus.core.arrusSessionStopScheme(self._session_handle)
        if self._current_processing is not None:
            self._current_processing.close()

    def run(self):
        """
        Runs the uploaded scheme.

        The behaviour of this method depends on the work mode:
        - MANUAL: triggers execution of batch of sequences only ONCE,
        - HOST, ASYNC: triggers execution of batch of sequences IN A LOOP (Host: trigger is on buffer element release).
         The run function can be called only once (before the scheme is stopped).
        """
        self._session_handle.run()

    def close(self):
        """
        Closes session.

        This method disconnects with all the devices available during this session.
        Sets the state of the session to closed, any subsequent call to the object
        methods (e.g. upload, startScheme..) will result in exception.
        """
        self.stop_scheme()
        self._session_handle.close()

    def get_device(self, path: str):
        """
        Returns a device identified by a given id.

        The available devices are determined by the initial session settings.

        The handle to device is invalid after the session is closed
        (i.e. the session object is disposed).

        :param path: a path to the device
        :return: a handle to device
        """
        # TODO use only device available in arrus core
        # First, try getting initially available devices.
        if path in self._py_devices:
            return self._py_devices[path]
        # Then try finding a device using arrus core implementation.

        device_handle = self._session_handle.getDevice(path)

        # Cast device to its type class.
        device_id = device_handle.getDeviceId()
        device_type = device_id.getDeviceType()
        specific_device_cast = {
            arrus.core.DeviceType_Us4R:
                lambda handle: arrus.devices.us4r.Us4R(
                    arrus.core.castToUs4r(handle)),
            arrus.core.DeviceType_File:
                lambda handle: arrus.devices.file.File(
                    arrus.core.castToFile(handle))
        }.get(device_type, None)
        if specific_device_cast is None:
            raise arrus.exceptions.DeviceNotFoundError(path)
        specific_device = specific_device_cast(device_handle)
        # TODO(pjarosik) key should be an id, not the whole path
        self._py_devices[path] = specific_device
        return specific_device

    def set_parameter(self, key: str, value: Sequence[Number]):
        """
        Sets the value for parameter with the given name.
        TODO: note: this method currently is not thread-safe
        """
        if self._current_processing is not None:
            return self._current_processing.pipeline.set_parameter(key, value)

    def get_parameter(self, key: str) -> Sequence[Number]:
        """
        Returns the current value for parameter with the given name.
        """
        if self._current_processing is not None:
            return self._current_processing.pipeline.get_parameter(key)

    def get_parameters(self) -> Dict[str, arrus.params.ParameterDef]:
        if self._current_processing is not None:
            return self._current_processing.pipeline.get_parameters()

    def get_session_context(self):
        return self._context

    @property
    def medium(self):
        """
        Returns currently set Medium.
        NOTE: this method is not thread-safe!
        """
        return self._context.medium

    @medium.setter
    def medium(self, value):
        """
        Sets a new medium in the current session context.
        NOTE: this method is not thread-safe!
        """
        self._context = SessionContext(medium=value)

    # def set_current_medium(self, medium: arrus.medium.Medium):
    #     # TODO mutex, forbid when context is frozen (e.g. when us4r is running)
    #     raise RuntimeError("NYI")

    def _create_py_devices(self):
        devices = {
            "/CPU:0": arrus.devices.cpu.CPU(0)
        }
        # Create CPU and GPU devices
        cupy_spec = importlib.util.find_spec("cupy")
        if cupy_spec is not None:
            import cupy
            cupy.cuda.device.Device(0).use()
            devices["/GPU:0"] = arrus.devices.gpu.GPU(0)
        return devices

    def _create_kernel_context(self, seq, device, medium, hardware_ddc):
        return arrus.kernels.kernel.KernelExecutionContext(
            device=device, medium=medium, op=seq, custom={},
            hardware_ddc=hardware_ddc
        )

    def _create_frame_acquisition_context(self, seq, raw_seq, device, medium):
        return arrus.metadata.FrameAcquisitionContext(
            device=device, sequence=seq, raw_sequence=raw_seq,
            medium=medium, custom_data={})

