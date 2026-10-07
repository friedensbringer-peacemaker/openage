# XR fork: game user interface drawn into the engine image (HUD bar, context
# menu, game menu, match board), see ui/game_ui_controller.h
add_sources(libopenage
	game_ui_controller.cpp
)

add_subdirectory(agesxr/)
