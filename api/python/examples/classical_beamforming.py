"""
This script acquires and reconstructs RF img for classical imaging scheme
(scanline by scanline).

GPU usage is recommended.
"""
#not working  
#RuntimeError: 128 different rx mappings can be loaded only, deviceId: Us4OEM:0.

import numpy as np
import arrus
from arrus.ops.us4r import Scheme, Pulse
from arrus.ops.imaging import LinSequence
from arrus.utils.gui import Display2D
from arrus.utils.imaging import get_bmode_imaging, get_extent, Pipeline, SelectFrames, Squeeze, Lambda, RemapToLogicalOrder


arrus.set_clog_level(arrus.logging.INFO)
arrus.add_log_file("test.log", arrus.logging.INFO)

# Here starts communication with the device.
with arrus.Session("us4r_L3-9i-D.prototxt") as sess:
    us4r = sess.get_device("/Us4R:0")
    us4r.set_hv_voltage(20)
    n_elements = us4r.get_probe_model().n_elements

    sequence = LinSequence(
        # tx_aperture_center_element=np.arange(0, n_elements,step=2),
        tx_aperture_center_element=np.arange(0, 4,step=2),
        tx_aperture_size=64,
        tx_focus=20e-3,
        pulse=Pulse(center_frequency=6e6, n_periods=2, inverse=False),

        rx_aperture_center_element=np.arange(0, 4,step=2),
        # rx_aperture_center_element=64,
        rx_aperture_size=64,
        rx_sample_range=(0, 4096), #profondeur d'imagerie en échantillons
        downsampling_factor=2,
        pri=200e-6,
        tgc_start=14,
        tgc_slope=2e2,
        speed_of_sound=1540)

    # Imaging output grid.
    x_grid = np.arange(-20, 20, 0.1) * 1e-3
    z_grid = np.arange(5, 40, 0.1) * 1e-3

    scheme = Scheme(
        tx_rx_sequence=sequence,
        processing=get_bmode_imaging(sequence=sequence, grid=(x_grid, z_grid))
    )

    # Upload sequence on the us4r-lite device.
    buffer, metadata = sess.upload(scheme)
    
    print("Metadata:", metadata)
    buffer_element = buffer
    
    display = Display2D(metadata=metadata, value_range=(20, 80), cmap="gray",
                        title="B-mode", xlabel="OX (mm)", ylabel="OZ (mm)",
                        extent=get_extent(x_grid, z_grid)*1e3,
                        show_colorbar=True)
    sess.start_scheme()
    display.start(buffer)

# When we exit the above scope, the session and scheme is properly closed.
print("Stopping the example.")