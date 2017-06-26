#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include <obs-module.h>
#include <obs-internal.h>
#include <obs-scene.h>
#include <obs-data.h>

#include <util/platform.h>
#include <util/dstr.h>
#include <windows.h>
#include <dxgi.h>
#include <emmintrin.h>
#include <ipc-util/pipe.h>

#include "obfuscate.h"
#include "inject-library.h"
#include "graphics-hook-info.h"
#include "window-helpers.h"
#include "app-helpers.h"
#include "cursor-capture.h"
#include "dc-capture.h"
#include "cJSON.h"
#include "nt-stuff.h"



//===============================================  Defines  ===============================================

#define do_log(level, format, ...) blog(level, "[boom-capture-univ: '%s'] " format, obs_source_get_name(gc->source), ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define SETTING_COMPATIBILITY    "sli_compatibility"
#define SETTING_CURSOR           "capture_cursor"
#define SETTING_TRANSPARENCY     "allow_transparency"
#define SETTING_LIMIT_FRAMERATE  "limit_framerate"
#define SETTING_BORDER_COLOR	 "border_color"
#define SETTING_BORDER_THICKNESS "border_thickness"

#define TEXT_BCU				 obs_module_text("Boom Replay")
#define TEXT_SLI_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_ALLOW_TRANSPARENCY  obs_module_text("AllowTransparency")
#define TEXT_CAPTURE_CURSOR      obs_module_text("CaptureCursor")
#define TEXT_LIMIT_FRAMERATE     obs_module_text("LimitFramerate")
#define TEXT_BORDER_COLOR		 obs_module_text("Border Color")
#define TEXT_BORDER_THICKNESS	 obs_module_text("Border Thickness")

#define DEFAULT_RETRY_INTERVAL 0.25f
#define ERROR_RETRY_INTERVAL 0.5f
#define RESIZE_CHECK_TIME 0.2f

#define DEFAULT_BORDER_COLOR		0xFF666666
#define DEFAULT_BORDER_THICKNESS	10







//===============================================  Structures  ===============================================

struct bcu_config {
	char                          *title_first;
	char                          *title_second;
	bool                          cursor : 1;
	bool                          force_shmem : 1;
	bool                          allow_transparency : 1;
	bool                          limit_framerate : 1;
	uint32_t					  border_color;
	int							  border_thickness;
};




struct bcu {
	obs_source_t                  *source;

	struct dc_capture			  capture;
	struct cursor_data            cursor_data;
	HANDLE                        injector_process;
	uint32_t                      cx;
	uint32_t                      cy;
	uint32_t                      pitch;
	DWORD                         process_id;
	DWORD                         thread_id;
	HWND                          next_window;
	HWND                          window;
	float                         retry_time;
	float                         fps_reset_time;
	float                         retry_interval;
	bool                          wait_for_target_startup : 1;
	bool                          showing : 1;
	bool                          active : 1;
	bool                          capturing : 1;
	bool                          activate_hook : 1;
	bool                          process_is_64bit : 1;
	bool                          error_acquiring : 1;
	bool                          dwm_capture : 1;
	bool                          initial_config : 1;
	bool                          convert_16bit : 1;

	struct bcu_config			  config;

	struct obs_video_info		  *ovi;
	bool						  started;
	char						  *dirpath;
	int							  toggle_position;
	bool						  use2D;
	float						  resize_timer;
	RECT						  last_rect;

	ipc_pipe_server_t             pipe;
	gs_texture_t                  *texture;
	struct hook_info              *global_hook_info;
	HANDLE                        keepalive_mutex;
	HANDLE                        hook_restart;
	HANDLE                        hook_stop;
	HANDLE                        hook_init;
	HANDLE                        hook_ready;
	HANDLE                        hook_exit;
	HANDLE                        hook_data_map;
	HANDLE                        global_hook_info_map;
	HANDLE                        target_process;
	HANDLE                        texture_mutexes[2];
	wchar_t                       *app_sid;

	bool                          is_app;
	float                         cursor_check_time;
	bool                          cursor_hidden;

	volatile long                 hotkey_window;
	volatile bool                 deactivate_hook;
	volatile bool                 activate_hook_now;

	int retrying;

	gs_effect_t					*solid;
	gs_eparam_t					*color;
	gs_technique_t				*tech;
	gs_vertbuffer_t				*box;
	uint32_t					colorVal;
	int							thickness;


	union {
		struct {
			struct shmem_data *shmem_data;
			uint8_t *texture_buffers[2];
		};

		struct shtex_data *shtex_data;
		void *data;
	};

	void(*copy_texture)(struct bcu*);
};




struct graphics_offsets offsets32 = { 0 };
struct graphics_offsets offsets64 = { 0 };

static inline bool use_anticheat(struct bcu *gc)
{
	return false;
}

static inline HANDLE open_mutex_plus_id(struct bcu *gc,
	const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return gc->is_app
		? open_app_mutex(gc->app_sid, new_name)
		: open_mutex(new_name);
}

static inline HANDLE open_mutex_gc(struct bcu *gc,
	const wchar_t *name)
{
	return open_mutex_plus_id(gc, name, gc->process_id);
}

static inline HANDLE open_event_plus_id(struct bcu *gc,
	const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return gc->is_app
		? open_app_event(gc->app_sid, new_name)
		: open_event(new_name);
}

static inline HANDLE open_event_gc(struct bcu *gc,
	const wchar_t *name)
{
	return open_event_plus_id(gc, name, gc->process_id);
}

static inline HANDLE open_map_plus_id(struct bcu *gc,
	const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);

	debug("map id: %S", new_name);

	return gc->is_app
		? open_app_map(gc->app_sid, new_name)
		: OpenFileMappingW(GC_MAPPING_FLAGS, false, new_name);
}

static inline HANDLE open_hook_info(struct bcu *gc)
{
	return open_map_plus_id(gc, SHMEM_HOOK_INFO, gc->process_id);
}


//===============================================  Initialize functions  ===============================================

//BCU hotkey helper struct
struct bcu_hotkey_helper {
	struct obs_scene_item * scene_item;
	int show_id;
	int hide_id;
	int reset_id;
};


//Assertation
bool bcu_assert(const char* funcname, void* param)
{
	if (param == NULL)
	{
		char message[255] = "";
		strcat(message, "[Warning]: ");
		strcat(message, funcname);
		strcat(message, " - parameter");
		strcat(message, " is null.");
		blog(LOG_WARNING, message);
		return false;
	}
	return true;
}


//Get JSON data
char* bcu_get_json_data_from_file(const char* path)
{
	blog(LOG_WARNING, "bcu_get_json_data_from_file");

	FILE *f;
	long len;
	char* result = NULL;

	f = fopen(path, "r");
	if (f)
	{
		fseek(f, 0, SEEK_END);
		len = ftell(f);
		fseek(f, 0, SEEK_SET);
		result = (char*)malloc(len + 1);
		fread(result, 1, len, f);
		fclose(f);
	}

	return result;
}


//Find id
bool bcu_find_id_helper(void *data, size_t idx, obs_hotkey_t *hotkey)
{
	blog(LOG_WARNING, "bcu_find_id_helper");

	if (bcu_assert("bcu_find_id_helper", data) == false) return false;
	if (bcu_assert("bcu_find_id_helper", hotkey) == false) return false;

	char buf_show_hide[256] = "libobs.show_scene_item.";
	char buf_reset[256] = "BCU.reset_position.";
	struct bcu_hotkey_helper *aa = data;

	strcat(buf_show_hide, aa->scene_item->source->context.name);
	strcat(buf_reset, aa->scene_item->source->context.name);


	if (strcmp(hotkey->name, buf_show_hide) == 0)
	{
		aa->show_id = hotkey->id;
		aa->hide_id = hotkey->pair_partner_id;
		if ((aa->show_id >= 0) && (aa->hide_id >= 0) && (aa->reset_id >= 0)) return false;
	}
	if (strcmp(hotkey->name, buf_reset) == 0)
	{
		aa->reset_id = hotkey->id;
		if ((aa->show_id >= 0) && (aa->hide_id >= 0) && (aa->reset_id >= 0)) return false;
	}

	return true;
}


//Get current scene
struct obs_scene* bcu_get_scene(void *data)
{
	blog(LOG_WARNING, "bcu_get_scene");

	if (bcu_assert("bcu_get_scene", data) == false) return NULL;

	struct bcu *gc = data;
	struct obs_source * scene_src;
	struct obs_scene * scene;
	char scene_name[256];

	if (gc->dirpath)
	{
		blog(LOG_WARNING, "		gc->dirpath = %s", gc->dirpath);

		char *text = bcu_get_json_data_from_file(gc->dirpath);
		blog(LOG_WARNING, "		text = %s", text);
		if (!text) return NULL;

		cJSON *json = cJSON_Parse(text);
		if (json)
		{
			json = json->child;

			while (json)
			{
				if (strcmp(json->string, "current_scene") == 0)
				{
					strcpy(scene_name, json->valuestring);
					break;
				}
				json = json->next;
			}

			cJSON_Delete(json);
		}
		free(text);
	}

	blog(LOG_WARNING, "		scene_name = %s", scene_name);
	scene_src = obs_get_source_by_name(scene_name);
	if (!scene_src) return NULL;

	scene = obs_scene_from_source(scene_src);
	blog(LOG_WARNING, "		get scene from source");

	obs_source_release(scene_src);
	blog(LOG_WARNING, "		release scene_src");

	return scene;
}


//Get scene item
struct obs_scene_item* bcu_get_scene_item(void *data)
{
	blog(LOG_WARNING, "bcu_get_scene_item");

	if (bcu_assert("bcu_get_scene_item", data) == false) return NULL;

	struct bcu *gc = data;
	struct obs_scene * scene = bcu_get_scene(data);
	struct obs_scene_item * item;

	if (scene) item = scene->first_item;
	else return NULL;

	while (item)
	{
		if ((item->source) && (gc->source))
		{
			if (strcmp(item->source->context.name, gc->source->context.name) == 0)
			{
				break;
			}
		}
		item = item->next;
	}

	return item;
}


//Set default position
void bcu_set_default_postition(void *data)
{
	blog(LOG_WARNING, "bcu_set_default_postition");

	if (bcu_assert("bcu_set_default_postition", data) == false) return;

	struct bcu *gc = data;
	struct obs_scene_item *item = bcu_get_scene_item(data);
	RECT rect;

	if (gc->ovi && item)
	{
		GetClientRect(gc->window, &rect);
		struct vec2 *pos = bzalloc(sizeof(struct vec2));
		struct vec2 *scale = bzalloc(sizeof(struct vec2));
		

		/*pos->x = gc->ovi->base_width - width * item->scale.x;
		pos->y = gc->ovi->base_height - height  * item->scale.y;*/
		scale->x = 1;
		scale->y = 1;
		obs_sceneitem_set_scale(item, scale);

		if (!gc->use2D)
		{
			uint32_t width;
			uint32_t height;
			if (gc->global_hook_info)
			{
				width = gc->global_hook_info->base_cx;
				height = gc->global_hook_info->base_cy;
			}
			else
			{
				width = gc->cx;
				height = gc->cy;
			}
			pos->x = gc->ovi->base_width - width;
			pos->y = gc->ovi->base_height - height;
			//pos->x = 960;
			//pos->y = 540;
		}
		else
		{
			pos->x = gc->ovi->base_width - rect.right;
			pos->y = gc->ovi->base_height - rect.bottom;
		}
		obs_sceneitem_set_pos(item, pos);
		bfree(pos);
		bfree(scale);
	}

	item = NULL;
}


//Reset dimensions
void bcu_set_scale(void *data)
{
	blog(LOG_WARNING, "bcu_set_scale");

	if (bcu_assert("bcu_set_scale", data) == false) return;

	struct bcu *gc = data;
	struct obs_scene_item * item = bcu_get_scene_item(data);
	

	if (item)
	{
		struct vec2 *pos = bzalloc(sizeof(struct vec2));
		struct vec2 *scale = bzalloc(sizeof(struct vec2));
		
		if (gc->last_rect.right > 0 && gc->last_rect.bottom > 0)
		{
			if (!gc->use2D)
			{
				uint32_t width;
				if (gc->global_hook_info)
				{
					width = gc->global_hook_info->base_cx;
				}
				else
				{
					width = gc->cx;
				}
				scale->x = ((float)gc->last_rect.right / width) * item->scale.x;
				scale->y = scale->x;
			}
			else
			{
				RECT rect;
				GetClientRect(gc->window, &rect);
				scale->x = ((float)gc->last_rect.right / rect.right) * item->scale.x;
				scale->y = scale->x;
			}
			obs_sceneitem_set_scale(item, scale);
		}
	}

	item = NULL;
}


//Hotkey positioning
static bool bcu_hotkey_pos(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	blog(LOG_WARNING, "bcu_hotkey_pos");

	if (bcu_assert("bcu_hotkey_pos", data) == false) return false;
	if (bcu_assert("bcu_hotkey_pos", &id) == false) return false;
	if (bcu_assert("bcu_hotkey_pos", hotkey) == false) return false;

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct bcu *gc = data;

	if (pressed && gc)
	{
		bcu_set_default_postition(gc);
		return true;
	}

	return false;
}


//Init
void bcu_init(void *data)
{
	blog(LOG_WARNING, "bcu_init");

	if (bcu_assert("bcu_init", data) == false) return;

	//MessageBox(NULL, (LPCWSTR)L"BoomApp is not running. Please start BoomApp before start playing game!", (LPCWSTR)L"Warning", MB_SYSTEMMODAL | MB_OK | MB_ICONWARNING);
	
	// Find BoomApp window
	char* title = bzalloc(255 * sizeof(char));
	strcpy(title, "Boom Replay");
	if (!find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_TITLE, "", title, ""))
	{
		MessageBoxA(NULL, "Boom Replay is not running. Please start Boom Replay before OBS!", "Warning", MB_SYSTEMMODAL | MB_OK | MB_ICONWARNING);
	}
	bfree(title);

	struct bcu *gc = data;
	struct obs_scene * scene = bcu_get_scene(data);
	struct obs_scene_item * item = bcu_get_scene_item(data);

	if (bcu_assert("bcu_init", scene) == false) return;
	if (bcu_assert("bcu_init", item) == false) return;

	//Set unvisible
	//obs_sceneitem_set_visible(item, false);


	//Register toggle position hotkey function
	char id[256] = "";
	char name[256] = "BCU.reset_position.";
	char desc[256] = "Reset ";

	strcat(id, gc->source->context.name);
	strcat(name, id);
	strcat(desc, id);

	gc->toggle_position = obs_hotkey_register_source(scene->source, name, desc, bcu_hotkey_pos, gc);


	//Set hotkeys to turn visibility
	obs_key_combination_t *keycomb_show_hide = bzalloc(sizeof(obs_key_combination_t));
	keycomb_show_hide->key = OBS_KEY_F7;
	keycomb_show_hide->modifiers = 0;

	obs_key_combination_t *keycomb_reset = bzalloc(sizeof(obs_key_combination_t));
	keycomb_reset->key = OBS_KEY_F8;
	keycomb_reset->modifiers = 0;

	struct bcu_hotkey_helper *aa = bzalloc(sizeof(struct bcu_hotkey_helper));

	aa->scene_item = item;
	aa->show_id = -1;
	aa->hide_id = -1;
	aa->reset_id = -1;

	obs_enum_hotkeys(bcu_find_id_helper, aa);

	obs_hotkey_load_bindings(aa->show_id, keycomb_show_hide, 1);
	obs_hotkey_load_bindings(aa->hide_id, keycomb_show_hide, 1);
	obs_hotkey_load_bindings(aa->reset_id, keycomb_reset, 1);

	item = NULL;
	scene = NULL;
}


//===============================================  Functions  ===============================================

static inline enum gs_color_format convert_format(uint32_t format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:     return GS_RGBA;
	case DXGI_FORMAT_B8G8R8X8_UNORM:     return GS_BGRX;
	case DXGI_FORMAT_B8G8R8A8_UNORM:     return GS_BGRA;
	case DXGI_FORMAT_R10G10B10A2_UNORM:  return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_UNORM: return GS_RGBA16;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return GS_RGBA16F;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return GS_RGBA32F;
	}

	return GS_UNKNOWN;
}


static void close_handle(HANDLE *p_handle)
{
	HANDLE handle = *p_handle;
	if (handle) {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		*p_handle = NULL;
	}
}


static inline HMODULE kernel32(void)
{
	static HMODULE kernel32_handle = NULL;
	if (!kernel32_handle)
		kernel32_handle = GetModuleHandleW(L"kernel32");
	return kernel32_handle;
}


static inline HANDLE open_process(DWORD desired_access, bool inherit_handle,
	DWORD process_id)
{
	static HANDLE(WINAPI *open_process_proc)(DWORD, BOOL, DWORD) = NULL;
	if (!open_process_proc)
		open_process_proc = get_obfuscated_func(kernel32(),
			"NuagUykjcxr", 0x1B694B59451ULL);

	return open_process_proc(desired_access, inherit_handle, process_id);
}


// Stop capturing
static void stop_capture(struct bcu *gc)
{
	if (bcu_assert("stop_capture", gc) == false) return;

	ipc_pipe_server_free(&gc->pipe);

	if (gc->hook_stop) {
		SetEvent(gc->hook_stop);
	}
	if (gc->global_hook_info) {
		UnmapViewOfFile(gc->global_hook_info);
		gc->global_hook_info = NULL;
	}
	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	if (gc->app_sid) {
		LocalFree(gc->app_sid);
		gc->app_sid = NULL;
	}

	close_handle(&gc->hook_restart);
	close_handle(&gc->hook_stop);
	close_handle(&gc->hook_ready);
	close_handle(&gc->hook_exit);
	close_handle(&gc->hook_init);
	close_handle(&gc->hook_data_map);
	close_handle(&gc->keepalive_mutex);
	close_handle(&gc->global_hook_info_map);
	close_handle(&gc->target_process);
	close_handle(&gc->texture_mutexes[0]);
	close_handle(&gc->texture_mutexes[1]);

	if (gc->texture) {
		obs_enter_graphics();
		gs_texture_destroy(gc->texture);
		obs_leave_graphics();
		gc->texture = NULL;
	}

	gc->copy_texture = NULL;
	gc->wait_for_target_startup = false;
	gc->active = false;
	gc->capturing = false;
}


static inline void free_config(struct bcu_config *config)
{
	if (bcu_assert("free_config", config) == false) return;

	bfree(config->title_first);
	bfree(config->title_second);
	memset(config, 0, sizeof(*config));
}


static void bcu_destroy(void *data)
{
	if (bcu_assert("bcu_destroy", data) == false) return;
	struct bcu *gc = data;

	if (!gc->use2D)
	{
		stop_capture(gc);
		obs_enter_graphics();
		cursor_data_free(&gc->cursor_data);
		obs_leave_graphics();
	}
	else
	{
		obs_enter_graphics();
		dc_capture_free(&gc->capture);
		obs_leave_graphics();
	}

	obs_hotkey_unregister(gc->toggle_position);
	free_config(&gc->config);
	bfree(gc);
}


static inline void get_config(struct bcu_config *cfg, obs_data_t *settings)
{
	if (bcu_assert("get_config", cfg) == false) return;
	if (bcu_assert("get_config", settings) == false) return;


	cfg->title_first = bzalloc(255 * sizeof(char));
	cfg->title_second = bzalloc(255 * sizeof(char));
	strcpy(cfg->title_first,	"ReplayView");
	strcpy(cfg->title_second,	"Boom Replay (Resizing affects replays in broadcast)");
	//strcpy(cfg->title_first,	"ShadowVolume");
	//strcpy(cfg->title_second,	"BasicHLSL");
	//strcpy(cfg->class, "ReplayView");
	//strcpy(cfg->class, "3DReplay");

	cfg->force_shmem = obs_data_get_bool(settings, SETTING_COMPATIBILITY);
	cfg->cursor = obs_data_get_bool(settings, SETTING_CURSOR);
	cfg->allow_transparency = obs_data_get_bool(settings, SETTING_TRANSPARENCY);
	cfg->limit_framerate = obs_data_get_bool(settings, SETTING_LIMIT_FRAMERATE);
	cfg->border_color = (uint32_t)obs_data_get_int(settings, SETTING_BORDER_COLOR);
	cfg->border_thickness = obs_data_get_int(settings, SETTING_BORDER_THICKNESS);
}


static inline int s_cmp(const char *str1, const char *str2)
{
	if (!str1 || !str2)
		return -1;

	return strcmp(str1, str2);
}


static inline bool capture_needs_reset(struct bcu_config *cfg1, struct bcu_config *cfg2)
{
	if (bcu_assert("capture_needs_reset", cfg1) == false) return false;
	if (bcu_assert("capture_needs_reset", cfg2) == false) return false;


	if ((s_cmp(cfg1->title_first, cfg2->title_first) != 0) || (s_cmp(cfg1->title_second, cfg2->title_second) != 0))
	{
		return true;
	}

	else if (cfg1->force_shmem != cfg2->force_shmem)
	{
		return true;

	}

	else if (cfg1->limit_framerate != cfg2->limit_framerate)
	{
		return true;

	}

	return false;
}


// Module update
static void bcu_update(void *data, obs_data_t *settings)
{
	struct bcu *gc = data;
	struct bcu_config cfg;

	if (bcu_assert("bcu_update", data) == false) return;
	if (bcu_assert("bcu_update", settings) == false) return;

	// Update the border color & thickness
	uint32_t color = (uint32_t)obs_data_get_int(settings, SETTING_BORDER_COLOR);
	gc->colorVal = color;
	int thickness = obs_data_get_int(settings, SETTING_BORDER_THICKNESS);
	gc->thickness = thickness;

	if (!gc->use2D)
	{
		bool reset_capture = false;

		get_config(&cfg, settings);
		reset_capture = capture_needs_reset(&cfg, &gc->config);

		gc->error_acquiring = false;
		gc->activate_hook = true;

		free_config(&gc->config);
		gc->config = cfg;
		gc->retry_interval = DEFAULT_RETRY_INTERVAL;
		gc->wait_for_target_startup = false;

		if (!gc->initial_config) {
			if (reset_capture) {
				stop_capture(gc);
			}
		}
		else {
			gc->initial_config = false;
		}
	}
	else
	{
		gc->window = NULL;
	}
}


extern void wait_for_hook_initialization(void);

// Module create
static void *bcu_create(obs_data_t *settings, obs_source_t *source)
{
	if (bcu_assert("bcu_create", settings) == false) return NULL;
	if (bcu_assert("bcu_create", source) == false) return NULL;

	struct bcu *gc = bzalloc(sizeof(*gc));

	wait_for_hook_initialization();

	gc->source = source;
	gc->initial_config = true;
	gc->retry_interval = DEFAULT_RETRY_INTERVAL;

	gc->use2D = false;
	gc->ovi = bzalloc(sizeof(struct obs_video_info));

	obs_get_video_info(gc->ovi);

	char buf[32];
	wchar_t wbuf[255];

	gc->dirpath = malloc(255 * sizeof(char));
	strcpy(gc->dirpath, getenv("APPDATA"));
	strcat(gc->dirpath, "\\obs-studio\\basic\\scenes\\");

	MultiByteToWideChar(CP_ACP, 0, gc->dirpath, strlen(gc->dirpath), wbuf, strlen(gc->dirpath));
	wbuf[strlen(gc->dirpath)] = L'\0';

	lstrcatW(wbuf, L"*.json");

	WIN32_FIND_DATA FindFileData;
	HANDLE hFind = FindFirstFile(wbuf, &FindFileData);

	WideCharToMultiByte(CP_ACP, 0, FindFileData.cFileName, lstrlenW(FindFileData.cFileName), buf, lstrlenW(FindFileData.cFileName), NULL, NULL);
	buf[lstrlenW(FindFileData.cFileName)] = '\0';

	strcat(gc->dirpath, buf);

	FindClose(hFind);

	// Init border effects
	obs_enter_graphics();
	gc->solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gc->color = gs_effect_get_param_by_name(gc->solid, "color");
	gc->tech = gs_effect_get_technique(gc->solid, "Solid");
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 0.0f);
	gc->box = gs_render_save();
	obs_leave_graphics();


	gc->started = false;




	bcu_update(gc, settings);
	return gc;
}


#define STOP_BEING_BAD \
	"  This is most likely due to security software. Please make sure " \
        "that the OBS installation folder is excluded/ignored in the "      \
        "settings of the security software you are using."


static bool check_file_integrity(struct bcu *gc, const char *file,
	const char *name)
{
	DWORD error;
	HANDLE handle;
	wchar_t *w_file = NULL;

	if (!file || !*file) {
		warn("BCU %s not found." STOP_BEING_BAD, name);
		return false;
	}

	if (!os_utf8_to_wcs_ptr(file, 0, &w_file)) {
		warn("Could not convert file name to wide string");
		return false;
	}

	handle = CreateFileW(w_file, GENERIC_READ | GENERIC_EXECUTE,
		FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	bfree(w_file);

	if (handle != INVALID_HANDLE_VALUE) {
		CloseHandle(handle);
		return true;
	}

	error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND) {
		warn("BCU file '%s' not found."
			STOP_BEING_BAD, file);
	}
	else if (error == ERROR_ACCESS_DENIED) {
		warn("BCU file '%s' could not be loaded."
			STOP_BEING_BAD, file);
	}
	else {
		warn("BCU file '%s' could not be loaded: %lu."
			STOP_BEING_BAD, file, error);
	}

	return false;
}


static inline bool is_64bit_windows(void)
{
#ifdef _WIN64
	return true;
#else
	BOOL x86 = false;
	bool success = !!IsWow64Process(GetCurrentProcess(), &x86);
	return success && !!x86;
#endif
}


static inline bool is_64bit_process(HANDLE process)
{
	BOOL x86 = true;
	if (is_64bit_windows()) {
		bool success = !!IsWow64Process(process, &x86);
		if (!success) {
			return false;
		}
	}

	return !x86;
}


static inline bool open_target_process(struct bcu *gc)
{
	gc->target_process = open_process(
		PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
		false, gc->process_id);
	if (!gc->target_process) {
		warn("could not open process: %s", gc->config.title_first);
		return false;
	}

	gc->process_is_64bit = is_64bit_process(gc->target_process);
	gc->is_app = is_app(gc->target_process);
	if (gc->is_app) {
		gc->app_sid = get_app_sid(gc->target_process);
	}
	return true;
}


static inline bool init_keepalive(struct bcu *gc)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", WINDOW_HOOK_KEEPALIVE,
		gc->process_id);

	gc->keepalive_mutex = CreateMutexW(NULL, false, new_name);
	if (!gc->keepalive_mutex) {
		warn("Failed to create keepalive mutex: %lu", GetLastError());
		return false;
	}

	return true;
}


//Returns the last Win32 error, in string format. Returns an empty string if there is no error.
char* GetLastErrorAsString()
{
	char* buf = malloc(256);
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, 0, GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 256, 0);

	return buf;
}


static inline bool init_texture_mutexes(struct bcu *gc)
{
	gc->texture_mutexes[0] = open_mutex_gc(gc, MUTEX_TEXTURE1);
	gc->texture_mutexes[1] = open_mutex_gc(gc, MUTEX_TEXTURE2);

	if (!gc->texture_mutexes[0] || !gc->texture_mutexes[1]) {
		DWORD error = GetLastError();

		info("init_texture_mutexes failing: %i %i %i %s", gc->texture_mutexes[0], gc->texture_mutexes[1], error, GetLastErrorAsString());
		if (error == 2) {
			if (!gc->retrying) {
				gc->retrying = 2;
				info("hook not loaded yet, retrying..");
			}
		}
		else {
			warn("failed to open texture mutexes: %lu",
				GetLastErrorAsString());
		}
		return false;
	}

	return true;
}


/* if there's already a hook in the process, then signal and start */
static inline bool attempt_existing_hook(struct bcu *gc)
{
	if (bcu_assert("attempt_existing_hook", gc) == false) return false;

	gc->hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
	if (gc->hook_restart) {
		debug("existing hook found, signaling process: %s",
			gc->config.title_first);
		SetEvent(gc->hook_restart);
		return true;
	}

	return false;
}


static inline void reset_frame_interval(struct bcu *gc)
{
	if (bcu_assert("reset_frame_interval", gc) == false) return;

	struct obs_video_info ovi;
	uint64_t interval = 0;

	if (obs_get_video_info(&ovi)) {
		interval = ovi.fps_den * 1000000000ULL / ovi.fps_num;

		/* Always limit capture framerate to some extent.  If a game
		* running at 900 FPS is being captured without some sort of
		* limited capture interval, it will dramatically reduce
		* performance. */
		if (!gc->config.limit_framerate)
			interval /= 2;
	}

	gc->global_hook_info->frame_interval = interval;
}


static inline bool init_hook_info(struct bcu *gc)
{
	gc->global_hook_info_map = open_hook_info(gc);
	if (!gc->global_hook_info_map) {
		warn("init_hook_info: get_hook_info failed: %lu",
			GetLastError());
		return false;
	}

	gc->global_hook_info = MapViewOfFile(gc->global_hook_info_map,
		FILE_MAP_ALL_ACCESS, 0, 0,
		sizeof(*gc->global_hook_info));
	if (!gc->global_hook_info) {
		warn("init_hook_info: failed to map data view: %lu",
			GetLastError());
		return false;
	}

	gc->global_hook_info->offsets = gc->process_is_64bit ?
		offsets64 : offsets32;
	gc->global_hook_info->force_shmem = gc->config.force_shmem;
	gc->global_hook_info->use_scale = false;
	reset_frame_interval(gc);

	obs_enter_graphics();
	if (!gs_shared_texture_available()) {
		warn("init_hook_info: shared texture capture unavailable");
		gc->global_hook_info->force_shmem = true;
	}
	obs_leave_graphics();

	return true;
}


static void pipe_log(void *param, uint8_t *data, size_t size)
{
	struct bcu *gc = param;
	if (data && size) info("%s", data);
}


static inline bool init_pipe(struct bcu *gc)
{
	char name[64];
	sprintf(name, "%s%lu", PIPE_NAME, gc->process_id);

	if (!ipc_pipe_server_start(&gc->pipe, name, pipe_log, gc)) {
		warn("init_pipe: failed to start pipe");
		return false;
	}

	return true;
}


static inline int inject_library(HANDLE process, const wchar_t *dll)
{
	return inject_library_obf(process, dll,
		"D|hkqkW`kl{k\\osofj", 0xa178ef3655e5ade7,
		"[uawaRzbhh{tIdkj~~", 0x561478dbd824387c,
		"[fr}pboIe`dlN}", 0x395bfbc9833590fd,
		"\\`zs}gmOzhhBq", 0x12897dd89168789a,
		"GbfkDaezbp~X", 0x76aff7238788f7db);
}


static inline bool hook_direct(struct bcu *gc,
	const char *hook_path_rel)
{
	wchar_t hook_path_abs_w[MAX_PATH];
	wchar_t *hook_path_rel_w;
	wchar_t *path_ret;
	HANDLE process;
	int ret;

	os_utf8_to_wcs_ptr(hook_path_rel, 0, &hook_path_rel_w);
	if (!hook_path_rel_w) {
		warn("hook_direct: could not convert string");
		return false;
	}

	path_ret = _wfullpath(hook_path_abs_w, hook_path_rel_w, MAX_PATH);
	bfree(hook_path_rel_w);

	if (path_ret == NULL) {
		warn("hook_direct: could not make absolute path");
		return false;
	}

	process = open_process(PROCESS_ALL_ACCESS, false, gc->process_id);
	if (!process) {
		warn("hook_direct: could not open process: %s (%lu)", gc->config.title_first, GetLastError());
		return false;
	}

	ret = inject_library(process, hook_path_abs_w);
	CloseHandle(process);

	if (ret != 0) {
		warn("hook_direct: inject failed: %d", ret);
		return false;
	}

	return true;
}


static inline bool create_inject_process(struct bcu *gc,
	const char *inject_path, const char *hook_dll)
{
	wchar_t *command_line_w = malloc(4096 * sizeof(wchar_t));
	wchar_t *inject_path_w;
	wchar_t *hook_dll_w;
	bool anti_cheat = false;
	PROCESS_INFORMATION pi = { 0 };
	STARTUPINFO si = { 0 };
	bool success = false;

	os_utf8_to_wcs_ptr(inject_path, 0, &inject_path_w);
	os_utf8_to_wcs_ptr(hook_dll, 0, &hook_dll_w);

	si.cb = sizeof(si);

	swprintf(command_line_w, 4096, L"\"%s\" \"%s\" %lu %lu",
		inject_path_w, hook_dll_w,
		(unsigned long)anti_cheat,
		anti_cheat ? gc->thread_id : gc->process_id);

	success = !!CreateProcessW(inject_path_w, command_line_w, NULL, NULL,
		false, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	if (success) {
		CloseHandle(pi.hThread);
		gc->injector_process = pi.hProcess;
	}
	else {
		warn("Failed to create inject helper process: %lu",
			GetLastError());
	}

	free(command_line_w);
	bfree(inject_path_w);
	bfree(hook_dll_w);
	return success;
}


static inline bool inject_hook(struct bcu *gc)
{
	bool matching_architecture;
	bool success = false;
	const char *hook_dll;
	char *inject_path;
	char *hook_path;

	if (gc->process_is_64bit) {
		hook_dll = "graphics-hook64.dll";
		inject_path = obs_module_file("inject-helper64.exe");
	}
	else {
		hook_dll = "graphics-hook32.dll";
		inject_path = obs_module_file("inject-helper32.exe");
	}

	hook_path = obs_module_file(hook_dll);

	if (!check_file_integrity(gc, inject_path, "inject helper")) {
		goto cleanup;
	}
	if (!check_file_integrity(gc, hook_path, "graphics hook")) {
		goto cleanup;
	}

#ifdef _WIN64
	matching_architecture = gc->process_is_64bit;
#else
	matching_architecture = !gc->process_is_64bit;
#endif

	if (matching_architecture && !use_anticheat(gc)) {
		info("using direct hook");
		success = hook_direct(gc, hook_path);
	}
	else {
		info("using helper (%s hook)", use_anticheat(gc) ?
			"compatibility" : "direct");
		success = create_inject_process(gc, inject_path, hook_dll);
	}

cleanup:
	bfree(inject_path);
	bfree(hook_path);
	return success;
}


static inline bool init_events(struct bcu *gc)
{
	if (!gc->hook_restart) {
		gc->hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
		if (!gc->hook_restart) {
			warn("init_events: failed to get hook_restart "
				"event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_stop) {
		gc->hook_stop = open_event_gc(gc, EVENT_CAPTURE_STOP);
		if (!gc->hook_stop) {
			warn("init_events: failed to get hook_stop event: %lu",
				GetLastError());
			return false;
		}
	}

	if (!gc->hook_init) {
		gc->hook_init = open_event_gc(gc, EVENT_HOOK_INIT);
		if (!gc->hook_init) {
			warn("init_events: failed to get hook_init event: %lu",
				GetLastError());
			return false;
		}
	}

	if (!gc->hook_ready) {
		gc->hook_ready = open_event_gc(gc, EVENT_HOOK_READY);
		if (!gc->hook_ready) {
			warn("init_events: failed to get hook_ready event: %lu",
				GetLastError());
			return false;
		}
	}

	if (!gc->hook_exit) {
		gc->hook_exit = open_event_gc(gc, EVENT_HOOK_EXIT);
		if (!gc->hook_exit) {
			warn("init_events: failed to get hook_exit event: %lu",
				GetLastError());
			return false;
		}
	}

	return true;
}


static bool target_suspended(struct bcu *gc)
{
	return thread_is_suspended(gc->process_id, gc->thread_id);
}


static bool init_hook(struct bcu *gc)
{
	struct dstr exe = { 0 };

	if (get_window_exe(&exe, gc->next_window)) {
		info("attempting to hook process: %s %i", exe.array, gc->is_app);
	}
	else {
		info("unable to attempt hook: %s", exe.array);
	}
	dstr_free(&exe);

	if (target_suspended(gc)) {
		info("target_suspended failed");
		return false;
	}
	if (!open_target_process(gc)) {
		info("open_target_process failed");
		return false;
	}
	if (!init_keepalive(gc)) {
		info("init_keepalive failed");
		return false;
	}
	if (!init_pipe(gc)) {
		info("init_pipe failed");
		return false;
	}
	if (!attempt_existing_hook(gc)) {
		if (!inject_hook(gc)) {
			info("inject_hook failed");
			return false;
		}
	}
	if (!init_texture_mutexes(gc)) {
		info("init_texture_mutexes failed");
		return false;
	}
	if (!init_hook_info(gc)) {
		info("init_hook_info failed");
		return false;
	}
	if (!init_events(gc)) {
		info("init_events failed");
		return false;
	}

	SetEvent(gc->hook_init);

	gc->window = gc->next_window;
	gc->next_window = NULL;
	gc->active = true;
	gc->retrying = 0;
	info("hooked successfully");
	return true;
}


static void setup_window(struct bcu *gc, HWND window)
{
	HANDLE hook_restart;
	HANDLE process;

	GetWindowThreadProcessId(window, &gc->process_id);
	if (gc->process_id) {
		process = open_process(PROCESS_QUERY_INFORMATION,
			false, gc->process_id);
		if (process) {
			gc->is_app = is_app(process);
			if (gc->is_app) {
				gc->app_sid = get_app_sid(process);
			}
			CloseHandle(process);
		}
	}

	/* do not wait if we're re-hooking a process */
	hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
	if (hook_restart) {
		gc->wait_for_target_startup = false;
		CloseHandle(hook_restart);
	}

	/* otherwise if it's an unhooked process, always wait a bit for the
	* target process to start up before starting the hook process;
	* sometimes they have important modules to load first or other hooks
	* (such as steam) need a little bit of time to load.  ultimately this
	* helps prevent crashes */
	if (gc->wait_for_target_startup) {
		gc->retry_interval = 0.1f;
		gc->wait_for_target_startup = false;
	}
	else {
		gc->next_window = window;
	}
}


static void get_selected_window(struct bcu *gc)
{
	HWND window;

	if ((strcmpi(gc->config.title_first, "dwm") == 0) || (strcmpi(gc->config.title_second, "dwm") == 0))
	{
		wchar_t class_wf[512];
		wchar_t class_ws[512];
		os_utf8_to_wcs(gc->config.title_first, 0, class_wf, 512);
		os_utf8_to_wcs(gc->config.title_second, 0, class_ws, 512);
		if (window = FindWindowW(class_wf, NULL))
		{
			gc->use2D = false;
		}
		else if (window = FindWindowW(class_ws, NULL))
		{
			gc->use2D = true;
		}
	}
	else 
	{
		if (window = find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_TITLE, "", gc->config.title_first, ""))
		{
			gc->use2D = false;

			blog(LOG_WARNING, "get_selected_window is 3D");
		}
		else if (window = find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_TITLE, "", gc->config.title_second, ""))
		{
			gc->use2D = true;
			blog(LOG_WARNING, "get_selected_window is 2D");
		}
	}

	if (window) 
	{
		if (!gc->use2D) setup_window(gc, window);
		info("Found ReplayView window! %i", gc->is_app);
	}
	else 
	{
		gc->wait_for_target_startup = true;
		//gc->use2D = false;
		//info("No ReplayView window!");
	}
}


static void try_hook(struct bcu *gc)
{
	get_selected_window(gc);

	if (gc->next_window)
	{
		gc->thread_id = GetWindowThreadProcessId(gc->next_window, &gc->process_id);

		// Make sure we never try to hook ourselves (projector)
		if (gc->process_id == GetCurrentProcessId()) {
			return;
		}

		if (!gc->thread_id || !gc->process_id) {
			warn("error acquiring, failed to get window "
				"thread/process ids: %lu",
				GetLastError());
			gc->error_acquiring = true;
			return;
		}

		if (!init_hook(gc)) {
			info("try_hook init_hook failed, stopping capture");
			stop_capture(gc);
		}
	}
	else {
		gc->active = false;
	}
}


enum capture_result {
	CAPTURE_FAIL,
	CAPTURE_RETRY,
	CAPTURE_SUCCESS
};


static inline enum capture_result init_capture_data(struct bcu *gc)
{
	gc->cx = gc->global_hook_info->cx;
	gc->cy = gc->global_hook_info->cy;
	gc->pitch = gc->global_hook_info->pitch;

	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	CloseHandle(gc->hook_data_map);

	gc->hook_data_map = open_map_plus_id(gc, SHMEM_TEXTURE,
		gc->global_hook_info->map_id);
	if (!gc->hook_data_map) {
		DWORD error = GetLastError();
		if (error == 2) {
			return CAPTURE_RETRY;
		}
		else {
			warn("init_capture_data: failed to open file "
				"mapping: %lu", error);
		}
		return CAPTURE_FAIL;
	}

	gc->data = MapViewOfFile(gc->hook_data_map, FILE_MAP_ALL_ACCESS, 0, 0,
		gc->global_hook_info->map_size);
	if (!gc->data) {
		warn("init_capture_data: failed to map data view: %lu",
			GetLastError());
		return CAPTURE_FAIL;
	}

	return CAPTURE_SUCCESS;
}


#define PIXEL_16BIT_SIZE 2
#define PIXEL_32BIT_SIZE 4


static inline uint32_t convert_5_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x1F) * (255.0 / 31.0));
}


static inline uint32_t convert_6_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x3F) * (255.0 / 63.0));
}


static void copy_b5g6r5_tex(struct bcu *gc, int cur_texture,
	uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (uint32_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (uint32_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i*)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000007E0);
			__m128i green_offset = _mm_set1_epi32(0x00000008);
			__m128i red_channel_mask = _mm_set1_epi32(0x0000F800);
			__m128i red_offset = _mm_set1_epi32(0x00000300);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i*)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i*)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}


static void copy_b5g5r5a1_tex(struct bcu *gc, int cur_texture,
	uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (uint32_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (uint32_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red, pixels_alpha;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i*)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000003E0);
			__m128i green_offset = _mm_set1_epi32(0x000000C);
			__m128i red_channel_mask = _mm_set1_epi32(0x00007C00);
			__m128i red_offset = _mm_set1_epi32(0x00000180);
			__m128i alpha_channel_mask = _mm_set1_epi32(0x00008000);
			__m128i alpha_offset = _mm_set1_epi32(0x00000001);
			__m128i alpha_mask32 = _mm_set1_epi32(0xFF000000);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_low, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i*)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_high, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i*)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}


static inline void copy_16bit_tex(struct bcu *gc, int cur_texture,
	uint8_t *data, uint32_t pitch)
{
	if (gc->global_hook_info->format == DXGI_FORMAT_B5G5R5A1_UNORM) {
		copy_b5g5r5a1_tex(gc, cur_texture, data, pitch);

	}
	else if (gc->global_hook_info->format == DXGI_FORMAT_B5G6R5_UNORM) {
		copy_b5g6r5_tex(gc, cur_texture, data, pitch);
	}
}


static void copy_shmem_tex(struct bcu *gc)
{
	int cur_texture = gc->shmem_data->last_tex;
	HANDLE mutex = NULL;
	uint32_t pitch;
	int next_texture;
	uint8_t *data;

	if (cur_texture < 0 || cur_texture > 1)
		return;

	next_texture = cur_texture == 1 ? 0 : 1;

	if (object_signalled(gc->texture_mutexes[cur_texture])) {
		mutex = gc->texture_mutexes[cur_texture];

	}
	else if (object_signalled(gc->texture_mutexes[next_texture])) {
		mutex = gc->texture_mutexes[next_texture];
		cur_texture = next_texture;

	}
	else {
		return;
	}

	if (gs_texture_map(gc->texture, &data, &pitch)) {
		if (gc->convert_16bit) {
			copy_16bit_tex(gc, cur_texture, data, pitch);

		}
		else if (pitch == gc->pitch) {
			memcpy(data, gc->texture_buffers[cur_texture],
				pitch * gc->cy);
		}
		else {
			uint8_t *input = gc->texture_buffers[cur_texture];
			uint32_t best_pitch =
				pitch < gc->pitch ? pitch : gc->pitch;

			for (uint32_t y = 0; y < gc->cy; y++) {
				uint8_t *line_in = input + gc->pitch * y;
				uint8_t *line_out = data + pitch * y;
				memcpy(line_out, line_in, best_pitch);
			}
		}

		gs_texture_unmap(gc->texture);
	}

	ReleaseMutex(mutex);
}


static inline bool is_16bit_format(uint32_t format)
{
	return format == DXGI_FORMAT_B5G5R5A1_UNORM ||
		format == DXGI_FORMAT_B5G6R5_UNORM;
}


static inline bool init_shmem_capture(struct bcu *gc)
{
	enum gs_color_format format;

	gc->texture_buffers[0] =
		(uint8_t*)gc->data + gc->shmem_data->tex1_offset;
	gc->texture_buffers[1] =
		(uint8_t*)gc->data + gc->shmem_data->tex2_offset;

	gc->convert_16bit = is_16bit_format(gc->global_hook_info->format);
	format = gc->convert_16bit ?
		GS_BGRA : convert_format(gc->global_hook_info->format);

	obs_enter_graphics();
	gs_texture_destroy(gc->texture);
	gc->texture = gs_texture_create(gc->cx, gc->cy, format, 1, NULL,
		GS_DYNAMIC);
	obs_leave_graphics();

	if (!gc->texture) {
		warn("init_shmem_capture: failed to create texture");
		return false;
	}

	gc->copy_texture = copy_shmem_tex;
	return true;
}


static inline bool init_shtex_capture(struct bcu *gc)
{
	obs_enter_graphics();
	gs_texture_destroy(gc->texture);
	gc->texture = gs_texture_open_shared(gc->shtex_data->tex_handle);
	obs_leave_graphics();

	if (!gc->texture) {
		warn("init_shtex_capture: failed to open shared handle");
		return false;
	}

	return true;
}


static bool start_capture(struct bcu *gc)
{
	debug("Starting capture");

	if (gc->global_hook_info->type == CAPTURE_TYPE_MEMORY) {
		if (!init_shmem_capture(gc)) {
			warn("start_capture init_shmem_capture failed");
			return false;
		}

		info("memory capture successful");
	}
	else {
		if (!init_shtex_capture(gc)) {
			warn("start_capture init_shtex_capture failed");
			return false;
		}

		info("shared texture capture successful");
	}

	return true;
}


static inline bool capture_valid(struct bcu *gc)
{
	if (!gc->dwm_capture && !IsWindow(gc->window))
		return false;

	// if the window title is still the same
	struct dstr cur_class = { 0 };
	get_window_title(&cur_class, gc->window);

	if (dstr_cmpi(&cur_class, gc->config.title_first) != 0)
	{
		return false;
	}

	return !object_signalled(gc->target_process);
}


// if the window title is still the same
static inline bool is_valid(void *data)
{
	struct bcu *gc = data;
	bool valid = false;

	if (gc->window && IsWindow(gc->window) && gc->config.title_second)
	{
		struct dstr cur_class = { 0 };
		get_window_title(&cur_class, gc->window);
		if (dstr_cmpi(&cur_class, gc->config.title_second) == 0)
		{
			valid = true;
		}
	}
	return valid;
}


static void check_foreground_window(struct bcu *gc, float seconds)
{
	// Hides the cursor if the user isn't actively in the game
	gc->cursor_check_time += seconds;
	if (gc->cursor_check_time >= 0.1f) {
		DWORD foreground_process_id;
		GetWindowThreadProcessId(GetForegroundWindow(),
			&foreground_process_id);
		if (gc->process_id != foreground_process_id)
			gc->cursor_hidden = true;
		else
			gc->cursor_hidden = false;
		gc->cursor_check_time = 0.0f;
	}
}

// Module item tick
static void bcu_tick(void *data, float seconds)
{
	if (bcu_assert("bcu_tick", data) == false) return;

	struct bcu *gc = data;
	RECT rect;
	bool reset_capture = false;

	if (!gc->use2D)
	{
		if (!obs_source_showing(gc->source)) {
			if (gc->showing) {
				if (gc->active)
					stop_capture(gc);
				gc->showing = false;
			}
			return;
		}
		else if (!gc->showing) {
			gc->retry_time = 0.5f;
		}

		if (gc->hook_stop && object_signalled(gc->hook_stop)) {
			debug("hook stop signal received");
			stop_capture(gc);
		}
		/*if (gc->active && deactivate) {
			stop_capture(gc);
		}*/

		if (gc->active && !gc->hook_ready && gc->process_id) {
			gc->hook_ready = open_event_gc(gc, EVENT_HOOK_READY);
		}

		if (gc->injector_process && object_signalled(gc->injector_process)) {
			DWORD exit_code = 0;

			GetExitCodeProcess(gc->injector_process, &exit_code);
			close_handle(&gc->injector_process);

			if (exit_code != 0) {
				warn("inject process failed: %ld", (long)exit_code);
				gc->error_acquiring = true;

			}
			else if (!gc->capturing) {
				gc->retry_interval = ERROR_RETRY_INTERVAL;
				stop_capture(gc);
			}
		}

		if (gc->hook_ready && object_signalled(gc->hook_ready)) {
			debug("capture initializing!");
			enum capture_result result = init_capture_data(gc);


			if (result == CAPTURE_SUCCESS)
				gc->capturing = start_capture(gc);
			else

			if (result != CAPTURE_RETRY && !gc->capturing) {
				gc->retry_interval = ERROR_RETRY_INTERVAL;
				stop_capture(gc);
			}
		}

		gc->retry_time += seconds;

		if (!gc->active) {
			if (!gc->error_acquiring &&
				gc->retry_time > gc->retry_interval) {
				if (gc->activate_hook) {
					try_hook(gc);
					gc->retry_time = 0.0f;
				}
			}
		}
		else {
			if (!capture_valid(gc)) {
				info("capture window no longer exists, "
					"terminating capture");
				stop_capture(gc);
			}
			else {
				if (gc->copy_texture) {
					obs_enter_graphics();
					gc->copy_texture(gc);
					obs_leave_graphics();
				}

				if (gc->config.cursor) {
					check_foreground_window(gc, seconds);
					obs_enter_graphics();
					cursor_capture(&gc->cursor_data);
					obs_leave_graphics();
				}

				gc->fps_reset_time += seconds;
				if (gc->fps_reset_time >= gc->retry_interval) {
					reset_frame_interval(gc);
					gc->fps_reset_time = 0.0f;
				}
			}
		}

		if (!gc->showing)
			gc->showing = true;
	}
	else
	{
		if (!obs_source_showing(gc->source))
			return;

		if (!gc->window || !IsWindow(gc->window) || !is_valid(data)) {
			if (!gc->config.title_second)
				return;

			gc->window = find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_TITLE, "", gc->config.title_second, "");
			if (!gc->window) {
				if (gc->capture.valid)
					dc_capture_free(&gc->capture);
				gc->use2D = false;
				return;
			}

			reset_capture = true;

		}
		else if (IsIconic(gc->window)) {
			return;
		}

		obs_enter_graphics();

		GetClientRect(gc->window, &rect);

		if (!reset_capture) {
			gc->resize_timer += seconds;

			if (gc->resize_timer >= RESIZE_CHECK_TIME) {
				if (rect.bottom != gc->last_rect.bottom ||
					rect.right != gc->last_rect.right)
					reset_capture = true;

				gc->resize_timer = 0.0f;
			}
		}

		if (reset_capture) {
			gc->resize_timer = 0.0f;
			gc->last_rect = rect;
			dc_capture_free(&gc->capture);
			dc_capture_init(&gc->capture, 0, 0, rect.right, rect.bottom,
				gc->config.cursor, false);
		}

		dc_capture_capture(&gc->capture, gc->window);
		obs_leave_graphics();
	}
}


// Render cursor
static inline void bcu_render_cursor(struct bcu *gc)
{
	POINT p = { 0 };

	if (bcu_assert("bcu_render_cursor", gc) == false) return;

	HWND window;

	if (!gc->global_hook_info->base_cx ||
		!gc->global_hook_info->base_cy)
		return;

	window = !!gc->global_hook_info->window
		? (HWND)(uintptr_t)gc->global_hook_info->window
		: gc->window;

	ClientToScreen(window, &p);

	float x_scale = (float)gc->global_hook_info->cx /
		(float)gc->global_hook_info->base_cx;
	float y_scale = (float)gc->global_hook_info->cy /
		(float)gc->global_hook_info->base_cy;

	cursor_draw(&gc->cursor_data, -p.x, -p.y, x_scale, y_scale,
		gc->global_hook_info->base_cx,
		gc->global_hook_info->base_cy);
}


void bcu_render_border(struct bcu *gc, float width, float height)
{
	gs_effect_t    *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t    *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	struct vec4 colorVal;
	vec4_from_rgba(&colorVal, gc->colorVal);
	gs_effect_set_vec4(color, &colorVal);
	gs_technique_begin(gc->tech);
	gs_technique_begin_pass(gc->tech, 0);

	gs_matrix_push();
	float size = gc->thickness + 1;
	gs_matrix_translate3f(-size, -size, 0);
	gs_matrix_scale3f(width + (size * 2), height + (size * 2), 1.0f);
	gs_load_vertexbuffer(gc->box);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_matrix_pop();

	gs_technique_end_pass(gc->tech);
	gs_technique_end(gc->tech);
	gs_load_vertexbuffer(NULL);
}


// Module item render
static void bcu_render(void *data, gs_effect_t *effect)
{
	if (bcu_assert("bcu_render", data) == false) return;

	struct bcu *gc = data;

	// Border
	float width = obs_source_get_width(gc->source);
	float height = obs_source_get_height(gc->source);


	if (!gc->use2D)
	{
		if (!gc->texture || !gc->active)
			return;

		bcu_render_border(gc, width, height);

		effect = obs_get_base_effect(gc->config.allow_transparency ? OBS_EFFECT_DEFAULT : OBS_EFFECT_OPAQUE);

		while (gs_effect_loop(effect, "Draw")) {
			obs_source_draw(gc->texture, 0, 0, 0, 0, gc->global_hook_info->flip);

			if (gc->config.allow_transparency && gc->config.cursor && !gc->cursor_hidden) {
				bcu_render_cursor(gc);
			}
		}

		if (!gc->config.allow_transparency && gc->config.cursor && !gc->cursor_hidden) {
			effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

			while (gs_effect_loop(effect, "Draw")) {
				bcu_render_cursor(gc);
			}
		}
		
	}
	else
	{
		bcu_render_border(gc, width, height);
		dc_capture_render(&gc->capture, obs_get_base_effect(OBS_EFFECT_OPAQUE));
		UNUSED_PARAMETER(effect);
	}
}


// Module item width
static uint32_t bcu_width(void *data)
{
	if (bcu_assert("bcu_width", data) == false) return 0;
	struct bcu *gc = data;

	if (!gc->use2D)
	{
		return gc->active ? gc->global_hook_info->cx : 0;
	}
	else
	{
		return gc->capture.width;
	}
}


// Module item height
static uint32_t bcu_height(void *data)
{
	if (bcu_assert("bcu_height", data) == false) return 0;
	struct bcu *gc = data;

	if (!gc->use2D)
	{
		return gc->active ? gc->global_hook_info->cy : 0;
	}
	else
	{
		return gc->capture.height;
	}
}


// Module name
static const char *bcu_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_BCU;
}




// Module default settings
static void bcu_defaults(obs_data_t *settings)
{
	if (bcu_assert("bcu_defaults", settings) == false) return;

	obs_data_set_default_bool(settings, SETTING_COMPATIBILITY, false);
	obs_data_set_default_bool(settings, SETTING_CURSOR, true);
	obs_data_set_default_bool(settings, SETTING_TRANSPARENCY, false);
	obs_data_set_default_bool(settings, SETTING_LIMIT_FRAMERATE, false);
	obs_data_set_default_int(settings, SETTING_BORDER_COLOR, DEFAULT_BORDER_COLOR);
	obs_data_set_default_int(settings, SETTING_BORDER_THICKNESS, DEFAULT_BORDER_THICKNESS);
}


// Module properties
static obs_properties_t *bcu_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();

	obs_properties_add_bool(ppts, SETTING_COMPATIBILITY, TEXT_SLI_COMPATIBILITY);
	obs_properties_add_bool(ppts, SETTING_TRANSPARENCY, TEXT_ALLOW_TRANSPARENCY);
	obs_properties_add_bool(ppts, SETTING_LIMIT_FRAMERATE, TEXT_LIMIT_FRAMERATE);
	obs_properties_add_bool(ppts, SETTING_CURSOR, TEXT_CAPTURE_CURSOR);
	obs_properties_add_color(ppts, SETTING_BORDER_COLOR, TEXT_BORDER_COLOR);
	obs_properties_add_int_slider(ppts, SETTING_BORDER_THICKNESS, TEXT_BORDER_THICKNESS, 0, 25, 1);

	UNUSED_PARAMETER(data);
	return ppts;
}




// Module info
struct obs_source_info bcu_info = {
	.id = "boom_capture_universal",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = bcu_name,
	.create = bcu_create,
	.destroy = bcu_destroy,
	.get_width = bcu_width,
	.get_height = bcu_height,
	.get_defaults = bcu_defaults,
	.get_properties = bcu_properties,
	.update = bcu_update,
	.video_tick = bcu_tick,
	.video_render = bcu_render
};
