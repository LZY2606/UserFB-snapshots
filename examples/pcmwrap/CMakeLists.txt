add_library(pcmwrap
    pcm_frame.c
    pcm_wrap.c
    pcm_wrap_mxf.c
)

target_include_directories(pcmwrap PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(pcmwrap PUBLIC
    MXF
)

set_target_properties(pcmwrap PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

include("${PROJECT_SOURCE_DIR}/cmake/source_filename.cmake")
set_source_filename(pcmwrap "${CMAKE_CURRENT_LIST_DIR}" "libMXF")


add_executable(pcmwrap-bin
    main.c
)

target_include_directories(pcmwrap-bin PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(pcmwrap-bin
    pcmwrap
    MXF
)

set_target_properties(pcmwrap-bin PROPERTIES
    OUTPUT_NAME pcmwrap
)

include("${PROJECT_SOURCE_DIR}/cmake/source_filename.cmake")
set_source_filename(pcmwrap-bin "${CMAKE_CURRENT_LIST_DIR}" "libMXF")
