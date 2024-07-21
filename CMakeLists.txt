project(v4l2-mpeg-to-http)

add_executable(${CMAKE_PROJECT_NAME} logging.h logging.c mjpeg_server.h mjpeg_server.c v4l2_client.h v4l2_client.c main.c)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

# show list
# https://trac.ffmpeg.org/wiki/Capture/Webcam
# ffmpeg -f v4l2 -list_formats all -i /dev/video0
