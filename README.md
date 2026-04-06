# ESP32-P4 AVB Endpoint implementation

This repo contains the implementation of an AVB listener & talker endpoint based on an ESP32-P4.
The implementation is the result of a prototype that was developed during my masters thesis.
Compatability was tested with a MOTU 624 and a MAC mini (Late 2014) as well a MAC mini 2024 utilizing an Apple M4.

The implementation is build upon the esp-idf framework and
its [ptp example](https://github.com/espressif/esp-idf/tree/release/v5.5/examples/ethernet/ptp).
Furthermore, the prototype utilizes the gPTP (IEEE802.1AS) implementation of
this [PR](https://github.com/espressif/esp-idf/pull/18249), which is currently under code review.

# Dependencies

During my thesis I used audio capes that were designed and developed internally at the HAW Kiel.
However, the prototype will work with the onboard audio codec ES8133.

For further details feel free to deep dive into the thesis document.

## Setup

### ESP-IDF

To build the project, you need to have the ESP-IDF framework installed and set up on your machine. You can follow the
official ESP-IDF setup guide [here](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/get-started/index.html).

### Update gPTP requirements

The gPTP implementation currently requires a manually change to the esp-idf, in order to work properly.
Therefor, add the following line to `emac_hal_ptp_start` func in the `components/hal/emac_hal.c` file, as
explained [here](https://github.com/espressif/esp-idf/pull/15001#issuecomment-2527262246).

```c
emac_ll_ts_ptp_snap_type_sel(hal->ptp_regs, 1);
```

This is a workaround to ensure that the gPTP implementation can properly capture the timestamp of incoming packets,
which is crucial for accurate synchronization in AVB applications.

In
the [master](https://github.com/espressif/esp-idf/blob/c32c7152efe8df1a0411d9b11a1f0105e1eebfe1/components/esp_hal_emac/emac_hal.c#L372)
branch this is set per default.

### Choose the audio codec

The prototye is designed to work with different audio codes, however, the default configuration is set to work with the
onboard ES8133 codec.
If you want to use a different codec, you need to change the configuration via `idf.py menuconfig` under
`AVB Configuration`.