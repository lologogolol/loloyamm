# Git-for-Windows bash spells D:/work as /d/work. Native mingw32-make does not understand that spelling, so normalize
# the source path in the few files written by FFmpeg's configure script before invoking make.
file(TO_CMAKE_PATH "${OPENYAMM_FFMPEG_NORMALIZE_SOURCE}" openyammFfmpegWindowsSource)
if (openyammFfmpegWindowsSource MATCHES "^([A-Za-z]):/(.*)$")
    string(TOLOWER "${CMAKE_MATCH_1}" openyammFfmpegDrive)
    set(openyammFfmpegMsysSource "/${openyammFfmpegDrive}/${CMAKE_MATCH_2}")
    foreach(openyammFfmpegFile IN ITEMS Makefile ffbuild/config.mak ffbuild/config.sh)
        set(openyammFfmpegPath "${OPENYAMM_FFMPEG_NORMALIZE_BUILD}/${openyammFfmpegFile}")
        if (EXISTS "${openyammFfmpegPath}")
            file(READ "${openyammFfmpegPath}" openyammFfmpegText)
            string(REPLACE "${openyammFfmpegMsysSource}" "${openyammFfmpegWindowsSource}"
                openyammFfmpegText "${openyammFfmpegText}")
            file(WRITE "${openyammFfmpegPath}" "${openyammFfmpegText}")
        endif()
    endforeach()
endif()
