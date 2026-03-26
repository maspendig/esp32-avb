# ESP32-P4 AVB Endpoint implementation

This repo contains the implementation of an AVB listener & talker endpoint based on an ESP32-P4.
The implementation is the result of a prototype that was developed during my masters thesis.
Compatability was tested with a MOTU 624 and a MAC mini (Late 2014) as well a MAC mini 2024 utilizing an Apple M4.

The implementation is build upon the esp-idf framework and its [ptp example](https://github.com/espressif/esp-idf/tree/release/v5.5/examples/ethernet/ptp).
Furthermore, the prototype utilizes the gPTP (IEEE802.1AS) implementation of this [PR](https://github.com/espressif/esp-idf/pull/18249), which is currently under code review. 

# Dependencies
During my thesis i used audio capes that were designed and developed internally at the HAW Kiel.
However the prototype will work with the onboard audio codec ES8133.

For further details feel free to deep dive into the thesis document.
