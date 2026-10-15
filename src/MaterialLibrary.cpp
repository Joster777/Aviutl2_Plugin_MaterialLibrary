//----------------------------------------------------------------------------------
//	MaterialLibrary (汎用素材) for AviUtl ExEdit2   version 0.3
//
//	アプリケーションデータフォルダ\MaterialLibrary\<種類>\<素材ファイル> に置いた
//	効果音・BGM・画像・動画などを、設定画面のドロップダウンから選んで使えるようにする汎用プラグインです。
//
//	- 追加メニューには「汎用素材」だけが表示されます
//	- 選んだ素材の形式(音声/画像/動画)に合わせて、オブジェクトが自動的にその形式のオブジェクトに切り替わります
//	  (タイムライン上の色も 音声=赤 / 画像=青 / 動画=青赤グラデ になります)
//	    汎用素材(音声) / 汎用素材(画像) / 汎用素材(動画) ... 内部用 (追加メニューには表示されません)
//	- 新規オブジェクトは「(未選択)」の状態で作られ、種類と素材をドロップダウンで選ぶまで何も読み込みません
//	- 編集メニュー「汎用素材を再生バーの位置に生成」で、再生バー位置にオブジェクトを作れます (ショートカット割り当て可)
//	- 種類を選ぶと、その種類の素材だけが表示されます
//	- 素材を選ぶとオブジェクトの長さを素材の長さに合わせ、オブジェクト名を素材のファイル名にします
//	- 音声・動画は素材本来の長さより長く引き延ばせません (ループ再生がONのときを除く)
//	- 「素材の登録」から、ファイルを選んで素材ライブラリへ登録できます
//	  プレビュー画面などへファイルをD&Dした時の取り込み先としても登録されます
//	- 素材一覧はAviUtl2の起動時に読み込まれます
//----------------------------------------------------------------------------------
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "filter2.h"
#include "plugin2.h"
#include "cache2.h"
#include "config2.h"
#include "logger2.h"

namespace fs = std::filesystem;

//---------------------------------------------------------------------
//	定数
//---------------------------------------------------------------------
// get_audio_file_data() が返す音声が、シーンのサンプリングレートに変換済みかどうか
// 1 : 変換済みとして扱う (通常はこちら)
// 0 : 素材ファイル本来のサンプリングレートとして扱い、このプラグインで線形補間して変換する
// ※再生した音の高さや長さがずれる場合は、この値を切り替えてください
#define MATERIAL_LIBRARY_HOST_RESAMPLES 1

// 1 : 素材を選んだ時にオブジェクト名を素材のファイル名にする
// 0 : オブジェクト名は変更しない
#define MATERIAL_LIBRARY_AUTO_NAME 1

static const wchar_t* const PLUGIN_LABEL = L"汎用素材";
static const wchar_t* const PLUGIN_INFO = L"MaterialLibrary version 0.3";
static const wchar_t* const FOLDER_NAME = L"MaterialLibrary";
static const wchar_t* const UNSELECTED_LABEL = L"(未選択)";
static const wchar_t* const ITEM_TYPE = L"種類";
static const wchar_t* const ITEM_VOLUME = L"音量";
static const wchar_t* const ITEM_PAN = L"左右";
static const wchar_t* const ITEM_START = L"再生位置";
static const wchar_t* const ITEM_LOOP = L"ループ再生";
static const wchar_t* const ITEM_IMPORT_FILE = L"登録するファイル";

// 素材の形式
enum : int
{
	MEDIA_AUDIO = 1,
	MEDIA_IMAGE = 2,
	MEDIA_VIDEO = 4,
	MEDIA_ALL = MEDIA_AUDIO | MEDIA_IMAGE | MEDIA_VIDEO,
};

//---------------------------------------------------------------------
//	ホストから受け取るハンドル
//---------------------------------------------------------------------
static HOST_APP_TABLE* g_host = nullptr;
static EDIT_HANDLE* g_edit = nullptr;
static CACHE_HANDLE* g_cache = nullptr;
static CONFIG_HANDLE* g_config = nullptr;
static LOG_HANDLE* g_logger = nullptr;

static void log_text(LPCWSTR text)
{
	if (g_logger)
	{
		g_logger->log(g_logger, text);
	}
}

//---------------------------------------------------------------------
//	文字列ユーティリティ
//---------------------------------------------------------------------
static std::wstring utf8_to_wide(const char* text)
{
	if (!text || !*text)
	{
		return std::wstring();
	}
	int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (length <= 1)
	{
		return std::wstring();
	}
	std::wstring result((size_t)length - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], length);
	return result;
}

static std::wstring to_lower(std::wstring text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c)
		{
			return (wchar_t)towlower(c);
		});
	return text;
}

static bool is_all_digits(const std::wstring& text)
{
	if (text.empty())
	{
		return false;
	}
	for (wchar_t c : text)
	{
		if (c < L'0' || c > L'9')
		{
			return false;
		}
	}
	return true;
}

//---------------------------------------------------------------------
//	素材ライブラリのデータ
//---------------------------------------------------------------------
struct Material
{
	int id;
	int type;				// MEDIA_AUDIO / MEDIA_IMAGE / MEDIA_VIDEO
	std::wstring name;		// 表示名 (種類フォルダからの相対パス)
	std::wstring path;		// ファイルのフルパス
};

struct Category
{
	int id;
	std::wstring name;		// 種類名 (フォルダ名)
	std::vector<Material> materials;
};

static std::wstring g_library_root;
static std::set<std::wstring> g_material_stems;	// 素材のファイル名(拡張子なし)
static std::vector<Category> g_library;		// フォルダにある全ての素材

static bool in_list(const std::wstring& ext, const wchar_t* const* list, size_t count)
{
	for (size_t i = 0; i < count; i++)
	{
		if (ext == list[i])
		{
			return true;
		}
	}
	return false;
}

// 拡張子から素材の形式を求める (対応していない場合は0)
static int classify_extension(const std::wstring& ext)
{
	static const wchar_t* const images[] = { L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp", L".tga" };
	static const wchar_t* const audios[] = { L".wav", L".mp3", L".ogg", L".flac", L".m4a", L".aac", L".wma", L".opus" };
	static const wchar_t* const videos[] = { L".mp4", L".mov", L".avi", L".mkv", L".webm", L".wmv", L".flv", L".m4v", L".mpg", L".mpeg", L".mts", L".m2ts" };
	if (in_list(ext, images, std::size(images)))
	{
		return MEDIA_IMAGE;
	}
	if (in_list(ext, audios, std::size(audios)))
	{
		return MEDIA_AUDIO;
	}
	if (in_list(ext, videos, std::size(videos)))
	{
		return MEDIA_VIDEO;
	}
	return 0;
}

// 名前から、プロジェクトに保存されても変わりにくいIDを作る
static int make_id(const std::wstring& name, std::vector<int>& used)
{
	unsigned int hash = 2166136261u;
	for (wchar_t c : to_lower(name))
	{
		hash ^= (unsigned int)c;
		hash *= 16777619u;
	}
	int id = (int)(hash & 0x7fffffff);
	if (id == 0)
	{
		id = 1;
	}
	while (std::find(used.begin(), used.end(), id) != used.end())
	{
		id = (id % 0x7ffffffe) + 1;
	}
	used.push_back(id);
	return id;
}

static void scan_library()
{
	g_library.clear();
	g_material_stems.clear();

	std::wstring app_data = (g_config && g_config->app_data_path) ? g_config->app_data_path : L"";
	if (app_data.empty())
	{
		return;
	}
	fs::path root = fs::path(app_data) / FOLDER_NAME;
	g_library_root = root.wstring();

	std::error_code ec;
	if (!fs::exists(root, ec))
	{
		// 初回起動時は既定の種類フォルダを作る
		fs::create_directories(root / L"SE", ec);
		fs::create_directories(root / L"BGM", ec);
		fs::create_directories(root / L"Image", ec);
	}

	std::vector<fs::path> directories;
	for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code ec2;
		if (it->is_directory(ec2))
		{
			directories.push_back(it->path());
		}
	}
	std::sort(directories.begin(), directories.end(), [](const fs::path& a, const fs::path& b)
		{
			return to_lower(a.filename().wstring()) < to_lower(b.filename().wstring());
		});

	std::vector<int> used_category_ids;
	for (const auto& dir : directories)
	{
		Category category;
		category.name = dir.filename().wstring();
		category.id = make_id(category.name, used_category_ids);

		std::vector<int> used_material_ids;
		std::vector<Material> materials;
		std::error_code ec3;
		for (fs::recursive_directory_iterator it(dir, ec3), end; !ec3 && it != end; it.increment(ec3))
		{
			std::error_code ec4;
			if (!it->is_regular_file(ec4))
			{
				continue;
			}
			int type = classify_extension(to_lower(it->path().extension().wstring()));
			if (type == 0)
			{
				continue;
			}
			Material material;
			material.name = fs::relative(it->path(), dir, ec4).generic_wstring();
			material.path = it->path().wstring();
			material.type = type;
			g_material_stems.insert(fs::path(material.name).stem().wstring());
			material.id = 0;
			materials.push_back(material);
		}
		std::sort(materials.begin(), materials.end(), [](const Material& a, const Material& b)
			{
				return to_lower(a.name) < to_lower(b.name);
			});
		for (auto& material : materials)
		{
			material.id = make_id(material.name, used_material_ids);
		}
		category.materials = std::move(materials);
		g_library.push_back(std::move(category));
	}
}

//---------------------------------------------------------------------
//	オブジェクトの種類 (音声 / 画像 / 動画 / 旧互換)
//	タイムラインでのオブジェクトの色は、本体がプラグインの FLAG_VIDEO / FLAG_AUDIO から決めるので
//	素材の形式ごとに別のフィルタプラグインとして登録します
//---------------------------------------------------------------------
static void on_register_button(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item);
static void on_fit_button(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item);

static const int KIND_COUNT = 4;

static const wchar_t* const IMPORT_FILE_FILTER =
L"対応する素材ファイル\0"
L"*.wav;*.mp3;*.ogg;*.flac;*.m4a;*.aac;*.wma;*.opus;"
L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tga;"
L"*.mp4;*.mov;*.avi;*.mkv;*.webm;*.wmv;*.flv;*.m4v;*.mpg;*.mpeg;*.mts;*.m2ts\0"
L"All Files (*.*)\0*.*\0";

struct Kind
{
	int index;
	const wchar_t* effect_name;	// エフェクト名 (オブジェクトに保存される名前)
	int media_type;				// このオブジェクトが扱う形式 (MEDIA_AUDIO / MEDIA_IMAGE / MEDIA_VIDEO / 0=形式を選ぶ前の入口)
	int mask;					// ドロップダウンに出す素材の形式 (全形式)
	int flag;					// FILTER_PLUGIN_TABLE のフラグ
	bool has_video;
	bool has_audio;
	std::vector<Category> categories;	// mask に合う素材だけの一覧

	// 種類ごとに数が変わる項目は、起動時に作る
	std::deque<std::wstring> strings;
	std::deque<std::vector<FILTER_ITEM_SELECT::ITEM>> lists;
	std::deque<FILTER_ITEM_SELECT> selects;
	std::deque<FILTER_ITEM_HIDE_RULE> hide_rules;
	FILTER_ITEM_SELECT* type_select = nullptr;
	std::vector<FILTER_ITEM_SELECT*> material_selects;
	std::vector<std::wstring> material_select_names;
	std::vector<void*> items;

	FILTER_ITEM_SEPARATOR separator_source{ L"素材" };
	FILTER_ITEM_SEPARATOR separator_play{ L"再生" };
	FILTER_ITEM_TRACK track_volume{ ITEM_VOLUME, 100.0, 0.0, 200.0, 0.1 };
	FILTER_ITEM_TRACK track_pan{ ITEM_PAN, 0.0, -100.0, 100.0, 0.1 };
	FILTER_ITEM_TRACK track_start{ ITEM_START, 0.0, 0.0, 3600.0, 0.001 };
	FILTER_ITEM_CHECK check_loop{ ITEM_LOOP, false };
	FILTER_ITEM_BUTTON button_fit{ L"長さを素材に合わせる", on_fit_button };
	FILTER_ITEM_GROUP group_register{ L"素材の登録", false };
	FILTER_ITEM_FILE file_import{ ITEM_IMPORT_FILE, L"", IMPORT_FILE_FILTER };
	FILTER_ITEM_BUTTON button_register{ L"ライブラリに登録", on_register_button };
	FILTER_ITEM_GROUP group_end{ L"" };

	FILTER_PLUGIN_TABLE table = {};

	Kind(int index, const wchar_t* effect_name, int media_type, int flag)
		: index(index), effect_name(effect_name), media_type(media_type), mask(MEDIA_ALL), flag(flag)
	{
		has_video = (flag & FILTER_PLUGIN_TABLE::FLAG_VIDEO) != 0;
		has_audio = (flag & FILTER_PLUGIN_TABLE::FLAG_AUDIO) != 0;
	}

	LPCWSTR keep_string(const std::wstring& text)
	{
		strings.push_back(text);
		return strings.back().c_str();
	}

	FILTER_ITEM_SELECT* make_select(const std::wstring& name, int value, std::vector<FILTER_ITEM_SELECT::ITEM> list)
	{
		list.push_back({ nullptr, 0 });
		lists.push_back(std::move(list));
		selects.emplace_back(keep_string(name), value, lists.back().data());
		return &selects.back();
	}

	// 設定項目を作る
	void build()
	{
		// この形式の素材がある種類だけを残す
		categories.clear();
		for (const auto& source : g_library)
		{
			Category category;
			category.id = source.id;
			category.name = source.name;
			for (const auto& material : source.materials)
			{
				if (material.type & mask)
				{
					category.materials.push_back(material);
				}
			}
			if (!category.materials.empty())
			{
				categories.push_back(std::move(category));
			}
		}

		items.clear();
		material_selects.clear();
		material_select_names.clear();

		items.push_back(&separator_source);

		// 種類の選択 (先頭は「未選択」。初期値も未選択にして、選ぶまで素材を読み込まない)
		std::vector<FILTER_ITEM_SELECT::ITEM> type_list;
		type_list.push_back({ UNSELECTED_LABEL, 0 });
		for (const auto& category : categories)
		{
			type_list.push_back({ keep_string(category.name), category.id });
		}
		type_select = make_select(ITEM_TYPE, 0, type_list);
		items.push_back(type_select);

		// 種類ごとの素材の選択 (こちらも先頭は「未選択」)
		for (const auto& category : categories)
		{
			std::vector<FILTER_ITEM_SELECT::ITEM> list;
			list.push_back({ UNSELECTED_LABEL, 0 });
			for (const auto& material : category.materials)
			{
				list.push_back({ keep_string(material.name), material.id });
			}
			int default_id = 0;
			std::wstring name = L"素材(" + category.name + L")";
			material_select_names.push_back(name);
			auto select = make_select(name, default_id, list);
			material_selects.push_back(select);
			items.push_back(select);
		}

		// 選んだ種類以外の素材リストを隠す
		for (size_t i = 0; i < categories.size(); i++)
		{
			hide_rules.emplace_back(material_selects[i]->name, ITEM_TYPE, FILTER_ITEM_HIDE_RULE::OPERATOR::NOT_EQUAL, categories[i].id);
			items.push_back(&hide_rules.back());
		}

		if (media_type != MEDIA_IMAGE)
		{
			items.push_back(&button_fit);
			items.push_back(&separator_play);
			if (has_audio)
			{
				items.push_back(&track_volume);
				items.push_back(&track_pan);
			}
			items.push_back(&track_start);
			items.push_back(&check_loop);
		}

		items.push_back(&group_register);
		items.push_back(&file_import);
		items.push_back(&button_register);
		items.push_back(&group_end);

		items.push_back(nullptr);
	}
};

static Kind* g_kinds[KIND_COUNT] = {};

// オブジェクトが持っているエフェクトから種類を求める
static Kind* find_object_kind(EDIT_SECTION* edit, OBJECT_HANDLE object)
{
	if (!object)
	{
		return nullptr;
	}
	for (Kind* kind : g_kinds)
	{
		if (kind && edit->count_object_effect(object, kind->effect_name) > 0)
		{
			return kind;
		}
	}
	return nullptr;
}

// 素材の形式に対応するオブジェクトの種類
static Kind* kind_for_media(int type)
{
	switch (type)
	{
	case MEDIA_AUDIO: return g_kinds[0];
	case MEDIA_IMAGE: return g_kinds[1];
	case MEDIA_VIDEO: return g_kinds[2];
	}
	return nullptr;
}

static std::string wide_to_utf8(const std::wstring& text)
{
	if (text.empty())
	{
		return std::string();
	}
	int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
	std::string result((size_t)length, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &result[0], length, nullptr, nullptr);
	return result;
}

//---------------------------------------------------------------------
//	設定値から素材を求める
//---------------------------------------------------------------------
static const Category* find_category_by_text(const Kind& kind, const std::wstring& text)
{
	for (const auto& category : kind.categories)
	{
		if (category.name == text)
		{
			return &category;
		}
	}
	if (is_all_digits(text))
	{
		int id = (int)std::wcstol(text.c_str(), nullptr, 10);
		for (const auto& category : kind.categories)
		{
			if (category.id == id)
			{
				return &category;
			}
		}
	}
	return nullptr;
}

static const Material* find_material_by_text(const Category& category, const std::wstring& text)
{
	for (const auto& material : category.materials)
	{
		if (material.name == text)
		{
			return &material;
		}
	}
	if (is_all_digits(text))
	{
		int id = (int)std::wcstol(text.c_str(), nullptr, 10);
		for (const auto& material : category.materials)
		{
			if (material.id == id)
			{
				return &material;
			}
		}
	}
	return nullptr;
}

// 現在の設定から素材を求める (フィルタ処理中用)
static const Material* resolve_runtime_material(const Kind& kind)
{
	// 種類または素材が未選択の間は、何も読み込まない (先頭の素材に自動で代替しない)
	if (kind.categories.empty() || !kind.type_select)
	{
		return nullptr;
	}
	for (size_t i = 0; i < kind.categories.size(); i++)
	{
		if (kind.categories[i].id != kind.type_select->value)
		{
			continue;
		}
		int material_id = kind.material_selects[i]->value;
		if (material_id == 0)
		{
			return nullptr;
		}
		for (const auto& material : kind.categories[i].materials)
		{
			if (material.id == material_id)
			{
				return &material;
			}
		}
		return nullptr;
	}
	return nullptr;
}

//---------------------------------------------------------------------
//	メディアファイル情報のキャッシュ
//---------------------------------------------------------------------
static std::mutex g_info_mutex;
static std::map<std::wstring, AUDIO_INFO> g_audio_infos;
static std::map<std::wstring, VIDEO_INFO> g_video_infos;

static bool get_audio_info(const std::wstring& path, AUDIO_INFO* info)
{
	if (!g_cache)
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(g_info_mutex);
		auto it = g_audio_infos.find(path);
		if (it != g_audio_infos.end())
		{
			*info = it->second;
			return true;
		}
	}
	AUDIO_INFO result = {};
	if (!g_cache->get_audio_file_info(path.c_str(), &result, sizeof(result)) || result.rate <= 0)
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(g_info_mutex);
		g_audio_infos[path] = result;
	}
	*info = result;
	return true;
}

static bool get_video_info(const std::wstring& path, VIDEO_INFO* info)
{
	if (!g_cache)
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(g_info_mutex);
		auto it = g_video_infos.find(path);
		if (it != g_video_infos.end())
		{
			*info = it->second;
			return true;
		}
	}
	VIDEO_INFO result = {};
	if (!g_cache->get_video_file_info(path.c_str(), &result, sizeof(result)) || result.width <= 0 || result.height <= 0)
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(g_info_mutex);
		g_video_infos[path] = result;
	}
	*info = result;
	return true;
}

// 素材本来の長さ(秒)を求める (画像など長さが決まっていない素材はfalse)
static bool get_material_seconds(const Material& material, double* seconds)
{
	if (material.type == MEDIA_IMAGE)
	{
		return false;
	}
	double result = 0.0;
	if (material.type == MEDIA_VIDEO)
	{
		VIDEO_INFO video = {};
		if (get_video_info(material.path, &video) && video.total_time > 0.0)
		{
			result = video.total_time;
		}
	}
	if (result <= 0.0)
	{
		AUDIO_INFO audio = {};
		if (get_audio_info(material.path, &audio) && audio.total_time > 0.0)
		{
			result = audio.total_time;
		}
	}
	if (result <= 0.0)
	{
		return false;
	}
	*seconds = result;
	return true;
}

//---------------------------------------------------------------------
//	画像フィルタ処理
//---------------------------------------------------------------------
static bool set_transparent(FILTER_PROC_VIDEO* video)
{
	static PIXEL_RGBA transparent = { 0, 0, 0, 0 };
	video->set_image_data(&transparent, 1, 1);
	return true;
}

// ファイルキャッシュから取得した画像を、現在のオブジェクトの画像として設定する
static bool apply_file_image(FILTER_PROC_VIDEO* video, CACHE_FILE_IMAGE& image)
{
	if (!image || image.width <= 0 || image.height <= 0)
	{
		return false;
	}
	int width = image.width;
	int height = image.height;
	const unsigned char* source = (const unsigned char*)image.buffer;
	int pitch = image.pitch;

	// よくある形式は自前でRGBAに変換して設定する
	if (image.format == INPUT_PIXEL_FORMAT::RGBA || image.format == INPUT_PIXEL_FORMAT::BGRA || image.format == INPUT_PIXEL_FORMAT::BGR)
	{
		std::vector<PIXEL_RGBA> buffer((size_t)width * height);
		for (int y = 0; y < height; y++)
		{
			const unsigned char* line = source + (size_t)y * pitch;
			PIXEL_RGBA* out = buffer.data() + (size_t)y * width;
			for (int x = 0; x < width; x++)
			{
				const unsigned char* p = line + (size_t)x * 4;
				if (image.format == INPUT_PIXEL_FORMAT::RGBA)
				{
					out[x] = { p[0], p[1], p[2], p[3] };
				}
				else if (image.format == INPUT_PIXEL_FORMAT::BGRA)
				{
					out[x] = { p[2], p[1], p[0], p[3] };
				}
				else
				{
					out[x] = { p[2], p[1], p[0], 255 };
				}
			}
		}
		video->set_image_data(buffer.data(), width, height);
		return true;
	}

	// それ以外の形式(YUY2/PA64/HF64/YC48)は本体に変換してもらう
	const wchar_t* temp = L"resource:MaterialLibrary_frame";
	if (!video->set_image_resource_data(temp, image.buffer, width, height, pitch, image.format))
	{
		return false;
	}
	bool copied = video->copy_image_resource(L"object", temp);
	video->release_image_resource(temp);
	return copied;
}

static bool proc_image(FILTER_PROC_VIDEO* video, const Material& material)
{
	// 画像ファイルをキャッシュ経由で読み込む
	// (get_image_resource_data() は "image:" のリソースを取得できないので使わない)
	if (g_cache)
	{
		auto image = g_cache->get_image_file_cache(material.path.c_str());
		if (apply_file_image(video, image))
		{
			return true;
		}
	}

	// キャッシュで読めない場合は、本体の画像リソースを現在のオブジェクトにコピーする
	std::wstring resource = L"image:" + material.path;
	int width = 0;
	int height = 0;
	if (video->get_image_resource_size(resource.c_str(), &width, &height) && width > 0 && height > 0)
	{
		video->set_image_data(nullptr, width, height);
		if (video->copy_image_resource(L"object", resource.c_str()))
		{
			return true;
		}
	}
	return set_transparent(video);
}

static bool proc_movie(Kind& kind, FILTER_PROC_VIDEO* video, const Material& material)
{
	VIDEO_INFO info = {};
	if (!get_video_info(material.path, &info))
	{
		return set_transparent(video);
	}

	// 取得する時間を求める
	double frame_time = (info.rate > 0 && info.scale > 0) ? (double)info.scale / (double)info.rate : (1.0 / 30.0);
	double time = video->object->time + kind.track_start.value;
	if (info.total_time > 0.0)
	{
		if (kind.check_loop.value)
		{
			time = std::fmod(time, info.total_time);
		}
		else
		{
			time = std::min(time, info.total_time - frame_time * 0.5);
		}
	}
	time = std::max(0.0, time);

	auto image = g_cache->get_video_file_cache_by_time(material.path.c_str(), 0, time);
	if (!apply_file_image(video, image))
	{
		return set_transparent(video);
	}
	return true;
}

static bool proc_video_impl(Kind& kind, FILTER_PROC_VIDEO* video)
{
	const Material* material = resolve_runtime_material(kind);
	if (!material || !g_cache)
	{
		return set_transparent(video);
	}
	switch (material->type)
	{
	case MEDIA_IMAGE:
		return proc_image(video, *material);
	case MEDIA_VIDEO:
		return proc_movie(kind, video, *material);
	default:
		// 音声素材のときは、見えない1x1の画像にする
		return set_transparent(video);
	}
}

//---------------------------------------------------------------------
//	音声フィルタ処理
//---------------------------------------------------------------------
// 素材ファイルからサンプルを読み込む (シーンのサンプリングレートで返す)
static int read_samples(const std::wstring& path, const AUDIO_INFO& info, int scene_rate, int64_t position, int count, float* left, float* right)
{
#if MATERIAL_LIBRARY_HOST_RESAMPLES
	(void)info;
	(void)scene_rate;
	return g_cache->get_audio_file_data(path.c_str(), 0, position, count, left, right);
#else
	// 素材ファイル本来のレートで読み込んで、線形補間でシーンのレートに変換する
	double ratio = (double)info.rate / (double)scene_rate;
	int64_t source_start = (int64_t)std::floor((double)position * ratio);
	int source_count = (int)std::ceil((double)count * ratio) + 2;
	std::vector<float> source_left((size_t)source_count, 0.0f);
	std::vector<float> source_right((size_t)source_count, 0.0f);
	int got = g_cache->get_audio_file_data(path.c_str(), 0, source_start, source_count, source_left.data(), source_right.data());
	if (got <= 0)
	{
		return 0;
	}
	int produced = 0;
	for (int i = 0; i < count; i++)
	{
		double source_position = (double)(position + i) * ratio - (double)source_start;
		int index = (int)std::floor(source_position);
		if (index + 1 >= got)
		{
			break;
		}
		float t = (float)(source_position - index);
		left[i] = source_left[index] * (1.0f - t) + source_left[index + 1] * t;
		right[i] = source_right[index] * (1.0f - t) + source_right[index + 1] * t;
		produced++;
	}
	return produced;
#endif
}

static bool proc_audio_impl(Kind& kind, FILTER_PROC_AUDIO* audio)
{
	int sample_num = audio->object->sample_num;
	int channel_num = audio->object->channel_num;
	if (sample_num <= 0)
	{
		return true;
	}

	std::vector<float> left((size_t)sample_num, 0.0f);
	std::vector<float> right((size_t)sample_num, 0.0f);

	const Material* material = resolve_runtime_material(kind);
	AUDIO_INFO info = {};
	if (material && material->type != MEDIA_IMAGE && g_cache && get_audio_info(material->path, &info))
	{
		int scene_rate = audio->scene->sample_rate;
		int64_t total = (int64_t)std::floor(info.total_time * (double)scene_rate);
		int64_t offset = (int64_t)std::floor(kind.track_start.value * (double)scene_rate);
		int64_t position = audio->object->sample_index + offset;
		bool loop = kind.check_loop.value;

		if (total > 0)
		{
			int done = 0;
			while (done < sample_num)
			{
				int64_t p = position + done;
				if (loop)
				{
					p %= total;
				}
				else if (p >= total)
				{
					break;
				}
				int64_t want = std::min<int64_t>((int64_t)sample_num - done, total - p);
				int got = read_samples(material->path, info, scene_rate, p, (int)want, left.data() + done, right.data() + done);
				if (got <= 0)
				{
					break;
				}
				done += got;
			}
		}

		// 音量と左右バランスを適用する
		double volume = kind.track_volume.value / 100.0;
		double pan = kind.track_pan.value / 100.0;
		float gain_left = (float)(volume * (pan > 0.0 ? 1.0 - pan : 1.0));
		float gain_right = (float)(volume * (pan < 0.0 ? 1.0 + pan : 1.0));
		for (int i = 0; i < sample_num; i++)
		{
			left[i] *= gain_left;
			right[i] *= gain_right;
		}
	}

	audio->set_sample_data(left.data(), 0);
	if (channel_num > 1)
	{
		audio->set_sample_data(right.data(), 1);
	}
	return true;
}

// フィルタ処理関数は種類ごとに別の関数にする (本体は呼び出し元の種類を渡してくれないため)
template<int K> static bool proc_video_k(FILTER_PROC_VIDEO* video)
{
	return proc_video_impl(*g_kinds[K], video);
}

template<int K> static bool proc_audio_k(FILTER_PROC_AUDIO* audio)
{
	return proc_audio_impl(*g_kinds[K], audio);
}

//---------------------------------------------------------------------
//	編集処理: オブジェクトの設定値を読む
//---------------------------------------------------------------------
static std::wstring read_item_text(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item)
{
	return utf8_to_wide(edit->get_object_item_value(object, effect, item));
}

static double read_item_number(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item)
{
	LPCSTR text = edit->get_object_item_value(object, effect, item);
	if (!text || !*text)
	{
		return 0.0;
	}
	return std::strtod(text, nullptr);
}

static bool read_item_check(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item)
{
	LPCSTR text = edit->get_object_item_value(object, effect, item);
	if (!text)
	{
		return false;
	}
	return std::strcmp(text, "1") == 0 || std::strcmp(text, "true") == 0;
}

// オブジェクトで選択されている素材を求める
static const Material* resolve_object_material(EDIT_SECTION* edit, const Kind& kind, OBJECT_HANDLE object)
{
	const Category* category = find_category_by_text(kind, read_item_text(edit, object, kind.effect_name, ITEM_TYPE));
	if (!category)
	{
		return nullptr;
	}
	size_t index = (size_t)(category - kind.categories.data());
	if (index >= kind.material_select_names.size())
	{
		return nullptr;
	}
	std::wstring text = read_item_text(edit, object, kind.effect_name, kind.material_select_names[index].c_str());
	return find_material_by_text(*category, text);
}

//---------------------------------------------------------------------
//	オブジェクトの長さを素材の長さに合わせる / 素材の長さを超えないようにする
//	allow_grow : trueなら素材が長い時にオブジェクトを延ばす / falseなら短くするだけ
//---------------------------------------------------------------------
static bool adjust_length(EDIT_SECTION* edit, OBJECT_HANDLE object, const Kind& kind, const Material& material, bool allow_grow)
{
	// 画像は長さが決まっていないので、自由に延ばせる
	if (material.type == MEDIA_IMAGE)
	{
		return false;
	}
	// ループ再生のときは、長さを自由に決めてよいので変更しない
	if (read_item_check(edit, object, kind.effect_name, ITEM_LOOP))
	{
		return false;
	}
	double seconds = 0.0;
	if (!get_material_seconds(material, &seconds))
	{
		return false;
	}

	double rate = (double)edit->info->rate;
	double scale = (double)edit->info->scale;
	if (rate <= 0.0 || scale <= 0.0)
	{
		return false;
	}

	double remain = std::max(0.0, seconds - read_item_number(edit, object, kind.effect_name, ITEM_START));
	int frames = std::max(1, (int)std::ceil(remain * rate / scale - 1e-6));

	OBJECT_LAYER_FRAME current = edit->get_object_layer_frame(object);
	int current_frames = current.end - current.start + 1;
	if (frames == current_frames)
	{
		return true;
	}
	if (frames > current_frames)
	{
		if (!allow_grow)
		{
			return true;
		}
		// 右に別のオブジェクトがある場合は、その手前までにする
		OBJECT_HANDLE next = edit->find_object(current.layer, current.end + 1);
		if (next)
		{
			OBJECT_LAYER_FRAME next_frame = edit->get_object_layer_frame(next);
			int limit = next_frame.start - current.start;
			if (limit < frames)
			{
				frames = std::max(1, limit);
			}
		}
		if (frames <= current_frames)
		{
			return true;
		}
	}

	// 終了位置を、現在の長さとの差分だけ動かす
	int section_num = edit->get_object_section_num(object);
	int end_value = edit->get_object_section_frame(object, section_num);
	if (end_value < 0)
	{
		end_value = current.end;
	}
	int new_end = end_value + (frames - current_frames);
	if (!edit->move_object_section(object, section_num, new_end))
	{
		log_text(L"MaterialLibrary: failed to change object length");
		return false;
	}
	return true;
}

static bool fit_length(EDIT_SECTION* edit, OBJECT_HANDLE object)
{
	Kind* kind = find_object_kind(edit, object);
	if (!kind)
	{
		return false;
	}
	const Material* material = resolve_object_material(edit, *kind, object);
	if (!material)
	{
		return false;
	}
	return adjust_length(edit, object, *kind, *material, true);
}

static void on_fit_button(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item)
{
	(void)effect;
	(void)item;
	fit_length(edit, object);
}

static void on_fit_menu(EDIT_SECTION* edit)
{
	fit_length(edit, edit->get_focus_object());
}

// 再生バー(カーソル)の位置に、未選択の汎用素材オブジェクトを生成する
// 選択中のレイヤーが埋まっている場合は、空いている下のレイヤーに作る
static void on_create_menu(EDIT_SECTION* edit)
{
	const int frame = edit->info->frame;
	const int start_layer = edit->info->layer;
	const double rate = (double)edit->info->rate;
	const double scale = (double)edit->info->scale;
	const double fps = (rate > 0.0 && scale > 0.0) ? rate / scale : 30.0;
	const int default_length = std::max(1, (int)std::lround(fps * 5.0));	// 既定は5秒

	for (int i = 0; i < 100; i++)
	{
		int layer = start_layer + i;
		int length = default_length;
		OBJECT_HANDLE next = edit->find_object(layer, frame);
		if (next)
		{
			OBJECT_LAYER_FRAME next_frame = edit->get_object_layer_frame(next);
			if (next_frame.start <= frame)
			{
				continue;	// 再生バー位置に既にオブジェクトがある
			}
			length = std::min(length, next_frame.start - frame);
		}
		OBJECT_HANDLE created = edit->create_object(PLUGIN_LABEL, layer, frame, length);
		if (created)
		{
			edit->set_cursor_layer_frame(layer, frame);
			edit->set_focus_object(created);
			return;
		}
	}
	log_text(L"MaterialLibrary: failed to create object at cursor");
}

//---------------------------------------------------------------------
//	オブジェクトの監視
//	- 素材を選んだ時に、長さの調整とオブジェクト名の設定をする
//	- 素材の長さを超えて引き延ばされたら、素材の長さに戻す
//	- 「登録するファイル」が変わったら、登録処理を始める
//	(イベント通知スレッドから直接は編集できないので、メインスレッドへ転送する)
//---------------------------------------------------------------------
static const wchar_t* const WATCHER_CLASS = L"MaterialLibraryWatcher";
static const UINT WM_MATERIAL_LIBRARY_CHECK = WM_APP + 41;
static const UINT WM_MATERIAL_LIBRARY_IMPORT = WM_APP + 42;
static HWND g_watcher_window = nullptr;
static std::atomic<bool> g_check_pending(false);

struct ObjectState
{
	std::wstring selection;		// 前回確認した時に選ばれていた素材のパス
	std::wstring auto_name;		// このプラグインが付けたオブジェクト名
	std::wstring import_file;	// 前回確認した時の「登録するファイル」
};
static std::map<int64_t, ObjectState> g_states;

static std::deque<std::wstring> g_import_queue;		// 登録待ちのファイル
static bool g_importing = false;
static bool g_clear_import_item = false;

static bool is_default_object_name(const std::wstring& name)
{
	if (name.empty() || name == PLUGIN_LABEL)
	{
		return true;
	}
	for (Kind* kind : g_kinds)
	{
		if (kind && name == kind->effect_name)
		{
			return true;
		}
	}
	return false;
}

// オブジェクト名を素材のファイル名にする (ユーザーが付けた名前は変更しない)
static void apply_object_name(EDIT_SECTION* edit, OBJECT_HANDLE object, const Material& material, ObjectState& state)
{
#if MATERIAL_LIBRARY_AUTO_NAME
	std::wstring desired = fs::path(material.name).stem().wstring();
	if (desired.empty())
	{
		return;
	}
	LPCWSTR current_text = edit->get_object_name(object);
	std::wstring current = current_text ? current_text : L"";
	if (current == desired)
	{
		state.auto_name = desired;
		return;
	}
	// 標準の名前、このプラグインが付けた名前、他の素材のファイル名のままなら、素材に合わせて付け直す
	// (オブジェクトを作り直しても判定できるように、前回の名前を覚えることには頼らない)
	if (is_default_object_name(current) || current == state.auto_name || g_material_stems.count(current))
	{
		edit->set_object_name(object, desired.c_str());
		state.auto_name = desired;
	}
#else
	(void)edit;
	(void)object;
	(void)material;
	(void)state;
#endif
}

//---------------------------------------------------------------------
//	選んだ素材の形式に合わせて、オブジェクトを別の種類のオブジェクトに置き換える
//	(エイリアスの effect.name だけを書き換えて作り直す。設定値や他のフィルタ効果は引き継がれます)
//---------------------------------------------------------------------
static std::set<int64_t> g_no_convert;		// 置き換えに失敗したオブジェクト (何度も試さない)
static bool g_alias_logged = false;

static std::string replace_effect_name(const std::string& alias, const std::wstring& from, const std::wstring& to)
{
	std::string from_utf8 = wide_to_utf8(from);
	std::string to_utf8 = wide_to_utf8(to);
	auto trim_ascii = [](std::string text)
		{
			size_t start = text.find_first_not_of(" \t\r");
			if (start == std::string::npos)
			{
				return std::string();
			}
			size_t end = text.find_last_not_of(" \t\r");
			return text.substr(start, end - start + 1);
		};

	std::string result;
	size_t pos = 0;
	while (pos <= alias.size())
	{
		size_t end = alias.find('\n', pos);
		bool last = (end == std::string::npos);
		std::string line = alias.substr(pos, last ? std::string::npos : end - pos);
		size_t equal = line.find('=');
		if (equal != std::string::npos && trim_ascii(line.substr(0, equal)) == "effect.name" && trim_ascii(line.substr(equal + 1)) == from_utf8)
		{
			bool cr = !line.empty() && line.back() == '\r';
			line = "effect.name=" + to_utf8 + (cr ? "\r" : "");
		}
		result += line;
		if (last)
		{
			break;
		}
		result += '\n';
		pos = end + 1;
	}
	return result;
}

static void check_object(EDIT_SECTION* edit, OBJECT_HANDLE object, bool is_focus);

// 置き換えたらtrue (新しいオブジェクトは確認まで済ませる)
static bool convert_object(EDIT_SECTION* edit, OBJECT_HANDLE object, const Kind& from, const Kind& to, bool is_focus)
{
	int64_t old_id = edit->get_object_id(object);
	if (g_no_convert.count(old_id))
	{
		return false;
	}
	LPCSTR alias_text = edit->get_object_alias(object);
	if (!alias_text)
	{
		g_no_convert.insert(old_id);
		return false;
	}
	std::string original = alias_text;
	std::string converted = replace_effect_name(original, from.effect_name, to.effect_name);
	if (converted == original)
	{
		// エイリアスの書式が想定と違う場合は、ログに出して置き換えない
		g_no_convert.insert(old_id);
		if (!g_alias_logged)
		{
			g_alias_logged = true;
			std::wstring head = utf8_to_wide(original.substr(0, 400).c_str());
			log_text((L"MaterialLibrary: effect.name not found in alias: " + head).c_str());
		}
		return false;
	}

	OBJECT_LAYER_FRAME frame = edit->get_object_layer_frame(object);
	int length = frame.end - frame.start + 1;
	ObjectState old_state = g_states[old_id];

	edit->delete_object(object);
	OBJECT_HANDLE created = edit->create_object_from_alias(converted.c_str(), frame.layer, frame.start, length);
	if (!created)
	{
		// 失敗したら元のオブジェクトに戻す
		created = edit->create_object_from_alias(original.c_str(), frame.layer, frame.start, length);
		log_text(L"MaterialLibrary: failed to convert object");
		if (created)
		{
			g_no_convert.insert(edit->get_object_id(created));
			if (is_focus)
			{
				edit->set_focus_object(created);
			}
		}
		return true;
	}

	// 新しいオブジェクトは、素材を選び直した時と同じ扱い(長さ合わせ・名前設定)にする
	int64_t new_id = edit->get_object_id(created);
	ObjectState& state = g_states[new_id];
	state = old_state;
	state.selection = L"__converted__";
	if (is_focus)
	{
		edit->set_focus_object(created);
	}
	check_object(edit, created, is_focus);
	return true;
}

static void check_object(EDIT_SECTION* edit, OBJECT_HANDLE object, bool is_focus)
{
	Kind* kind = find_object_kind(edit, object);
	if (!kind)
	{
		return;
	}
	const Material* material = resolve_object_material(edit, *kind, object);

	// 選んだ素材の形式と、オブジェクトの種類が違う場合は置き換える
	if (material)
	{
		Kind* target = kind_for_media(material->type);
		if (target && target != kind && convert_object(edit, object, *kind, *target, is_focus))
		{
			return;
		}
	}
	std::wstring key = material ? material->path : std::wstring();
	std::wstring import_file = is_focus ? read_item_text(edit, object, kind->effect_name, ITEM_IMPORT_FILE) : std::wstring();

	int64_t id = edit->get_object_id(object);
	auto it = g_states.find(id);
	bool first = (it == g_states.end());
	ObjectState& state = g_states[id];
	bool changed = !first && state.selection != key;
	state.selection = key;

	if (material)
	{
		apply_object_name(edit, object, *material, state);
		// 素材を選び直した時は長さを合わせる。それ以外は素材より長くなっている時だけ短くする
		adjust_length(edit, object, *kind, *material, changed);
	}

	if (is_focus)
	{
		if (!first && !import_file.empty() && import_file != state.import_file)
		{
			g_import_queue.push_back(import_file);
			g_clear_import_item = true;
		}
		state.import_file = import_file;
	}
}

static void check_objects(EDIT_SECTION* edit)
{
	OBJECT_HANDLE focus = edit->get_focus_object();
	std::vector<OBJECT_HANDLE> objects;
	if (focus)
	{
		objects.push_back(focus);
	}
	int selected = edit->get_selected_object_num();
	for (int i = 0; i < selected; i++)
	{
		OBJECT_HANDLE object = edit->get_selected_object(i);
		if (object && object != focus)
		{
			objects.push_back(object);
		}
	}
	for (OBJECT_HANDLE object : objects)
	{
		check_object(edit, object, object == focus);
	}
}

// 登録が終わった「登録するファイル」を空に戻す
static void clear_import_item(EDIT_SECTION* edit)
{
	OBJECT_HANDLE focus = edit->get_focus_object();
	Kind* kind = find_object_kind(edit, focus);
	if (!kind)
	{
		return;
	}
	if (!read_item_text(edit, focus, kind->effect_name, ITEM_IMPORT_FILE).empty())
	{
		edit->set_object_item_value(focus, kind->effect_name, ITEM_IMPORT_FILE, "");
	}
	g_states[edit->get_object_id(focus)].import_file.clear();
}

static void on_object_event(void* param)
{
	(void)param;
	if (g_watcher_window && !g_check_pending.exchange(true))
	{
		PostMessage(g_watcher_window, WM_MATERIAL_LIBRARY_CHECK, 0, 0);
	}
}

//---------------------------------------------------------------------
//	素材の登録ダイアログ
//---------------------------------------------------------------------
static const wchar_t* const IMPORT_CLASS = L"MaterialLibraryImportDialog";
static const int IDC_CATEGORY = 2001;
static const int IDC_NAME = 2002;

struct ImportDialogState
{
	std::vector<std::wstring> categories;
	std::wstring default_category;
	std::wstring category;
	std::wstring name;
	bool accepted = false;
	bool running = true;
	HWND combo = nullptr;
	HWND edit = nullptr;
};

static std::wstring read_window_text(HWND hwnd)
{
	int length = GetWindowTextLengthW(hwnd);
	std::wstring text((size_t)length + 1, L'\0');
	GetWindowTextW(hwnd, &text[0], length + 1);
	text.resize((size_t)length);
	return text;
}

static std::wstring trim(const std::wstring& text)
{
	size_t start = text.find_first_not_of(L" \t");
	if (start == std::wstring::npos)
	{
		return std::wstring();
	}
	size_t end = text.find_last_not_of(L" \t");
	return text.substr(start, end - start + 1);
}

static bool is_valid_file_name(const std::wstring& text)
{
	if (text.empty() || text == L"." || text == L"..")
	{
		return false;
	}
	return text.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
}

static LRESULT CALLBACK import_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	auto state = (ImportDialogState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	switch (message)
	{
	case WM_NCCREATE:
	{
		auto create = (CREATESTRUCT*)lparam;
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
		break;
	}
	case WM_COMMAND:
	{
		if (!state)
		{
			break;
		}
		if (LOWORD(wparam) == IDOK)
		{
			std::wstring category = trim(read_window_text(state->combo));
			std::wstring name = trim(read_window_text(state->edit));
			if (!is_valid_file_name(category))
			{
				MessageBoxW(hwnd, L"素材の種類には、フォルダ名として使える文字を入力してください。", L"素材の登録", MB_OK | MB_ICONWARNING);
				return 0;
			}
			if (!is_valid_file_name(name))
			{
				MessageBoxW(hwnd, L"素材名には、ファイル名として使える文字を入力してください。", L"素材の登録", MB_OK | MB_ICONWARNING);
				return 0;
			}
			state->category = category;
			state->name = name;
			state->accepted = true;
			state->running = false;
			return 0;
		}
		if (LOWORD(wparam) == IDCANCEL)
		{
			state->running = false;
			return 0;
		}
		break;
	}
	case WM_CLOSE:
	{
		if (state)
		{
			state->running = false;
		}
		return 0;
	}
	}
	return DefWindowProc(hwnd, message, wparam, lparam);
}

static HWND create_label(HWND parent, LPCWSTR text, int x, int y, int width, int height)
{
	HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, width, height, parent, nullptr, GetModuleHandle(nullptr), nullptr);
	SendMessage(hwnd, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
	return hwnd;
}

// 登録ダイアログを表示する (OKならtrue)
static bool show_import_dialog(HWND parent, const std::wstring& default_name, ImportDialogState& state)
{
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(wcex);
	wcex.lpszClassName = IMPORT_CLASS;
	wcex.lpfnWndProc = import_proc;
	wcex.hInstance = GetModuleHandle(nullptr);
	wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassExW(&wcex);

	const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
	const DWORD ex_style = WS_EX_DLGMODALFRAME;
	RECT window_rect = { 0, 0, 452, 172 };
	AdjustWindowRectEx(&window_rect, style, FALSE, ex_style);
	int width = window_rect.right - window_rect.left;
	int height = window_rect.bottom - window_rect.top;
	RECT parent_rect = {};
	if (parent)
	{
		GetWindowRect(parent, &parent_rect);
	}
	int x = parent ? (parent_rect.left + parent_rect.right - width) / 2 : CW_USEDEFAULT;
	int y = parent ? (parent_rect.top + parent_rect.bottom - height) / 2 : CW_USEDEFAULT;

	HWND hwnd = CreateWindowExW(
		ex_style,
		IMPORT_CLASS,
		L"素材の登録",
		style,
		x, y, width, height,
		parent, nullptr, GetModuleHandle(nullptr), &state);
	if (!hwnd)
	{
		return false;
	}

	HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	create_label(hwnd, L"素材の種類 (一覧から選ぶか、新しい名前を入力)", 16, 12, 420, 18);
	state.combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
		16, 32, 420, 200, hwnd, (HMENU)(INT_PTR)IDC_CATEGORY, GetModuleHandle(nullptr), nullptr);
	SendMessage(state.combo, WM_SETFONT, (WPARAM)font, TRUE);
	for (const auto& category : state.categories)
	{
		SendMessageW(state.combo, CB_ADDSTRING, 0, (LPARAM)category.c_str());
	}
	if (!state.categories.empty())
	{
		LRESULT found = CB_ERR;
		if (!state.default_category.empty())
		{
			found = SendMessageW(state.combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)state.default_category.c_str());
		}
		SendMessage(state.combo, CB_SETCURSEL, found != CB_ERR ? (WPARAM)found : 0, 0);
	}

	create_label(hwnd, L"素材名", 16, 68, 420, 18);
	state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", default_name.c_str(),
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
		16, 88, 420, 24, hwnd, (HMENU)(INT_PTR)IDC_NAME, GetModuleHandle(nullptr), nullptr);
	SendMessage(state.edit, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessage(state.edit, EM_SETSEL, 0, -1);

	HWND ok = CreateWindowExW(0, L"BUTTON", L"登録", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
		252, 126, 88, 28, hwnd, (HMENU)(INT_PTR)IDOK, GetModuleHandle(nullptr), nullptr);
	SendMessage(ok, WM_SETFONT, (WPARAM)font, TRUE);
	HWND cancel = CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		348, 126, 88, 28, hwnd, (HMENU)(INT_PTR)IDCANCEL, GetModuleHandle(nullptr), nullptr);
	SendMessage(cancel, WM_SETFONT, (WPARAM)font, TRUE);

	if (parent)
	{
		EnableWindow(parent, FALSE);
	}
	ShowWindow(hwnd, SW_SHOW);
	SetFocus(state.edit);

	MSG msg;
	while (state.running && GetMessage(&msg, nullptr, 0, 0) > 0)
	{
		if (!IsDialogMessage(hwnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	if (parent)
	{
		EnableWindow(parent, TRUE);
	}
	DestroyWindow(hwnd);
	if (parent)
	{
		SetForegroundWindow(parent);
	}
	return state.accepted;
}

//---------------------------------------------------------------------
//	素材のライブラリへの登録
//---------------------------------------------------------------------
static std::wstring expand_path_variable(const std::wstring& path)
{
	const std::wstring variable = L"%AppData%";
	if (path.compare(0, variable.size(), variable) == 0 && g_config && g_config->app_data_path)
	{
		return std::wstring(g_config->app_data_path) + path.substr(variable.size());
	}
	return path;
}

static DWORD WINAPI restart_thread(LPVOID)
{
	Sleep(300);
	if (g_edit)
	{
		g_edit->restart_host_app();
	}
	return 0;
}

// 指定ファイルをライブラリへ登録する (登録したらtrue)
static bool import_material_file(const std::wstring& raw_source)
{
	HWND parent = g_edit ? g_edit->get_host_app_window() : nullptr;

	std::wstring source = expand_path_variable(raw_source);
	if (source.empty())
	{
		MessageBoxW(parent, L"「登録するファイル」にファイルをドロップするか、選択してから押してください。", L"素材の登録", MB_OK | MB_ICONINFORMATION);
		return false;
	}
	std::error_code ec;
	if (!fs::is_regular_file(source, ec))
	{
		MessageBoxW(parent, L"指定されたファイルが見つかりません。", L"素材の登録", MB_OK | MB_ICONWARNING);
		return false;
	}
	std::wstring ext = to_lower(fs::path(source).extension().wstring());
	int type = classify_extension(ext);
	if (type == 0)
	{
		MessageBoxW(parent,
			L"対応していないファイル形式です。\n"
			L"(音声: wav, mp3, ogg, flac, m4a, aac, wma, opus / 画像: png, jpg, bmp, gif, webp, tga / 動画: mp4, mov, avi, mkv, webm, wmv, flv, m4v, mpg, mts)",
			L"素材の登録", MB_OK | MB_ICONWARNING);
		return false;
	}

	ImportDialogState state;
	for (const auto& category : g_library)
	{
		state.categories.push_back(category.name);
		// 同じ形式の素材がある種類を、最初の選択にする
		if (state.default_category.empty())
		{
			for (const auto& material : category.materials)
			{
				if (material.type == type)
				{
					state.default_category = category.name;
					break;
				}
			}
		}
	}
	if (!show_import_dialog(parent, fs::path(source).stem().wstring(), state))
	{
		return false;
	}

	fs::path directory = fs::path(g_library_root) / state.category;
	fs::create_directories(directory, ec);
	fs::path destination = directory / (state.name + fs::path(source).extension().wstring());
	for (int i = 2; fs::exists(destination, ec); i++)
	{
		destination = directory / (state.name + L" (" + std::to_wstring(i) + L")" + fs::path(source).extension().wstring());
	}
	if (!CopyFileW(source.c_str(), destination.c_str(), TRUE))
	{
		MessageBoxW(parent, L"ファイルのコピーに失敗しました。", L"素材の登録", MB_OK | MB_ICONERROR);
		return false;
	}

	int answer = MessageBoxW(parent,
		L"素材を登録しました。\n素材一覧への反映には、AviUtl2の再起動が必要です。\n今すぐ再起動しますか?",
		L"素材の登録", MB_YESNO | MB_ICONQUESTION);
	if (answer == IDYES)
	{
		HANDLE thread = CreateThread(nullptr, 0, restart_thread, nullptr, 0, nullptr);
		if (thread)
		{
			CloseHandle(thread);
		}
	}
	return true;
}

static void on_register_button(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item)
{
	(void)item;
	if (g_importing)
	{
		return;
	}
	std::wstring source = read_item_text(edit, object, effect, ITEM_IMPORT_FILE);
	g_importing = true;
	bool registered = import_material_file(source);
	g_importing = false;
	if (registered)
	{
		// 登録済みのファイルが残らないように空に戻す
		edit->set_object_item_value(object, effect, ITEM_IMPORT_FILE, "");
		g_states[edit->get_object_id(object)].import_file.clear();
	}
}

// 登録待ちのファイルを順番に処理する
static void run_import_queue()
{
	if (g_importing)
	{
		return;
	}
	g_importing = true;
	while (!g_import_queue.empty())
	{
		std::wstring file = g_import_queue.front();
		g_import_queue.pop_front();
		import_material_file(file);
	}
	g_importing = false;

	if (g_clear_import_item)
	{
		g_clear_import_item = false;
		if (g_edit)
		{
			g_edit->call_edit_section(clear_import_item);
		}
	}
	// 登録中に受け取った変更通知を取りこぼさないように、もう一度確認する
	on_object_event(nullptr);
}

static LRESULT CALLBACK watcher_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_MATERIAL_LIBRARY_CHECK)
	{
		g_check_pending = false;
		if (g_edit && !g_importing)
		{
			g_edit->call_edit_section(check_objects);
			if (!g_import_queue.empty())
			{
				run_import_queue();
			}
		}
		return 0;
	}
	if (message == WM_MATERIAL_LIBRARY_IMPORT)
	{
		std::unique_ptr<std::wstring> file((std::wstring*)lparam);
		if (file && !file->empty())
		{
			g_import_queue.push_back(*file);
			run_import_queue();
		}
		return 0;
	}
	return DefWindowProc(hwnd, message, wparam, lparam);
}

// プレビュー画面などへファイルをD&Dした時に呼ばれる
static void on_file_drop(void* param, LPCWSTR file)
{
	(void)param;
	if (!file || !*file || !g_watcher_window)
	{
		return;
	}
	// ダイアログはメインスレッドで出したいので、監視ウィンドウへ転送する
	auto path = new std::wstring(file);
	if (!PostMessage(g_watcher_window, WM_MATERIAL_LIBRARY_IMPORT, 0, (LPARAM)path))
	{
		delete path;
	}
}

//---------------------------------------------------------------------
//	汎用プラグイン構造体定義
//---------------------------------------------------------------------
static COMMON_PLUGIN_TABLE g_common_plugin_table = {
	L"MaterialLibrary",
	PLUGIN_INFO
};

//---------------------------------------------------------------------
//	エクスポート関数
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle)
{
	g_logger = handle;
}

EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle)
{
	g_config = handle;
}

EXTERN_C __declspec(dllexport) void InitializeCache(CACHE_HANDLE* handle)
{
	g_cache = handle;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version)
{
	(void)version;
	return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin()
{
	if (g_watcher_window)
	{
		DestroyWindow(g_watcher_window);
		g_watcher_window = nullptr;
	}
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void)
{
	return &g_common_plugin_table;
}

static void register_kind(HOST_APP_TABLE* host, int index, const wchar_t* name, int media_type, int flag,
	bool (*func_video)(FILTER_PROC_VIDEO*), bool (*func_audio)(FILTER_PROC_AUDIO*))
{
	Kind* kind = new Kind(index, name, media_type, flag | FILTER_PLUGIN_TABLE::FLAG_INPUT);
	g_kinds[index] = kind;
	kind->build();
	kind->table = {
		kind->flag,
		kind->effect_name,
		PLUGIN_LABEL,
		PLUGIN_INFO,
		kind->items.data(),
		func_video,
		func_audio,
		nullptr,		// func_create (FLAG_USERDATA未使用)
		nullptr			// func_destroy
	};
	host->register_filter_plugin(&kind->table);
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host)
{
	g_host = host;
	g_edit = host->create_edit_handle();

	// 素材ライブラリを読み込んで、設定項目を作る
	// タイムラインでの色は FLAG_VIDEO / FLAG_AUDIO の組み合わせで決まるので、形式ごとにオブジェクトを用意して
	// 素材を選んだ時に自動で切り替える (追加メニューに出るのは入口の「汎用素材」だけ)
	//   音声のみ = 音声の色 / 画像のみ = 画像の色 / 画像+音声 = 動画の色
	scan_library();
	const int hide = FILTER_PLUGIN_TABLE::FLAG_HIDEMENU;
	register_kind(host, 0, L"汎用素材(音声)", MEDIA_AUDIO,
		FILTER_PLUGIN_TABLE::FLAG_AUDIO | hide,
		nullptr, proc_audio_k<0>);
	register_kind(host, 1, L"汎用素材(画像)", MEDIA_IMAGE,
		FILTER_PLUGIN_TABLE::FLAG_VIDEO | hide,
		proc_video_k<1>, nullptr);
	register_kind(host, 2, L"汎用素材(動画)", MEDIA_VIDEO,
		FILTER_PLUGIN_TABLE::FLAG_VIDEO | FILTER_PLUGIN_TABLE::FLAG_AUDIO | hide,
		proc_video_k<2>, proc_audio_k<2>);
	// 入口。旧バージョンで作ったオブジェクトもこの名前なので、そのまま開ける
	register_kind(host, 3, L"汎用素材", 0,
		FILTER_PLUGIN_TABLE::FLAG_VIDEO | FILTER_PLUGIN_TABLE::FLAG_AUDIO,
		proc_video_k<3>, proc_audio_k<3>);

	// 編集メニュー (ショートカットキーを割り当てられます)
	host->register_edit_menu(L"汎用素材を再生バーの位置に生成", on_create_menu);
	host->register_edit_menu(L"汎用素材の長さを素材に合わせる", on_fit_menu);

	// オブジェクトの監視用のウィンドウ (メッセージ受信専用)
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(wcex);
	wcex.lpszClassName = WATCHER_CLASS;
	wcex.lpfnWndProc = watcher_proc;
	wcex.hInstance = GetModuleHandle(nullptr);
	if (RegisterClassExW(&wcex) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
	{
		g_watcher_window = CreateWindowExW(0, WATCHER_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), nullptr);
	}
	host->register_event_listener(EVENT_TYPE::UPDATE_OBJECT, nullptr, on_object_event);
	host->register_event_listener(EVENT_TYPE::CHANGE_FOCUS_OBJECT, nullptr, on_object_event);

	// ファイルをD&Dした時の取り込み先として登録する
	host->register_file_drop_param_handler(L"素材ライブラリに登録", IMPORT_FILE_FILTER, nullptr, on_file_drop);
}