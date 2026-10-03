# XR fork: the demos need the Qt GUI application
if(OPENAGE_QT)
	add_sources(libopenage
		tests.cpp
	)

	add_subdirectory("demo")
endif()

pxdgen(
	tests.h
)
