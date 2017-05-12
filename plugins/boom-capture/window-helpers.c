#define PSAPI_VERSION 1
#include <obs.h>
#include <util/dstr.h>

#include <windows.h>
#include <psapi.h>
#include "window-helpers.h"
#include "obfuscate.h"

static inline void encode_dstr(struct dstr *str)
{
	dstr_replace(str, "#", "#22");
	dstr_replace(str, ":", "#3A");
}

static inline char *decode_str(const char *src)
{
	struct dstr str = { 0 };
	dstr_copy(&str, src);
	dstr_replace(&str, "#3A", ":");
	dstr_replace(&str, "#22", "#");
	return str.array;
}

extern void build_window_strings(const char *str,
	char **class,
	char **title,
	char **exe)
{
	char **strlist;

	*class = NULL;
	*title = NULL;
	*exe = NULL;

	if (!str) {
		return;
	}

	strlist = strlist_split(str, ':', true);

	if (strlist && strlist[0] && strlist[1] && strlist[2]) {
		*title = decode_str(strlist[0]);
		*class = decode_str(strlist[1]);
		*exe = decode_str(strlist[2]);
	}

	strlist_free(strlist);
}

static HMODULE kernel32(void)
{
	static HMODULE kernel32_handle = NULL;
	if (!kernel32_handle)
		kernel32_handle = GetModuleHandleA("kernel32");
	return kernel32_handle;
}

static inline HANDLE open_process(DWORD desired_access, bool inherit_handle,
	DWORD process_id)
{
	static HANDLE(WINAPI *open_process_proc)(DWORD, BOOL, DWORD) = NULL;
	if (!open_process_proc)
		open_process_proc = get_obfuscated_func(kernel32(),
			"B}caZyah`~q", 0x2D5BEBAF6DDULL);

	return open_process_proc(desired_access, inherit_handle, process_id);
}

bool get_window_exe(struct dstr *name, HWND window)
{
	wchar_t     wname[MAX_PATH];
	struct dstr temp = { 0 };
	bool        success = false;
	HANDLE      process = NULL;
	char        *slash;
	DWORD       id;

	GetWindowThreadProcessId(window, &id);
	if (id == GetCurrentProcessId())
		return false;

	process = open_process(PROCESS_QUERY_LIMITED_INFORMATION, false, id);
	if (!process)
		goto fail;

	if (!GetProcessImageFileNameW(process, wname, MAX_PATH))
		goto fail;

	dstr_from_wcs(&temp, wname);
	slash = strrchr(temp.array, '\\');
	if (!slash)
		goto fail;

	dstr_copy(name, slash + 1);
	success = true;

fail:
	if (!success)
		dstr_copy(name, "unknown");

	dstr_free(&temp);
	CloseHandle(process);
	return true;
}

void get_window_title(struct dstr *name, HWND hwnd)
{
	wchar_t *temp;
	int len;

	len = GetWindowTextLengthW(hwnd);
	if (!len)
		return;

	temp = malloc(sizeof(wchar_t) * (len + 1));
	if (GetWindowTextW(hwnd, temp, len + 1))
		dstr_from_wcs(name, temp);
	free(temp);
}

void get_window_class(struct dstr *class, HWND hwnd)
{
	wchar_t temp[256];

	temp[0] = 0;
	if (GetClassNameW(hwnd, temp, sizeof(temp) / sizeof(wchar_t)))
		dstr_from_wcs(class, temp);
}

static void add_window(obs_property_t *p, HWND hwnd)
{
	struct dstr class = { 0 };
	struct dstr title = { 0 };
	struct dstr exe = { 0 };
	struct dstr encoded = { 0 };
	struct dstr desc = { 0 };

	if (!get_window_exe(&exe, hwnd))
		return;
	get_window_title(&title, hwnd);
	get_window_class(&class, hwnd);

	dstr_printf(&desc, "[%s]: %s", exe.array, title.array);

	encode_dstr(&title);
	encode_dstr(&class);
	encode_dstr(&exe);

	dstr_cat_dstr(&encoded, &title);
	dstr_cat(&encoded, ":");
	dstr_cat_dstr(&encoded, &class);
	dstr_cat(&encoded, ":");
	dstr_cat_dstr(&encoded, &exe);

	obs_property_list_add_string(p, desc.array, encoded.array);

	dstr_free(&encoded);
	dstr_free(&desc);
	dstr_free(&class);
	dstr_free(&title);
	dstr_free(&exe);
}




static bool check_window_valid(HWND window, enum window_search_mode mode)
{
	DWORD styles, ex_styles;
	RECT  rect;

	if (!IsWindowVisible(window) ||
		(mode == EXCLUDE_MINIMIZED && IsIconic(window)))
		return false;

	GetClientRect(window, &rect);
	styles = (DWORD)GetWindowLongPtr(window, GWL_STYLE);
	ex_styles = (DWORD)GetWindowLongPtr(window, GWL_EXSTYLE);

	if (ex_styles & WS_EX_TOOLWINDOW)
		return false;
	if (styles & WS_CHILD)
		return false;
	if (mode == EXCLUDE_MINIMIZED && (rect.bottom == 0 || rect.right == 0))
		return false;

	return true;
}

static inline HWND next_window(HWND window, enum window_search_mode mode)
{
	while (true) {
		window = GetNextWindow(window, GW_HWNDNEXT);
		if (!window || check_window_valid(window, mode))
			break;
	}

	return window;
}

static inline HWND first_window(enum window_search_mode mode)
{
	HWND window = GetWindow(GetDesktopWindow(), GW_CHILD);
	if (!check_window_valid(window, mode))
		window = next_window(window, mode);
	return window;
}

void fill_window_list(obs_property_t *p, enum window_search_mode mode)
{
	HWND window = first_window(mode);

	while (window) {
		add_window(p, window);
		window = next_window(window, mode);
	}
}

static bool window_rating(HWND window, const char *title)
{
	struct dstr cur_class = { 0 };
	struct dstr cur_title = { 0 };
	struct dstr cur_exe = { 0 };
	bool equals = false;

	if (!get_window_exe(&cur_exe, window))
		return 0;
	get_window_title(&cur_title, window);
	get_window_class(&cur_class, window);

	if (dstr_cmpi(&cur_title, title) == 0)
		equals = true;

	dstr_free(&cur_class);
	dstr_free(&cur_title);
	dstr_free(&cur_exe);

	return equals;
}

HWND find_window(enum window_search_mode mode, const char *title)
{
	HWND window = first_window(mode);
	HWND best_window = NULL;

	while (window) {
		if (window_rating(window, title))
		{
			best_window = window;
			break;
		}

		window = next_window(window, mode);
	}

	return best_window;
}