project(boom-capture)

set(boom-capture_HEADERS
	obfuscate.h
	hook-helpers.h
	inject-library.h
	cursor-capture.h
	graphics-hook-info.h
	window-helpers.h
	app-helpers.h
	cJSON.h
	dc-capture.h)

set(boom-capture_SOURCES
	obfuscate.c
	inject-library.c
	cursor-capture.c
	game-capture.c
	window-helpers.c
	app-helpers.c
	load-graphics-offsets.c
	plugin-main.c
	vr-window-capture.c
	boom-universal.c
	dc-capture.c
	cJSON.c)

add_library(boom-capture MODULE
	${boom-capture_SOURCES}
	${boom-capture_HEADERS})
target_link_libraries(boom-capture
	libobs
	ipc-util
	psapi)

install_obs_plugin_with_data(boom-capture data)

add_subdirectory(graphics-hook)
add_subdirectory(get-graphics-offsets)
add_subdirectory(inject-helper)
