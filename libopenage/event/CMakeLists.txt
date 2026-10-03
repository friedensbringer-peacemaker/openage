add_sources(libopenage
	event_loop.cpp
	event.cpp
	evententity.cpp
	eventhandler.cpp
	eventqueue.cpp
	eventstore.cpp
	state.cpp
	tests.cpp
)

# XR fork: the demos need the Qt GUI application
if(OPENAGE_QT)
	add_subdirectory("demo")
endif()
