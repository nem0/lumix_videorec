nvEncodeAPI.h is from FFmpeg nv-codec-headers, tag n12.1.14.0 (NVIDIA Video Codec SDK 12.0.16 headers),
https://github.com/FFmpeg/nv-codec-headers, MIT license (see the header). It is only needed to build
the videorec plugin, the library itself (nvEncodeAPI64.dll) is loaded at runtime from the NVIDIA driver.
