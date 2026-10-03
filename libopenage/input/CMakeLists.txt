add_sources(libopenage
    action.cpp
	event.cpp
	input_context.cpp
	input_manager.cpp
    text_to_event.cpp
)

# XR fork: the input demo needs a Qt window
if(OPENAGE_QT)
	add_sources(libopenage
		tests.cpp
	)
endif()

add_subdirectory("controller")
