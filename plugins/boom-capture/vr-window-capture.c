#include <stdlib.h>
#include <stdio.h>
#include <util/dstr.h>
#include <obs-internal.h>
#include <obs-scene.h>
#include <obs-data.h>
#include "dc-capture.h"
#include "cJSON.h"
#include "window-helpers.h"



#define TEXT_WINDOW_CAPTURE obs_module_text("BoomCapture2D")
#define TEXT_WINDOW         obs_module_text("VRWindowCapture.Window")
#define TEXT_MATCH_PRIORITY obs_module_text("VRWindowCapture.Priority")
#define TEXT_MATCH_TITLE    obs_module_text("VRWindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS    obs_module_text("VRWindowCapture.Priority.Class")
#define TEXT_MATCH_EXE      obs_module_text("VRWindowCapture.Priority.Exe")
#define TEXT_CAPTURE_CURSOR obs_module_text("CaptureCursor")
#define TEXT_COMPATIBILITY  obs_module_text("Compatibility")

#define RESIZE_CHECK_TIME 0.2f










//VR Window Capture Struct
struct vr_window_capture {
	obs_source_t         *source;

	char                 *classname;
	bool                 cursor;
	char				 *dirpath;
	bool				 compatibility;

	struct dc_capture    capture;
	struct obs_video_info *ovi;
	bool				started;

	float                resize_timer;

	int toggle_position;

	HWND                 window;
	RECT                 last_rect;
};




//Vr hotkey helper struct
struct vr_hotkey_helper {
	struct obs_scene_item * scene_item;
	int show_id;
	int hide_id;
	int reset_id;
};










//Update settings func
static void vr_update_settings(struct vr_window_capture *wc, obs_data_t *s)
{
	blog(LOG_WARNING, "vr_update_settings");

	bfree(wc->classname);

	wc->classname = bzalloc(255 * sizeof(char));
	strcpy(wc->classname, "BasicHLSL");
	//strcpy(wc->classname, "ReplayView");
	//strcpy(wc->classname, "CalcFrame");

	wc->cursor        = obs_data_get_bool(s, "cursor");
}




//Get JSON data
char* vr_get_json_data_from_file(const char* path)
{
	blog(LOG_WARNING, "vr_get_json_data_from_file");

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
bool vr_find_id_helper(void *data, size_t idx, obs_hotkey_t *hotkey)
{
	blog(LOG_WARNING, "vr_find_id_helper");

	char buf_show_hide[256] = "libobs.show_scene_item.";
	char buf_reset[256] = "VR.reset_position.";
	struct vr_hotkey_helper *aa = data;

	
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
struct obs_scene* vr_get_scene(void *data)
{
	blog(LOG_WARNING, "vr_get_scene");

	struct vr_window_capture *wc = data;
	struct obs_source * scene_src;
	struct obs_scene * scene;
	char scene_name[256];

	if (wc->dirpath)
	{
		blog(LOG_WARNING, "		wc->dirpath = %s", wc->dirpath);

		char *text = vr_get_json_data_from_file(wc->dirpath);
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
struct obs_scene_item* vr_get_scene_item(void *data)
{
	blog(LOG_WARNING, "vr_get_scene_item");

	struct vr_window_capture *wc = data;
	struct obs_scene * scene = vr_get_scene(data);
	struct obs_scene_item * item;	

	if (scene) item = scene->first_item;

	while (item)
	{
		blog(LOG_INFO, "item context name: %s", item->source->context.name);
		blog(LOG_INFO, "wc context name: %s", wc->source->context.name);
		if (strcmp(item->source->context.name, wc->source->context.name) == 0)
		{
			break;
		}
		item = item->next;
	}

	return item;
}




//Set default position
void vr_set_default_postition(void *data)
{
	blog(LOG_WARNING, "vr_set_default_postition");

	struct vr_window_capture *wc = data;
	struct obs_scene_item * item = vr_get_scene_item(data);
	RECT rect;
	
	if (wc->ovi)
	{
		//blog(LOG_INFO, "Try to set the initial place!");
		GetClientRect(wc->window, &rect);
		struct vec2 *pos = bzalloc(sizeof(struct vec2));
		//blog(LOG_INFO, "The rect of the captured window is: top: %d, left: %d, bottom: %d, right: %d", rect.top, rect.left, rect.bottom, rect.right);
	
		pos->x = wc->ovi->base_width - rect.right;
		pos->y = wc->ovi->base_height - rect.bottom;

		obs_sceneitem_set_pos(item, pos);
	}

	item = NULL;
	blog(LOG_WARNING, "vr_set_default_postition end");
}




//Hotkey positioning
static bool hotkey_pos_vr(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	blog(LOG_WARNING, "hotkey_pos_vr");

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct vr_window_capture *wc = data;

	if (pressed && wc) {

		vr_set_default_postition(wc);

		return true;
	}

	return false;
}




//Init
void vr_init(void *data)
{
	blog(LOG_WARNING, "vr_init");

	struct vr_window_capture *wc = data;
	struct obs_scene * scene = vr_get_scene(data);
	struct obs_scene_item * item = vr_get_scene_item(data);


	//Set unvisible
	obs_sceneitem_set_visible(item, true);


	//Register toggle position hotkey function
	char id[256] = "";
	char name[256] = "VR.reset_position.";
	char desc[256] = "Reset ";

	strcat(id, wc->source->context.name);
	strcat(name, id);
	strcat(desc, id);

	wc->toggle_position = obs_hotkey_register_source(scene->source, name, desc, hotkey_pos_vr, wc);

	
	//Set hotkeys to turn visibility
	obs_key_combination_t *keycomb_show_hide = bzalloc(sizeof(obs_key_combination_t));
	keycomb_show_hide->key = OBS_KEY_F7;
	keycomb_show_hide->modifiers = 0;

	obs_key_combination_t *keycomb_reset = bzalloc(sizeof(obs_key_combination_t));
	keycomb_reset->key = OBS_KEY_F8;
	keycomb_reset->modifiers = 0;

	struct vr_hotkey_helper *aa = bzalloc(sizeof(struct vr_hotkey_helper));

	aa->scene_item = item;
	aa->show_id = -1;
	aa->hide_id = -1;
	aa->reset_id = -1;

	obs_enum_hotkeys(vr_find_id_helper, aa);

	obs_hotkey_load_bindings(aa->show_id, keycomb_show_hide, 1);
	obs_hotkey_load_bindings(aa->hide_id, keycomb_show_hide, 1);
	obs_hotkey_load_bindings(aa->reset_id, keycomb_reset, 1);

	item = NULL;
	scene = NULL;
}
/* ------------------------------------------------------------------------- */












//Create plugin
static void *wc_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_WARNING, "wc_create");

	struct vr_window_capture *wc = bzalloc(sizeof(struct vr_window_capture));
	wc->source = source;

	wc->ovi = bzalloc(sizeof(struct obs_video_info));

	obs_get_video_info(wc->ovi);

	char buf[32];
	wchar_t wbuf[255];

	wc->dirpath = malloc(255 * sizeof(char));
	strcpy(wc->dirpath, getenv("APPDATA"));
	strcat(wc->dirpath, "\\obs-studio\\basic\\scenes\\");

	MultiByteToWideChar(CP_ACP, 0, wc->dirpath, strlen(wc->dirpath), wbuf, strlen(wc->dirpath));
	wbuf[strlen(wc->dirpath)] = L'\0';

	lstrcatW(wbuf, L"*.json");

	WIN32_FIND_DATA FindFileData;
	HANDLE hFind = FindFirstFile(wbuf, &FindFileData);

	WideCharToMultiByte(CP_ACP, 0, FindFileData.cFileName, lstrlenW(FindFileData.cFileName), buf, lstrlenW(FindFileData.cFileName), NULL, NULL);
	buf[lstrlenW(FindFileData.cFileName)] = '\0';

	strcat(wc->dirpath, buf);

	FindClose(hFind);

	wc->started = false;
	wc->compatibility = false;



	vr_update_settings(wc, settings);
	return wc;
}










//Destroy plugin
static void wc_destroy(void *data)
{
	blog(LOG_WARNING, "wc_destroy");

	struct vr_window_capture *wc = data;

	if (wc) 
	{
		obs_enter_graphics();
		dc_capture_free(&wc->capture);
		obs_leave_graphics();

		obs_hotkey_unregister(wc->toggle_position);

		bfree(wc->classname);

		bfree(wc);
	}
}










//Get plugin name
static const char *wc_getname(void *unused)
{
	blog(LOG_WARNING, "wc_getname");

	UNUSED_PARAMETER(unused);
	return TEXT_WINDOW_CAPTURE;
}










//Update settings
static void wc_update(void *data, obs_data_t *settings)
{
	//blog(LOG_WARNING, "wc_update");

	struct vr_window_capture *wc = data;
	vr_update_settings(wc, settings);

	
	/* forces a reset */
	wc->window = NULL;
}










//Get source width
static uint32_t wc_width(void *data)
{
	//blog(LOG_WARNING, "wc_width");

	struct vr_window_capture *wc = data;
	return wc->capture.width;
}










//Get source height
static uint32_t wc_height(void *data)
{
	//blog(LOG_WARNING, "wc_height");

	struct vr_window_capture *wc = data;
	return wc->capture.height;
}










//Default properties
static void wc_defaults(obs_data_t *defaults)
{
	//blog(LOG_WARNING, "wc_height");

	obs_data_set_default_bool(defaults, "cursor", true);
	obs_data_set_default_bool(defaults, "compatibility", false);
}










//Property window
static obs_properties_t *wc_properties(void *unused)
{
	//blog(LOG_WARNING, "wc_height");

	UNUSED_PARAMETER(unused);

	obs_properties_t *ppts = obs_properties_create();

	obs_properties_add_bool(ppts, "cursor", TEXT_CAPTURE_CURSOR);

	return ppts;
}




// if the window title is still the same
static inline bool is_valid(void *data) 
{
	//blog(LOG_INFO, "Trying to check if the window title is not changed!");
	struct vr_window_capture *wc = data;
	bool valid = false;
	if (wc->window && IsWindow(wc->window) && wc->classname) {
		struct dstr cur_class = { 0 };
		get_window_title(&cur_class, wc->window);
		if (dstr_cmpi(&cur_class, wc->classname) == 0) {
		//	blog(LOG_INFO, "The window title is not changed!");
			valid = true;
		}
	}
	return valid;
}




//Application tick
static void wc_tick(void *data, float seconds)
{
	//blog(LOG_WARNING, "wc_tick");

	struct vr_window_capture *wc = data;
	RECT rect;
	bool reset_capture = false;


	//If source not showing skip tick
	if (!obs_source_showing(wc->source)) 
		return;


	//If we do no find window yet
	if (!wc->window || !IsWindow(wc->window) || !is_valid(data)) 
	{
		if (!wc->classname)
			return;

		wc->window = find_window(INCLUDE_MINIMIZED, wc->classname);
		if (!wc->window) 
		{
			blog(LOG_INFO, "Not found window!!!!");
			if (wc->capture.valid)
				dc_capture_free(&wc->capture);
			return;
		}
		else 
		{
			blog(LOG_INFO, "We find a window!!!!");
		}

		reset_capture = true;

	} 
	else if (IsIconic(wc->window)) 
	{
		return;
	}


	//Start view
	obs_enter_graphics();


	//Set initial settings
	if (!wc->started)
	{
		//Init
		vr_init(data);

		//Set position
		vr_set_default_postition(data);

		//Enable started flag
		wc->started = true;
	}


	//Get window size
	GetClientRect(wc->window, &rect);




	//Update window
	if (!reset_capture) {
		wc->resize_timer += seconds;

		if (wc->resize_timer >= RESIZE_CHECK_TIME) {
			if (rect.bottom != wc->last_rect.bottom ||
			    rect.right  != wc->last_rect.right)
				reset_capture = true;

			wc->resize_timer = 0.0f;
		}
	}

	if (reset_capture) 
	{
		wc->resize_timer = 0.0f;
		wc->last_rect = rect;
		dc_capture_free(&wc->capture);
		dc_capture_init(&wc->capture, 0, 0, rect.right, rect.bottom, wc->cursor, wc->compatibility);
	}

	dc_capture_capture(&wc->capture, wc->window);
	obs_leave_graphics();
}




//Render frame
static void wc_render(void *data, gs_effect_t *effect)
{
	//blog(LOG_WARNING, "wc_render");

	struct vr_window_capture *wc = data;
	
	dc_capture_render(&wc->capture, obs_get_base_effect(OBS_EFFECT_OPAQUE));
	UNUSED_PARAMETER(effect);
}




//Vr capture info
struct obs_source_info window_capture_info = {
	.id             = "vr_window_capture",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = wc_getname,
	.create         = wc_create,
	.destroy        = wc_destroy,
	.update         = wc_update,
	.video_render   = wc_render,
	.video_tick     = wc_tick,
	.get_width      = wc_width,
	.get_height     = wc_height,
	.get_defaults   = wc_defaults,
	.get_properties = wc_properties
};