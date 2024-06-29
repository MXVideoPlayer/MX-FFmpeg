#  [![Activity](https://img.shields.io/github/commit-activity/m/MXVideoPlayer/MX-FFmpeg?label=commits&style=flat-square)](https://github.com/EpicMorg/docker/commits) [![GitHub issues](https://img.shields.io/github/issues/MXVideoPlayer/MX-FFmpeg.svg?style=popout-square)](https://github.com/EpicMorg/docker/issues) [![GitHub forks](https://img.shields.io/github/forks/MXVideoPlayer/MX-FFmpeg.svg?style=popout-square)](https://github.com/EpicMorg/docker/network) [![GitHub stars](https://img.shields.io/github/stars/MXVideoPlayer/MX-FFmpeg.svg?style=popout-square)](https://github.com/EpicMorg/docker/stargazers)  [![Size](https://img.shields.io/github/repo-size/MXVideoPlayer/MX-FFmpeg?label=size&style=flat-square)](https://github.com/EpicMorg/docker/archive/master.zip) [![Release](https://img.shields.io/github/v/release/MXVideoPlayer/MX-FFmpeg?style=flat-square)](https://github.com/EpicMorg/docker/releases)  [![Changelog](https://img.shields.io/badge/Changelog-yellow.svg?style=popout-square)](CHANGELOG.md)

## Description

To build all `ffmpeg library`, change directory to `ffmpeg/JNI` and run `rebuild-ffmpeg.sh`. 

To build individual `ffmpeg library`, change directory to `ffmpeg/JNI` and run `build-ffmpeg.sh {architecture name}`.

Visit [https://mx.j2inter.com/ffmpeg](https://mx.j2inter.com/ffmpeg) for more information

# 3rd party libs
* [dav1d](https://code.videolan.org/videolan/dav1d/) `0.6.0`
* [ffmpeg](https://github.com/FFmpeg/FFmpeg) `4.2` (git)
* [lame](https://lame.sourceforge.io/download.php) `3.100`
* [libmodplug](http://modplug-xmms.sf.net/) `0.8.8.5`
* [libsmb2](https://github.com/sahlberg/libsmb2) `3.0.0.0`
* [libxml2](https://gitlab.gnome.org/GNOME/libxml2) `2.5<=2.7`
* [openssl](https://github.com/openssl/openssl) `1.0.2s`
* [opus](https://opus-codec.org/downloads/) `1.1`
* [speex](https://www.speex.org/downloads/) `1.2rc1`
* [zvbi](https://sourceforge.net/projects/zapping/files/zvbi/) `0.2.35`

# How to

## Building Custom FFmpeg library for MX Player:

### Prerequisites:
* `Linux` or `Cygwin` or `Mac`.
* `Android NDK` (`r20` for `v1.21.0`)
* `FFmpeg` source codes (can be downloaded from the download page)

### Build Instructions:
* Extract the downloaded `FFmpeg sources`
* Open `ffmpeg` -> `JNI` folder
* ~~Change `CPU_CORE` In `build.sh` and `build-ffmpeg.sh` to match CPU core number of your building machine.~~
  * At now, `CPU_CORE` counts automaticly via `ENV` file.
* If you would like to use your own `FFmpeg` build configuration, configs can be changed by editing `config-ffmpeg.sh`.
* Open `Terminal`.
* Change the working directory to `ffmpeg/JNI` directory.
* Create an environment variable `NDK` to point to your `NDK path` (e.g. `export NDK=/usr/src/android-ndk-r20b`)
* Run `rebuild-ffmpeg.sh` to build custom codec for all supported architectures
* If you would like to build custom codec for a particular architecture, then run both `build-openssl.sh` and `build-ffmpeg.sh` with one of the following arguments.
  * `arm64`
  * `neon`
  * `tegra3`
  * `tegra2`
  * `x86`
  * `x86_64`
* On successful completion of the process, `libffmpeg.mx.so` file will be generated on relevant subdirectories under the libs folder (e.g. `ffmpeg/JNI/libs/arm64-v8a`).

### Shipping custom codec:
Since `1.7.29`, custom codec file naming convention is changed to the following format:

`libffmpeg.mx.so.<cpu-architecture-code>.<version-number>`

CPU architecture code is one of the followings:

* `neon64`
* `neon`
* `tegra3`
* `tegra2`
* `x86`
* `x86_64`

### Additional Notes:
1. The version number will be similar to MX Player (e.g. `1.7.29`, `1.7.30`). However, sometimes `MX Player` will be upgraded without upgrading codec. So, Custom Codec version may `not` be exactly the same as the version of MX Player.

2. If the codec is compressed in a .zip container, it's no longer required to be decompressed manually. MX Player itself extract codec file from .zip file when loading codec. Also, multiple codec files can be compressed on a single .zip file. MX Player loads correct codec from the file automatically.

3. MX Player will recognize zipped codec files on SDCard root and Download directory automatically if the filename contains version code.

* `*` This build instruction is applicable after version `1.13.0`
* `*` From version `1.7.6`, file names are changed to `libavutil.mx.so`, `libavformat.mx.so`.. so forth to resolve conflict with system library files.
* `*` From version `1.7.16`, it generates single file; `libffmpeg.mx.so`.
* `*` neon64 and x86_64 are supported since version `1.13.0`
