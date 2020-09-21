#include "stdafx.h"
// PCH ^

#include <../helpers/WindowPositionUtils.h>
#include <../helpers/atl-misc.h>

#include <sstream>

#include "libmpv.h"
#include "resource.h"
#include "thumbnailer.h"

namespace mpv {
extern cfg_uint cfg_thumb_seek;

static const GUID guid_thumb_pos_index = {
    0xbf7490f5,
    0xd59d,
    0x49d0,
    {0xb6, 0x9a, 0x8f, 0x81, 0x80, 0x9c, 0xef, 0x48}};

static const char str_thumb_pos_pin[] = "%path% | %subsong%";

static metadb_index_manager::ptr g_cachedAPI;
static metadb_index_manager::ptr theAPI() {
  auto ret = g_cachedAPI;
  if (ret.is_empty()) ret = metadb_index_manager::get();
  return ret;
}

class initquit_impl : public initquit {
 public:
  void on_quit() { g_cachedAPI.release(); }
};

class metadb_index_client_impl : public metadb_index_client {
 public:
  metadb_index_client_impl(const char* pinTo) {
    static_api_ptr_t<titleformat_compiler>()->compile_force(m_keyObj, pinTo);
  }

  metadb_index_hash transform(const file_info& info,
                              const playable_location& location) {
    pfc::string_formatter str;
    m_keyObj->run_simple(location, &info, str);
    return static_api_ptr_t<hasher_md5>()
        ->process_single_string(str)
        .xorHalve();
  }

  titleformat_object::ptr m_keyObj;
};

static metadb_index_client_impl* client_get() {
  static metadb_index_client_impl* g_thumb_client =
      new service_impl_single_t<metadb_index_client_impl>(str_thumb_pos_pin);

  return g_thumb_client;
}

class init_stage_callback_impl : public init_stage_callback {
 public:
  void on_init_stage(t_uint32 stage) {
    if (stage == init_stages::before_config_read) {
      auto api = metadb_index_manager::get();
      g_cachedAPI = api;
      try {
        api->add(client_get(), guid_thumb_pos_index,
                 4 * system_time_periods::week);
      } catch (std::exception const& e) {
        api->remove(guid_thumb_pos_index);
        FB2K_console_formatter() << "mpv: Thumb time store "
                                    "initialization failure: "
                                 << e;
        return;
      }
      api->dispatch_global_refresh();
    }
  }
};

static service_factory_single_t<init_stage_callback_impl>
    g_init_stage_callback_impl;
static service_factory_single_t<initquit_impl> g_initquit_impl;

bool thumb_time_store_get(metadb_handle_ptr metadb, double& out) {
  metadb_index_hash hash;
  if (!client_get()->hashHandle(metadb, hash)) return false;

  mem_block_container_impl temp;
  theAPI()->get_user_data(guid_thumb_pos_index, hash, temp);
  if (temp.get_size() > 0) {
    try {
      stream_reader_formatter_simple_ref<false> reader(temp.get_ptr(),
                                                       temp.get_size());
      reader >> out;
      return true;
    } catch (exception_io_data) {
    }
  }
  return false;
}

void thumb_time_store_set(metadb_handle_ptr metadb, const double pos) {
  metadb_index_hash hash;
  if (!client_get()->hashHandle(metadb, hash)) return;
  stream_writer_formatter_simple<false> writer;
  writer << pos;
  theAPI()->set_user_data(guid_thumb_pos_index, hash, writer.m_buffer.get_ptr(),
                          writer.m_buffer.get_size());

  remove_from_cache(metadb);
  theAPI()->dispatch_refresh(guid_thumb_pos_index, hash);
  metadb_io::get()->dispatch_refresh(metadb);
}

void thumb_time_store_purge(metadb_handle_ptr metadb) {
  metadb_index_hash hash;
  if (!client_get()->hashHandle(metadb, hash)) return;
  stream_writer_formatter_simple<false> writer;
  theAPI()->set_user_data(guid_thumb_pos_index, hash, writer.m_buffer.get_ptr(),
                          0);

  remove_from_cache(metadb);
  theAPI()->dispatch_refresh(guid_thumb_pos_index, hash);
  metadb_io::get()->dispatch_refresh(metadb);
}

static const GUID guid_cfg_thumb_chooser_popup_rect = {
    0x23c06119,
    0x5298,
    0x4dad,
    {0x9f, 0x41, 0x57, 0xd2, 0x75, 0xd2, 0x44, 0xc4}};
static cfg_struct_t<RECT> cfg_thumb_chooser_popup_rect(
    guid_cfg_thumb_chooser_popup_rect, 0);

struct CThumbnailChooserWindow : public CDialogImpl<CThumbnailChooserWindow> {
 public:
  enum { IDD = IDD_THUMBCHOOSER };

  metadb_handle_ptr metadb;

  CThumbnailChooserWindow(metadb_handle_ptr p_metadb) : metadb(p_metadb) {}

  BEGIN_MSG_MAP(CThumbnailChooserWindow)
  MSG_WM_INITDIALOG(OnInitDialog)
  MSG_WM_HSCROLL(OnScroll);
  COMMAND_HANDLER_EX(IDCANCEL, BN_CLICKED, OnCancel)
  COMMAND_HANDLER_EX(IDOK, BN_CLICKED, OnAccept)
  MSG_WM_DESTROY(on_destroy)
  END_MSG_MAP()

  static DWORD GetWndStyle(DWORD style) { return WS_POPUP | WS_VISIBLE; }

  BOOL on_erase_bg(CDCHandle dc) {
    CRect rc;
    WIN32_OP_D(GetClientRect(&rc));
    CBrush brush;
    WIN32_OP_D(brush.CreateSolidBrush(0x000000) != NULL);
    WIN32_OP_D(dc.FillRect(&rc, brush));

    return TRUE;
  }

  void OnScroll(UINT, int, CWindow) {
    seek(metadb->get_length() *
         ((CTrackBarCtrl)uGetDlgItem(IDC_SLIDER1)).GetPos() / 1000.0);
    return;
  }

  void OnCancel(UINT, int, CWindow) { DestroyWindow(); }
  void OnAccept(UINT, int, CWindow) {
    double mpv_time = -1.0;
    if (libmpv()->get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &mpv_time) >
        -1) {
      thumb_time_store_set(metadb, mpv_time - time_base);
    }

    DestroyWindow();
  }

  void update_title() {
    pfc::string8 title;
    // mpv::get_popup_title(title);
    uSetWindowText(m_hWnd, title);
  }

  mpv_handle* mpv = NULL;

  BOOL OnInitDialog(CWindow wnd, LPARAM lp) {
    SetClassLong(get_wnd(), GCL_HICON,
                 (LONG)LoadIcon(core_api::get_my_instance(),
                                MAKEINTRESOURCE(IDI_ICON1)));

    update_title();

    CTrackBarCtrl slider_seek = (CTrackBarCtrl)uGetDlgItem(IDC_SLIDER1);
    slider_seek.SetRangeMin(1);
    slider_seek.SetRangeMax(1000);

    ::ShowWindowCentered(*this, GetParent());

    double pos = 0.0;
    thumb_time_store_get(metadb, pos);
    if (pos > metadb->get_length()) pos = 0.0;
    slider_seek.SetPos(
        (int)min(1000, max(0, (pos / metadb->get_length()) * 1000)));

    pfc::string8 filename;
    filename.add_filename(metadb->get_path());
    if (filename.has_prefix("\\file://")) {
      filename.remove_chars(0, 8);

      double time_base_l = 0.0;
      if (metadb->get_subsong_index() > 1) {
        for (t_uint32 s = 0; s < metadb->get_subsong_index(); s++) {
          playable_location_impl tmp = metadb->get_location();
          tmp.set_subsong(s);
          metadb_handle_ptr subsong = metadb::get()->handle_create(tmp);
          if (subsong.is_valid()) {
            time_base_l += subsong->get_length();
          }
        }
      }
      time_base = time_base_l;
    }

    // create mpv
    if (!libmpv()->load_dll()) return false;

    mpv = libmpv()->create();

    int64_t l_wid = (intptr_t)(uGetDlgItem(IDC_STATIC_pic).m_hWnd);
    libmpv()->set_option(mpv, "wid", MPV_FORMAT_INT64, &l_wid);

    libmpv()->set_option_string(mpv, "load-scripts", "no");
    libmpv()->set_option_string(mpv, "ytdl", "no");
    libmpv()->set_option_string(mpv, "load-stats-overlay", "no");
    libmpv()->set_option_string(mpv, "load-osd-console", "no");

    std::stringstream time_sstring;
    time_sstring.setf(std::ios::fixed);
    time_sstring.precision(3);
    time_sstring << time_base + pos;
    std::string time_string = time_sstring.str();
    libmpv()->set_option_string(mpv, "start", time_string.c_str());
    libmpv()->set_option_string(mpv, "audio-display", "no");
    libmpv()->set_option_string(mpv, "pause", "yes");
    libmpv()->set_option_string(mpv, "hr-seek-framedrop", "no");
    libmpv()->set_option_string(mpv, "hr-seek-demuxer-offset", "-2");
    libmpv()->set_option_string(mpv, "audio", "no");
    libmpv()->set_option_string(mpv, "force-window", "yes");
    libmpv()->set_option_string(mpv, "idle", "yes");
    libmpv()->set_option_string(mpv, "keep-open", "yes");

    if (libmpv()->initialize(mpv) != 0) {
      libmpv()->destroy(mpv);
      mpv = NULL;
    } else {
      load_video(filename);
    }

    return true;
  }

  void seek(double time) {
    // build command
    std::stringstream time_sstring;
    time_sstring.setf(std::ios::fixed);
    time_sstring.precision(15);
    time_sstring << time_base + time;
    std::string time_string = time_sstring.str();
    const char* cmd[] = {"seek", time_string.c_str(), "absolute+exact", NULL};

    if (libmpv()->command(mpv, cmd) < 0) {
      // error
    }
  }

  void on_destroy() {
    if (mpv != NULL) {
      mpv_handle* temp = mpv;
      mpv = NULL;
      libmpv()->terminate_destroy(temp);
    }
    RECT client_rect = {};
    GetWindowRect(&client_rect);
    cfg_thumb_chooser_popup_rect = client_rect;
  }

  HWND get_wnd() { return m_hWnd; }

 private:
  double time_base;
  void load_video(pfc::string8 filename) {
    const char* cmd[] = {"loadfile", filename.c_str(), NULL};
    if (libmpv()->command(mpv, cmd) < 0) {
      // error
    }

    // wait for load
    const int64_t userdata = 853727396;
    if (libmpv()->observe_property(mpv, userdata, "seeking", MPV_FORMAT_FLAG) <
        0) {
      // error
    } else {
      for (int i = 0; i < 100; i++) {
        mpv_event* event = libmpv()->wait_event(mpv, 0.05);

        int ret = 0;
        libmpv()->get_property(mpv, "idle-active", MPV_FORMAT_FLAG, &ret);
        if (ret == 1) {
          libmpv()->unobserve_property(mpv, userdata);
          break;
        }
        libmpv()->get_property(mpv, "seeking", MPV_FORMAT_FLAG, &ret);
        if (ret == 0) {
          libmpv()->unobserve_property(mpv, userdata);
          break;
        }
      }
    }
    libmpv()->unobserve_property(mpv, userdata);
  }

 protected:
};

static void RunThumbnailChooserWindow(metadb_handle_ptr metadb) {
  try {
    new CWindowAutoLifetime<
        ImplementModelessTracking<CThumbnailChooserWindow> >(
        core_api::get_main_window(), metadb);
  } catch (std::exception const& e) {
    popup_message::g_complain("Popup creation failure", e);
  }
}

static const GUID guid_context_menu_group = {
    0x7f393709, 0x17f6, 0x4df8, {0x98, 0xc7, 0x4f, 0x4, 0x76, 0x1a, 0x20, 0xc}};

static contextmenu_group_popup_factory g_thums(guid_context_menu_group,
                                               contextmenu_groups::utilities,
                                               "Thumbnail");

class thumbnail_items : public contextmenu_item_simple {
 public:
  enum { cmd_choose = 0, cmd_remove_times, cmd_purgecache, cmd_total };
  GUID get_parent() { return contextmenu_groups::utilities; }
  unsigned get_num_items() { return cmd_total; }
  void get_item_name(unsigned item, pfc::string_base& p_out) {
    switch (item) {
      case cmd_choose:
        p_out = "Choose thumbnail";
        break;
      case cmd_remove_times:
        p_out = "Remove custom thumbnail times for items";
        break;
      case cmd_purgecache:
        p_out = "Purge cached thumbnails for items";
        break;
      default:
        uBugCheck();
    }
  }

  void context_command(unsigned p_index, metadb_handle_list_cref p_data,
                       const GUID& p_caller) {
    switch (p_index) {
      case cmd_choose:
        RunThumbnailChooserWindow(get_thumbnail_item_from_items(p_data));
        break;
      case cmd_remove_times:
        for (unsigned i = 0; i < p_data.get_size(); i++) {
          thumb_time_store_purge(p_data[i]);
        }
        break;
      case cmd_purgecache:
        for (unsigned i = 0; i < p_data.get_size(); i++) {
          remove_from_cache(p_data[i]);
          metadb_io::get()->dispatch_refresh(p_data[i]);
        }
        break;
      default:
        uBugCheck();
    }
  }

  bool get_item_description(unsigned item, pfc::string_base& p_out) {
    switch (item) {
      case cmd_choose:
        p_out = "Choose a thumbnail to use from the video";
        return true;
      case cmd_remove_times:
        p_out = "Remove saved custom thumbnail times for selected items";
        return true;
      case cmd_purgecache:
        p_out =
            "Remove cached thumbnails for selected items so that thumbnails "
            "will be regenerated";
        return true;
      default:
        uBugCheck();
    }
  }

  GUID get_item_guid(unsigned item) {
    static const GUID guid_choose = {
        0xafad2807,
        0x1c17,
        0x4a58,
        {0xa4, 0x56, 0xd7, 0x81, 0x80, 0x7b, 0xfc, 0xef}};
    static const GUID guid_purge = {
        0x66313798,
        0xd866,
        0x4d12,
        {0x92, 0x74, 0xcd, 0xb6, 0x25, 0xb5, 0xb1, 0xfc}};
    static const GUID guid_purgecache = {
        0xe57dafd2,
        0xb50e,
        0x4fad,
        {0x85, 0xde, 0x79, 0x3e, 0xed, 0x9b, 0x5d, 0x60}};

    switch (item) {
      case cmd_choose:
        return guid_choose;
      case cmd_remove_times:
        return guid_purge;
      case cmd_purgecache:
        return guid_purgecache;
      default:
        uBugCheck();
    }
  }
};

static contextmenu_item_factory_t<thumbnail_items> g_thumbnail_menu;
}  // namespace mpv
