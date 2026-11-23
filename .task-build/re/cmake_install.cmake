# Install script for directory: /Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/libre.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libre.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libre.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libre.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/re" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_aes.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_async.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_atomic.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_av1.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_base64.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_bfcp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_btrace.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_conf.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_convert.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_crc32.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_dbg.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_dd.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_dns.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_fmt.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_h264.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_h265.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_hash.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_hmac.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_http.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_httpauth.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_ice.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_json.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_list.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_main.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_mbuf.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_md5.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_mem.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_mod.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_mqueue.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_msg.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_net.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_odict.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_pcp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_pcm.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_rtmp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_rtp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_rtpext.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sa.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sdp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sha.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_shim.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sip.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sipevent.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sipreg.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sipsess.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_srtp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_stun.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_sys.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_tcp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_telev.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_thread.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_tls.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_tmr.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_trace.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_trice.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_turn.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_types.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_udp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_unixsock.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_uri.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/re_websock.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_aac.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_aubuf.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_auconv.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_audio.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_aufile.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_auframe.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_au.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_aulevel.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_aumix.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_auresamp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_autone.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_avc.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_dsp.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_dtmf.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_fir.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_flv.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_g711.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_goertzel.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_vidconv.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_video.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_vid.h"
    "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/include/rem_vidmix.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libre/libre-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libre/libre-targets.cmake"
         "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/CMakeFiles/Export/3fd2fa51af73f390879d8cf3a8a24fb5/libre-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libre/libre-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libre/libre-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libre" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/CMakeFiles/Export/3fd2fa51af73f390879d8cf3a8a24fb5/libre-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libre" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/CMakeFiles/Export/3fd2fa51af73f390879d8cf3a8a24fb5/libre-targets-debug.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/re" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/cmake/re-config.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libre" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/cmake/libre-config.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/libre.pc")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/packaging/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z02499/attempts/20261123T151052765424Z/repo/.task-build/re/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
