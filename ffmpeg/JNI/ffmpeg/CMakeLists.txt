cmake_minimum_required(VERSION 3.4.1)
include_directories(./
                    ../openssl/include
                    ../libass/libass
                    ../libsmb2/include
                    ../zvbi/src
                    )
#AUX_SOURCE_DIRECTORY(./ SOURCES)
file(GLOB SOURCES1 ./libavcodec/*.c
                   ./libavcodec/*/*.c
                   ./libavdevice/*.c
                   ./libavfilter/*.c
                   ./libavformat/*.c
                   ./libavresample/*.c
                   ./libavresample/*/*.c
                   ./libavutil/*.c
                   ./libavutil/*/*.c
                   ./libpostproc/*.c
                   ./libswresample/*.c
                   ./libswresample/*/*.c
                   ./libswscale/*.c
                   ./libswscale/*/*.c)
add_library(
             ffmpeg.mx 
             SHARED
             ${SOURCES1}
             )

