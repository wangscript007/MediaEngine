#include "rtmp_writer.h"
#include "core/snyeasylogging.h"
/*
  [Test Code]

  _writer = RtmpWriter::Create();
  _writer->SetPath("/tmp/output.ts");

  for(auto &track_item : _tracks)
  {
    auto &track = track_item.second;

    if( track->GetCodecId() == cmn::MediaCodecId::Opus )
      continue;

    auto quality = RtmpTrackInfo::Create();

    quality->SetCodecId( track->GetCodecId() );
    quality->SetBitrate( track->GetBitrate() );
    quality->SetTimeBase( track->GetTimeBase() );
    quality->SetWidth( track->GetWidth() );
    quality->SetHeight( track->GetHeight() );
    quality->SetSample( track->GetSample() );
    quality->SetChannel( track->GetChannel() );

    _writer->AddTrack(track->GetMediaType(), track->GetId(), quality);
  }

  _writer->Start();

  while(1)
  {
    _writer->PutData(
      media_packet->GetTrackId(),
      media_packet->GetPts(),
      media_packet->GetDts(),
      media_packet->GetFlag(),
      media_packet->GetData());
  }

  _writer->Stop();

*/

std::shared_ptr<RtmpWriter> RtmpWriter::Create() {
  auto object = std::make_shared<RtmpWriter>();

  return object;
}

RtmpWriter::RtmpWriter() : _format_context(nullptr) {
  av_register_all();
  avformat_network_init();
  av_log_set_callback(RtmpWriter::FFmpegLog);
  // av_log_set_level(AV_LOG_DEBUG);
}

RtmpWriter::~RtmpWriter() { Stop(); }

bool RtmpWriter::SetPath(const std::string &path, const std::string &format) {
  std::unique_lock<std::mutex> mlock(_lock);
  if (path.empty()) {
    LOG(ERROR) << "empty path";
    return false;
  }
  _path = path;

  // Release the allcated context;
  if (_format_context != nullptr) {
    avformat_close_input(&_format_context);
    avformat_free_context(_format_context);
    _format_context = nullptr;
  }

  AVOutputFormat *output_format = nullptr;

  // If the format is nullptr, it is automatically set based on the extension.
  if (!format.empty()) {
    output_format = av_guess_format(format.c_str(), nullptr, nullptr);
    if (output_format == nullptr) {
      LOG(ERROR) << "Unknown format. format(" << format << ")";
      return false;
    }
  }

  int error = avformat_alloc_output_context2(&_format_context, output_format, nullptr, path.c_str());
  if (error < 0) {
    char errbuf[256];
    av_strerror(error, errbuf, sizeof(errbuf));
    auto log = ov::String::FormatString("Could not create output context. error(%d:%s), path(%s)", error, errbuf,
                                        path.c_str());
    LOG(ERROR) << log;
    return false;
  }

  return true;
}

std::string RtmpWriter::GetPath() { return _path; }

bool RtmpWriter::Start() {
  std::unique_lock<std::mutex> mlock(_lock);
  AVDictionary *options = nullptr;
  // Examples
  // av_dict_set(&out_options, "timeout", "1000000", 0);
  // av_dict_set(&out_options, "tcp_nodelay", "1", 0);
  // _format_context->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
  if (!(_format_context->oformat->flags & AVFMT_NOFILE)) {
    int error = avio_open2(&_format_context->pb, _format_context->filename, AVIO_FLAG_WRITE, nullptr, &options);
    if (error < 0) {
      char errbuf[256];
      av_strerror(error, errbuf, sizeof(errbuf));
      auto log = ov::String::FormatString("Error opening file. error(%d:%s), filename(%s)", error, errbuf,
                                          _format_context->filename);
      LOG(ERROR) << log;
      return false;
    }
  }

  if (avformat_write_header(_format_context, nullptr) < 0) {
    LOG(ERROR) << "Could not create header";
    return false;
  }

  av_dump_format(_format_context, 0, _format_context->filename, 1);

  if (_format_context->oformat != nullptr) {
    auto oformat = _format_context->oformat;
    auto log = ov::String::FormatString("name : %s, long_name : %s, mime_type : %s, audio_codec : %d, video_codec : %d",
                                        oformat->name, oformat->long_name, oformat->mime_type, oformat->audio_codec,
                                        oformat->video_codec);
    LOG(INFO) << log.CStr();
  }
  return true;
}

bool RtmpWriter::Stop() {
  std::unique_lock<std::mutex> mlock(_lock);
  if (_format_context != nullptr) {
    if (_format_context->pb != nullptr) {
      av_write_trailer(_format_context);
    }
    avformat_close_input(&_format_context);
    avformat_free_context(_format_context);
    _format_context = nullptr;
  }
  return true;
}

bool RtmpWriter::AddTrack(cmn::MediaType media_type, int32_t track_id,
                          const std::shared_ptr<RtmpTrackInfo> &track_info) {
  std::unique_lock<std::mutex> mlock(_lock);
  AVStream *stream;
  // Stream #0:0(und): Video: h264 (Constrained Baseline) ([7][0][0][0] / 0x0007), yuv420p, 640x360 [SAR 1:1 DAR 16:9],
  // q=2-31, 683 kb/s, 24 fps, 24 tbr, 1k tbn, 90k tbc (default) Stream #0:0:     Video: h264, 1 reference frame
  // ([7][0][0][0] / 0x0007), yuv420p, 1920x1080 (0x0) [SAR 1:1 DAR 16:9], 0/1, q=2-31, 2500 kb/s
  // 	[12-02 17:03:03.131] I 25357 FFmpeg | third_parties.cpp:118  |     Stream #0:0
  // [12-02 17:03:03.131] I 25357 FFmpeg | third_parties.cpp:118  | : Video: h264 (Constrained Baseline), 1 reference
  // frame ([7][0][0][0] / 0x0007), yuv420p, 1920x1080 [SAR 1:1 DAR 16:9], 0/1, q=2-31, 2000 kb/s, 1k tbn, 30 tbc
  switch (media_type) {
    case cmn::MediaType::Video: {
      stream = avformat_new_stream(_format_context, nullptr);
      AVCodecParameters *codecpar = stream->codecpar;
      codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
      codecpar->codec_id =
          (track_info->GetCodecId() == cmn::MediaCodecId::H264)
              ? AV_CODEC_ID_H264
              : (track_info->GetCodecId() == cmn::MediaCodecId::H265)
                    ? AV_CODEC_ID_H265
                    : (track_info->GetCodecId() == cmn::MediaCodecId::Vp8)
                          ? AV_CODEC_ID_VP8
                          : (track_info->GetCodecId() == cmn::MediaCodecId::Vp9) ? AV_CODEC_ID_VP9 : AV_CODEC_ID_NONE;
      codecpar->codec_tag = 0;
      codecpar->bit_rate = track_info->GetBitrate();
      codecpar->width = track_info->GetWidth();
      codecpar->height = track_info->GetHeight();
      codecpar->format = AV_PIX_FMT_YUV420P;
      codecpar->sample_aspect_ratio = AVRational{1, 1};

      // set extradata for avc_decoder_configuration_record
      if (!track_info->GetExtradata().empty()) {
        codecpar->extradata_size = track_info->GetExtradata().size();
        codecpar->extradata = (uint8_t *)av_malloc(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memset(codecpar->extradata, 0, codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(codecpar->extradata, &track_info->GetExtradata()[0], codecpar->extradata_size);
      } else {
        LOG(WARNING) << "there is no extra data found in video track";
      }

      stream->display_aspect_ratio = AVRational{1, 1};
      stream->time_base = AVRational{track_info->GetTimeBase().GetNum(), track_info->GetTimeBase().GetDen()};

      _track_map[track_id] = stream->index;
      _trackinfo_map[track_id] = track_info;
    } break;

    case cmn::MediaType::Audio: {
      stream = avformat_new_stream(_format_context, nullptr);
      AVCodecParameters *codecpar = stream->codecpar;

      codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
      codecpar->codec_id =
          (track_info->GetCodecId() == cmn::MediaCodecId::Aac)
              ? AV_CODEC_ID_AAC
              : (track_info->GetCodecId() == cmn::MediaCodecId::Mp3)
                    ? AV_CODEC_ID_MP3
                    : (track_info->GetCodecId() == cmn::MediaCodecId::Opus) ? AV_CODEC_ID_OPUS : AV_CODEC_ID_NONE;
      codecpar->bit_rate = track_info->GetBitrate();
      codecpar->channels = static_cast<int>(track_info->GetChannel().GetCounts());
      codecpar->channel_layout = (track_info->GetChannel().GetLayout() == cmn::AudioChannel::Layout::LayoutMono)
                                     ? AV_CH_LAYOUT_MONO
                                     : (track_info->GetChannel().GetLayout() == cmn::AudioChannel::Layout::LayoutStereo)
                                           ? AV_CH_LAYOUT_STEREO
                                           : 0;  // <- Unknown
      codecpar->sample_rate = track_info->GetSample().GetRateNum();
      codecpar->frame_size = 1024;  // TODO: Need to Frame Size
      codecpar->codec_tag = 0;

      // set extradata for aac_specific_config
      if (!track_info->GetExtradata().empty()) {
        codecpar->extradata_size = track_info->GetExtradata().size();
        codecpar->extradata = (uint8_t *)av_malloc(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memset(codecpar->extradata, 0, codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(codecpar->extradata, &track_info->GetExtradata()[0], codecpar->extradata_size);
      }

      stream->time_base = AVRational{track_info->GetTimeBase().GetNum(), track_info->GetTimeBase().GetDen()};

      _track_map[track_id] = stream->index;
      _trackinfo_map[track_id] = track_info;
    } break;

    default: {
      auto log = ov::String::FormatString("This media type is not supported. media_type(%d)", media_type);
      LOG(WARNING) << log;
      return false;
    } break;
  }

  return true;
}

// MP4
//	- H264 : AnnexB bitstream
// 	- AAC : ASC(Audio Specific Config) bitstream
bool RtmpWriter::PutData(std::shared_ptr<sny::SnyMediaSample> &media_sample) {
  std::unique_lock<std::mutex> mlock(_lock);
  if (_format_context == nullptr) return false;
  // Find AVStream and Index;
  int stream_index;
  auto iter = _track_map.find(media_sample->getTrackID());
  if (iter == _track_map.end()) {
    auto log = ov::String::FormatString("There is no track id %d", media_sample->getTrackID());
    LOG(WARNING) << log;
    // Without a track, it's not an error. Ignore.
    return true;
  }
  stream_index = iter->second;
  AVStream *stream = _format_context->streams[stream_index];
  if (stream == nullptr) {
    LOG(WARNING) << "There is no stream";
    return false;
  }
  // Find Ouput Track Info
  auto track_info = _trackinfo_map[media_sample->getTrackID()];
  // Make avpacket
  AVPacket pkt = {nullptr};
  av_init_packet(&pkt);
  pkt.stream_index = stream_index;
  pkt.flags = media_sample->isKey() ? AV_PKT_FLAG_KEY : 0;
  pkt.pts = av_rescale_q(media_sample->pts(),
                         AVRational{track_info->GetTimeBase().GetNum(), track_info->GetTimeBase().GetDen()},
                         stream->time_base);
  pkt.dts = av_rescale_q(media_sample->dts(),
                         AVRational{track_info->GetTimeBase().GetNum(), track_info->GetTimeBase().GetDen()},
                         stream->time_base);

  // TODO: Depending on the extension, the bitstream format should be changed.
  // format(mpegts)
  // 	- H264 : Passthrough(AnnexB)
  //  - H265 : Passthrough(AnnexB)
  //  - AAC : Passthrough(ADTS)
  //  - OPUS : Passthrough (?)
  //  - VP8 : Passthrough (unknown name)
  // format(flv)
  //	- H264 : AnnexB -> AVCC
  //	- AAC : to LATM
  // format(mp4)
  //	- H264 : Passthrough
  //	- AAC : to LATM
  if (media_sample->getBitStreamFormat() == sny::kBitStreamH264AVCC ||
      media_sample->getBitStreamFormat() == sny::kBitStreamAACLATM) {
    pkt.size = media_sample->size();
    pkt.data = (uint8_t *)media_sample->data();
  }
  int error = av_interleaved_write_frame(_format_context, &pkt);
  if (error != 0) {
    char errbuf[256];
    av_strerror(error, errbuf, sizeof(errbuf));
    auto log = ov::String::FormatString("Send packet error(%d:%s)", error, errbuf);
    LOG(WARNING) << log;
    return false;
  }

  return true;
}

// FFMPEG DEBUG
void RtmpWriter::FFmpegLog(void *ptr, int level, const char *fmt, va_list vl) {
  va_list vl2;
  char line[1024];
  static int print_prefix = 1;

  va_copy(vl2, vl);
  av_log_default_callback(ptr, level, fmt, vl);
  av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
  va_end(vl2);

  // if(level >= AV_LOG_DEBUG) return;
  ov::String log;
  log.AppendVFormat(fmt, vl);
  switch (level) {
    case AV_LOG_QUIET:
    case AV_LOG_DEBUG:
    case AV_LOG_VERBOSE:
      // LOG(DEBUG)<<log;
      logd(OV_LOG_TAG, line, vl);
      break;
    case AV_LOG_INFO:
      // LOG(INFO)<<log;
      logi(OV_LOG_TAG, line, vl);
      break;
    case AV_LOG_WARNING:
      // LOG(WARNING)<<log;
      logw(OV_LOG_TAG, line, vl);
      break;
    case AV_LOG_ERROR:
    case AV_LOG_FATAL:
    case AV_LOG_PANIC:
      // LOG(ERROR)<<log;
      loge(OV_LOG_TAG, line, vl);
      break;
    case AV_LOG_TRACE:
      // log_level = ANDROID_LOG_VERBOSE;
      break;
    default:
      break;
  }
}
