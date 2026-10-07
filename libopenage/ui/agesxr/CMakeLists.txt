# XR fork: GL-free canvas, HUD and game menu of the XR layer (copied from the
# superproject by scripts/sync-ui-to-fork.sh, see PROVENANCE.md). Drawn into the
# engine image by renderer/stages/ui.
add_sources(libopenage
	stb_truetype_impl.cpp
	text_raster.cpp
	xr_canvas.cpp
	xr_game_ui.cpp
	xr_hud.cpp
)
# third party header, compiled without warnings
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/stb_truetype_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
