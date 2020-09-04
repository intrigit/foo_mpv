#include "stdafx.h"
#include <sstream>

#include <helpers/BumpableElem.h>

#include "libmpv.h"

// TODO doesn't start video when panel is first added into new tab (or maybe otherwise)

namespace {
	static const GUID guid_mpv_panel =
	{ 0x777a523a, 0x1ed, 0x48b9, { 0xb9, 0x1, 0xda, 0xb1, 0xbe, 0x31, 0x7c, 0xa4 } };

	struct CMpvWindow : public ui_element_instance, CWindowImpl<CMpvWindow>, play_callback_impl_base, mpv_player {
	public:
		DECLARE_WND_CLASS_EX(L"mpv_dui", CS_HREDRAW | CS_VREDRAW, 0)

		BEGIN_MSG_MAP(CMpvWindow)
			MSG_WM_ERASEBKGND(erase_bg)
			MSG_WM_DESTROY(kill_mpv)
		END_MSG_MAP()

		CMpvWindow(ui_element_config::ptr config, ui_element_instance_callback_ptr p_callback)
			: m_callback(p_callback), m_config(config)
		{
		}

		BOOL erase_bg(CDCHandle dc) {
			CRect rc; WIN32_OP_D(GetClientRect(&rc));
			CBrush brush;
			WIN32_OP_D(brush.CreateSolidBrush(0x00000000) != NULL);
			WIN32_OP_D(dc.FillRect(&rc, brush));
			return TRUE;
		}

		void initialize_window(HWND parent)
		{
			set_mpv_wid(Create(parent, 0, 0, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0));

			start();
		}

		void on_playback_starting(play_control::t_track_command p_command, bool p_paused) {
		}
		void on_playback_new_track(metadb_handle_ptr p_track) {
			play_path(p_track->get_path());
		}
		void on_playback_stop(play_control::t_stop_reason p_reason) {
			stop();
		}
		void on_playback_seek(double p_time) {
			seek(p_time);
		}
		void on_playback_pause(bool p_state) {
			pause(p_state);
		}
		void on_playback_edited(metadb_handle_ptr p_track) {
		}
		void on_playback_time(double p_time) {
			sync();
		}

		HWND get_wnd() { return m_hWnd; }

		void set_configuration(ui_element_config::ptr config) { m_config = config; }
		ui_element_config::ptr get_configuration() { return m_config; }
		static GUID g_get_guid() {
			return guid_mpv_panel;
		}
		static GUID g_get_subclass() { return ui_element_subclass_utility; }
		static void g_get_name(pfc::string_base& out) { out = "Mpv"; }
		static ui_element_config::ptr g_get_default_configuration() { return ui_element_config::g_create_empty(g_get_guid()); }
		static const char* g_get_description() { return "Mpv"; }
		void notify(const GUID& p_what, t_size p_param1, const void* p_param2, t_size p_param2size)
		{
			if (p_what == ui_element_notify_visibility_changed)
			{
				if (cfg_mpv_stop_hidden)
				{
					if (p_param1 == 1)
					{
						start();
					}
					else
					{
						stop();
					}
				}
				else
				{
					if (p_param1 == 1)
					{
						start();
					}
				}
			}
		};

	private:
		ui_element_config::ptr m_config;

	protected:
		const ui_element_instance_callback_ptr m_callback;
	};
	class ui_element_mpvimpl : public ui_element_impl<CMpvWindow> {};
	static service_factory_single_t<ui_element_mpvimpl> g_ui_element_mpvimpl_factory;
}

