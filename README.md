# NETINT Quadra FFmpeg README

> FFmpeg is a collection of libraries and tools to process multimedia content such as audio, video, subtitles and related metadata.

This is NETINT's fork of [FFmpeg](https://www.ffmpeg.org/) with custom codecs, filters, and modifications to get FFmpeg working with [NETINT Quadra Video Processing Units (VPU)](https://netint.com/technology/codensity-g5/).

The [NETINT Libxcoder](https://github.com/NETINT-Technologies/netint_libxcoder) software driver is a pre-requisite for this fork of FFmpeg.

## Build
### Dependencies
* NETINT `libxcoder`
* `pkgconfig`
* `nasm`

See `build_ffmpeg.sh -h` for dependencies of optional features.

### Scripted Build
```bash
bash build_ffmpeg.sh
```
See options using `bash build_ffmpeg.sh -h`

### Installation
```bash
sudo make install
sudo ldconfig
```

## Documentation

NETINT FFmpeg documentation is available at the [NETINT Docs Portal](https://docs.netint.com/quadra/)

Please also refer to the official FFmpeg documentation in the **doc/** directory.

Online documentation for official FFmpeg is available in its main [website](https://ffmpeg.org)
and [wiki](https://trac.ffmpeg.org).

## License

Following FFmpeg licensing convention, NETINT code in this project is LGPL version 2.1.

<!-- ## Forums

https://community.netint.ca/ -->