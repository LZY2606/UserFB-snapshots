set(zhdrs
	zbuffer.h
	zfeatures.h
	)

install(FILES ${zhdrs} DESTINATION ${INCLUDE_DIR})

install(FILES GL/gl.h DESTINATION ${INCLUDE_DIR}/TGL)
install(FILES GL/splat.h DESTINATION ${INCLUDE_DIR}/TGL)
