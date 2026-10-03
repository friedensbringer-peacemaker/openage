add_sources(libopenage
    cost_field.cpp
	definitions.cpp
	field_cache.cpp
    flow_field.cpp
	grid.cpp
	integration_field.cpp
    integrator.cpp
	path.cpp
	pathfinder.cpp
	portal.cpp
	sector.cpp
	tests.cpp
    types.cpp
)

# XR fork: the demos need the Qt GUI application
if(OPENAGE_QT)
	add_subdirectory("demo")
endif()
add_subdirectory("legacy")
