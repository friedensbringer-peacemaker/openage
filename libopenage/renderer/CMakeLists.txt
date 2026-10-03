add_sources(libopenage
	color.cpp
    definitions.cpp
	frame_sink.cpp
	geometry.cpp
	render_factory.cpp
    render_pass.cpp
    render_target.cpp
    renderable.cpp
	renderer.cpp
	shader_program.cpp
	texture.cpp
	texture_array.cpp
	types.cpp
    uniform_buffer.cpp
	uniform_input.cpp
	util.cpp
	window.cpp
)

add_subdirectory(camera/)
add_subdirectory(resources/)
add_subdirectory(stages/)

# XR fork: Qt window events, GUI and the demos (they need the Qt GUI
# application) only with Qt; font rendering is optional
if(OPENAGE_QT)
	add_sources(libopenage
		window_event_handler.cpp
		window_events_qt.cpp
	)
	add_subdirectory(demo/)
	add_subdirectory(gui/)
endif()
if(OPENAGE_WITH_FONT)
	add_subdirectory(font/)
endif()

if(OPENGL_FOUND)
    add_subdirectory(opengl/)
endif()

if(VULKAN_FOUND)
    add_subdirectory(vulkan/)
endif()
