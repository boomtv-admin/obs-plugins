#include <windows.h>
#include <obs-module.h>
#include <util/windows/win-version.h>
#include <util/platform.h>


OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("boom-capture", "en-US")


//extern struct obs_source_info vr_game_capture_info;
extern struct obs_source_info window_capture_info;
extern struct obs_source_info bcu_info;


extern bool cached_versions_match(void);
extern bool load_cached_graphics_offsets(bool is32bit);
extern bool load_graphics_offsets(bool is32bit);



/* temporary, will eventually be erased once we figure out how to create both
 * 32bit and 64bit versions of the helpers/hook */
#ifdef _WIN64
#define IS32BIT false
#else
#define IS32BIT true
#endif


#define USE_HOOK_ADDRESS_CACHE false




// Loading module
bool obs_module_load(void)
{
	struct win_version_info ver;
	bool win8_or_above = false;
	char *config_dir;

	config_dir = obs_module_config_path(NULL);
	if (config_dir) 
	{
		os_mkdirs(config_dir);
		bfree(config_dir);
	}

	get_win_ver(&ver);

	win8_or_above = ver.major > 6 || (ver.major == 6 && ver.minor >= 2);


	// Load BoomCapture2D
	/*obs_enter_graphics();
	obs_register_source(&window_capture_info);
	obs_leave_graphics();*/


	// Load BoomCapture
	/*if (USE_HOOK_ADDRESS_CACHE && cached_versions_match() && load_cached_graphics_offsets(IS32BIT)) 
	{
		load_cached_graphics_offsets(!IS32BIT);
		obs_register_source(&vr_game_capture_info);
	} 
	else if (load_graphics_offsets(IS32BIT))
	{
		load_graphics_offsets(!IS32BIT);
		obs_register_source(&vr_game_capture_info);
	}*/


	// Load BoomCaptureUniversal
	if (USE_HOOK_ADDRESS_CACHE && cached_versions_match() && load_cached_graphics_offsets(IS32BIT))
	{
		load_cached_graphics_offsets(!IS32BIT);
		obs_register_source(&bcu_info);
	}
	else if (load_graphics_offsets(IS32BIT))
	{
		load_graphics_offsets(!IS32BIT);
		obs_register_source(&bcu_info);
	}

	return true;
}
