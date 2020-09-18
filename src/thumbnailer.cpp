#pragma once
#include "stdafx.h"
// PCH ^

#include <sstream>

#include "../SDK/foobar2000.h"
#include "thumbnailer.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/frame.h"
#include "libswscale/swscale.h"
}

#include "SQLiteCpp/SQLiteCpp.h"
#include "include/sqlite3.h"

namespace mpv {

extern cfg_uint cfg_thumb_size, cfg_thumb_scaling, cfg_thumb_avoid_dark,
    cfg_thumb_seektype, cfg_thumb_seek, cfg_thumb_cache_quality,
    cfg_thumb_cache_format;
extern cfg_bool cfg_thumb_cache, cfg_thumbs, cfg_thumb_group_longest;
extern advconfig_checkbox_factory cfg_logging;

static std::unique_ptr<SQLite::Database> db_ptr;
static std::unique_ptr<SQLite::Statement> query_get;
static std::unique_ptr<SQLite::Statement> query_put;

class db_loader : public initquit {
 public:
  void on_init() override {
    pfc::string8 db_path = core_api::get_profile_path();
    db_path.add_filename("thumbcache.db");
    db_path.remove_chars(0, 7);

    const char* create_str =
        "CREATE TABLE IF NOT EXISTS thumbs(location TEXT NOT "
        "NULL, subsong INT NOT NULL, created INT, thumb BLOB, PRIMARY "
        "KEY(location, subsong))";
    const char* get_str =
        "SELECT thumb FROM thumbs WHERE location = ? AND subsong = ?";
    const char* put_str =
        "INSERT INTO thumbs VALUES (?, ?, CURRENT_TIMESTAMP, ?)";

    try {
      db_ptr.reset(new SQLite::Database(
          db_path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE));

      try {
        db_ptr->exec(create_str);
        query_get.reset(new SQLite::Statement(*db_ptr, get_str));
        query_put.reset(new SQLite::Statement(*db_ptr, put_str));
      } catch (SQLite::Exception e) {
        try {
          console::error(
              "mpv: Error reading thumbnail table, attempting to recreate "
              "database");
          db_ptr->exec("DROP TABLE thumbs");
          db_ptr->exec(create_str);
          query_get.reset(new SQLite::Statement(*db_ptr, get_str));
          query_put.reset(new SQLite::Statement(*db_ptr, put_str));
        } catch (SQLite::Exception e) {
          std::stringstream msg;
          msg << "mpv: Error accessing thumbnail table: " << e.what();
          console::error(msg.str().c_str());
        }
      }

      std::stringstream msg;
      msg << "mpv: SQLite mode " << sqlite3_threadsafe();
      console::info(msg.str().c_str());
    } catch (SQLite::Exception e) {
      std::stringstream msg;
      msg << "mpv: Error accessing thumbnail cache: " << e.what();
      console::error(msg.str().c_str());
    }
  }
};

static initquit_factory_t<db_loader> g_db_loader;

void clear_thumbnail_cache() {
  if (db_ptr) {
    try {
      int deletes = db_ptr->exec("DELETE FROM thumbs");
      std::stringstream msg;
      msg << "mpv: Deleted " << deletes << " thumbnails from database";
      console::info(msg.str().c_str());
    } catch (SQLite::Exception e) {
      std::stringstream msg;
      msg << "mpv: Error clearing thumbnail cache: " << e.what();
      console::error(msg.str().c_str());
    }

  } else {
    console::error("mpv: Thumbnail cache not loaded");
  }

  compact_thumbnail_cache();
}

void sqlitefunction_missing(sqlite3_context* context, int num,
                            sqlite3_value** val) {
  const char* location = (const char*)sqlite3_value_text(val[0]);
  try {
    if (filesystem::g_exists(location, fb2k::noAbort)) {
      sqlite3_result_int(context, 1);
      return;
    }
  } catch (exception_io e) {
  }
  sqlite3_result_int(context, 0);
}

void clean_thumbnail_cache() {
  if (db_ptr) {
    try {
      db_ptr->createFunction("missing", 1, true, nullptr,
                             sqlitefunction_missing);
      int deletes =
          db_ptr->exec("DELETE FROM thumbs WHERE missing(location) IS 0");
      std::stringstream msg;
      msg << "mpv: Deleted " << deletes << " dead thumbnails from database";
      console::info(msg.str().c_str());
    } catch (SQLite::Exception e) {
      std::stringstream msg;
      msg << "mpv: Error clearing thumbnail cache: " << e.what();
      console::error(msg.str().c_str());
    }
  } else {
    console::error("mpv: Thumbnail cache not loaded");
  }

  compact_thumbnail_cache();
}

void regenerate_thumbnail_cache() {
  if (db_ptr) {
    try {
    } catch (SQLite::Exception e) {
      std::stringstream msg;
      msg << "mpv: Error clearing thumbnail cache: " << e.what();
      console::error(msg.str().c_str());
    }
  } else {
    console::error("mpv: Thumbnail cache not loaded");
  }
}

void compact_thumbnail_cache() {
  if (db_ptr) {
    try {
      db_ptr->exec("VACUUM");
      console::info("mpv: Thumbnail database compacted");
    } catch (SQLite::Exception e) {
      std::stringstream msg;
      msg << "mpv: Error clearing thumbnail cache: " << e.what();
      console::error(msg.str().c_str());
    }
  } else {
    console::error("mpv: Thumbnail cache not loaded");
  }
}

static void libavtry(int error, const char* cmd) {
  if (error < 0) {
    char* error_str = new char[500];
    av_strerror(error, error_str, 500);
    std::stringstream msg;
    msg << "mpv: libav error for " << cmd << ": " << error_str;
    console::error(msg.str().c_str());
    delete[] error_str;

    throw exception_album_art_unsupported_entry();
  }
}

thumbnailer::thumbnailer(metadb_handle_ptr p_metadb)
    : metadb(p_metadb),
      time_start_in_file(0.0),
      time_end_in_file(metadb->get_length()) {
  // get filename
  pfc::string8 filename;
  filename.add_filename(metadb->get_path());
  if (filename.has_prefix("\\file://")) {
    filename.remove_chars(0, 8);

    if (filename.is_empty()) {
      throw exception_album_art_not_found();
    }
  } else {
    throw exception_album_art_not_found();
  }

  // get start/end time of track
  time_start_in_file = 0.0;
  if (metadb->get_subsong_index() > 1) {
    for (t_uint32 s = 0; s < metadb->get_subsong_index(); s++) {
      playable_location_impl tmp = metadb->get_location();
      tmp.set_subsong(s);
      metadb_handle_ptr subsong = metadb::get()->handle_create(tmp);
      if (subsong.is_valid()) {
        time_start_in_file += subsong->get_length();
      }
    }
  }
  time_end_in_file = time_start_in_file + metadb->get_length();

  p_format_context = avformat_alloc_context();
  output_packet = av_packet_alloc();
  outputformat_frame = av_frame_alloc();
  p_packet = av_packet_alloc();
  p_frame = av_frame_alloc();

  // open file and find video stream
  libavtry(avformat_open_input(&p_format_context, filename.c_str(), NULL, NULL),
           "open input");
  libavtry(avformat_find_stream_info(p_format_context, NULL),
           "find stream info");

  stream_index = -1;
  for (unsigned i = 0; i < p_format_context->nb_streams; i++) {
    AVCodecParameters* local_codec_params =
        p_format_context->streams[i]->codecpar;

    if (local_codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
      codec = avcodec_find_decoder(local_codec_params->codec_id);
      if (codec == NULL) continue;

      stream_index = i;
      params = local_codec_params;
      break;
    }
  }

  if (stream_index == -1) {
    throw exception_album_art_not_found();
  }

  p_codec_context = avcodec_alloc_context3(codec);
  libavtry(avcodec_parameters_to_context(p_codec_context, params),
           "make codec context");
  libavtry(avcodec_open2(p_codec_context, codec, NULL), "open codec");

  // init output encoding
  if (!cfg_thumb_cache || !db_ptr || !query_get || !query_put ||
      cfg_thumb_cache_format == 2) {
    output_pixelformat = AV_PIX_FMT_BGR24;
    output_encoder = avcodec_find_encoder(AV_CODEC_ID_BMP);
    output_codeccontext = avcodec_alloc_context3(output_encoder);
    set_output_quality = false;
  } else if (cfg_thumb_cache_format == 0) {
    output_pixelformat = AV_PIX_FMT_YUVJ444P;
    output_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    output_codeccontext = avcodec_alloc_context3(output_encoder);
    output_codeccontext->flags |= AV_CODEC_FLAG_QSCALE;
    set_output_quality = true;
  } else if (cfg_thumb_cache_format == 1) {
    output_pixelformat = AV_PIX_FMT_RGB24;
    output_encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    output_codeccontext = avcodec_alloc_context3(output_encoder);
    set_output_quality = false;
  } else {
    console::error("mpv: Could not determine target thumbnail format");
    throw exception_album_art_not_found();
  }
}

thumbnailer::~thumbnailer() {
  av_frame_free(&outputformat_frame);
  av_packet_free(&output_packet);
  avcodec_free_context(&output_codeccontext);

  av_packet_free(&p_packet);
  av_frame_free(&p_frame);
  avcodec_free_context(&p_codec_context);
  avformat_close_input(&p_format_context);
}

void thumbnailer::seek(double fraction) {
  double seek_time =
      time_start_in_file + fraction * (time_end_in_file - time_start_in_file);

  libavtry(av_seek_frame(p_format_context, -1,
                         (int64_t)(seek_time * (double)AV_TIME_BASE),
                         AVSEEK_FLAG_ANY),
           "seek");
}

album_art_data_ptr thumbnailer::encode_output() {
  AVRational aspect_ratio = av_guess_sample_aspect_ratio(
      p_format_context, p_format_context->streams[stream_index], p_frame);

  int scale_to_width = p_frame->width;
  int scale_to_height = p_frame->height;
  if (aspect_ratio.num != 0) {
    scale_to_width = scale_to_width * aspect_ratio.num / aspect_ratio.den;
  }

  int target_dimension = 1;
  switch (cfg_thumb_size) {
    case 0:
      target_dimension = 200;
      break;
    case 1:
      target_dimension = 400;
      break;
    case 2:
      target_dimension = 600;
      break;
    case 3:
      target_dimension = 1000;
      break;
    case 4:
      target_dimension = max(scale_to_width, scale_to_height);
      break;
  }

  if (scale_to_width > scale_to_height) {
    scale_to_height = target_dimension * scale_to_height / scale_to_width;
    scale_to_width = target_dimension;
  } else {
    scale_to_width = target_dimension * scale_to_width / scale_to_height;
    scale_to_height = target_dimension;
  }

  outputformat_frame->format = output_pixelformat;
  outputformat_frame->width = scale_to_width;
  outputformat_frame->height = scale_to_height;
  libavtry(av_frame_get_buffer(outputformat_frame, 32),
           "get output frame buffer");

  int flags = 0;
  switch (cfg_thumb_scaling) {
    case 0:
      flags = SWS_BILINEAR;
      break;
    case 1:
      flags = SWS_BICUBIC;
      break;
    case 2:
      flags = SWS_LANCZOS;
      break;
    case 3:
      flags = SWS_SINC;
      break;
    case 4:
      flags = SWS_SPLINE;
      break;
  }

  SwsContext* swscontext = sws_getContext(
      p_frame->width, p_frame->height, (AVPixelFormat)p_frame->format,
      outputformat_frame->width, outputformat_frame->height, output_pixelformat,
      flags, 0, 0, 0);

  if (swscontext == NULL) {
    throw exception_album_art_not_found();
  }

  sws_scale(swscontext, p_frame->data, p_frame->linesize, 0, p_frame->height,
            outputformat_frame->data, outputformat_frame->linesize);

  output_codeccontext->width = outputformat_frame->width;
  output_codeccontext->height = outputformat_frame->height;
  output_codeccontext->pix_fmt = output_pixelformat;
  output_codeccontext->time_base = AVRational{1, 1};

  if (set_output_quality) {
    output_codeccontext->global_quality =
        (31 - cfg_thumb_cache_quality) * FF_QP2LAMBDA;
    outputformat_frame->quality = output_codeccontext->global_quality;
  }

  libavtry(avcodec_open2(output_codeccontext, output_encoder, NULL),
           "open output codec");

  libavtry(avcodec_send_frame(output_codeccontext, outputformat_frame),
           "send output frame");

  if (output_packet == NULL) {
    throw exception_album_art_not_found();
  }

  libavtry(avcodec_receive_packet(output_codeccontext, output_packet),
           "receive output packet");

  return album_art_data_impl::g_create(output_packet->data,
                                       output_packet->size);
}

void thumbnailer::decode_frame() {
  while (av_read_frame(p_format_context, p_packet) >= 0) {
    if (p_packet->stream_index != stream_index) continue;

    if (avcodec_send_packet(p_codec_context, p_packet) < 0) continue;

    if (avcodec_receive_frame(p_codec_context, p_frame) < 0) continue;

    break;
  }

  if (p_frame->width == 0 || p_frame->height == 0) {
    throw exception_album_art_not_found();
  }

  if (outputformat_frame == NULL) {
    throw exception_album_art_not_found();
  }
}

album_art_data_ptr thumbnailer::get_art() {
  if (cfg_thumb_seektype == 0) {
    // fixed absolute seek
    seek(0.01 * (double)cfg_thumb_seek);
    decode_frame();
  } else {
    // choose a good frame
    seek(0.3);
  }

  return encode_output();
}

class empty_album_art_path_list_impl : public album_art_path_list {
 public:
  empty_album_art_path_list_impl() {}
  const char* get_path(t_size index) const { return NULL; }
  t_size get_count() const { return 0; }

 private:
};

class ffmpeg_thumbnailer : public album_art_extractor_instance_v2 {
 private:
  metadb_handle_list_cref items;

 public:
  ffmpeg_thumbnailer(metadb_handle_list_cref items) : items(items) {}

  album_art_data_ptr query(const GUID& p_what,
                           abort_callback& p_abort) override {
    if (!cfg_thumbs || items.get_size() == 0)
      throw exception_album_art_not_found();

    metadb_handle_ptr item;

    if (cfg_thumb_group_longest) {
      for (int i = 0; i < items.get_size(); i++) {
        if (item.is_empty() || items[i]->get_length() > item->get_length()) {
          item = items[i];
        }
      }
    } else {
      item = items.get_item(0);
    }

    if (cfg_thumb_cache && query_get) {
      try {
        query_get->reset();
        query_get->bind(1, item->get_path());
        query_get->bind(2, item->get_subsong_index());
        if (query_get->executeStep()) {
          SQLite::Column blobcol = query_get->getColumn(0);

          if (blobcol.getBlob() == NULL) {
            std::stringstream msg;
            msg << "mpv: Image was null when fetching cached thumbnail: "
                << item->get_path() << "[" << item->get_subsong_index() << "]";
            console::error(msg.str().c_str());
            throw exception_album_art_not_found();
          }

          auto pic = album_art_data_impl::g_create(blobcol.getBlob(),
                                                   blobcol.getBytes());

          if (cfg_logging) {
            std::stringstream msg;
            msg << "mpv: Fetch from thumbnail cache: " << item->get_path()
                << "[" << item->get_subsong_index() << "]";
            console::info(msg.str().c_str());
          }

          return pic;
        }
      } catch (std::exception e) {
        std::stringstream msg;
        msg << "mpv: Error accessing thumbnail cache: " << e.what();
        console::error(msg.str().c_str());
      }
    }

    thumbnailer ex(item);
    album_art_data_ptr res = ex.get_art();

    if (res == NULL) {
      throw exception_album_art_not_found();
    }

    if (cfg_thumb_cache && query_put) {
      try {
        query_put->reset();
        query_put->bind(1, item->get_path());
        query_put->bind(2, item->get_subsong_index());
        query_put->bind(3, res->get_ptr(), res->get_size());
        query_put->exec();

        if (cfg_logging) {
          std::stringstream msg;
          msg << "mpv: Write to thumbnail cache: " << item->get_path() << "["
              << item->get_subsong_index() << "]";
          console::info(msg.str().c_str());
        }
      } catch (SQLite::Exception e) {
        std::stringstream msg;
        msg << "mpv: Error writing to thumbnail cache: " << e.what();
        console::error(msg.str().c_str());
      }
    }

    return res;
  }

  album_art_path_list::ptr query_paths(const GUID& p_what,
                                       foobar2000_io::abort_callback& p_abort) {
    empty_album_art_path_list_impl* my_list =
        new service_impl_single_t<empty_album_art_path_list_impl>();
    return my_list;
  }
};

class my_album_art_fallback : public album_art_fallback {
 public:
  album_art_extractor_instance_v2::ptr open(
      metadb_handle_list_cref items, pfc::list_base_const_t<GUID> const& ids,
      abort_callback& abort) {
    return new service_impl_t<ffmpeg_thumbnailer>(items);
  }
};

static service_factory_single_t<my_album_art_fallback> g_my_album_art_fallback;
}  // namespace mpv
