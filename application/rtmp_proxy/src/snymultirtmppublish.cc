/*
 *copyleft (c) 2021 www.streamingnology.com
 *code released under GPL license
 */

#include "snymultirtmppublish.h"
#include <core/snyeasylogging.h>
#include <media/mux/snymuxerfactory.h>
namespace app {
SnyMultiRTMPPublish::SnyMultiRTMPPublish(std::string name) : threads_(this) { name_ = std::move(name); }

SnyMultiRTMPPublish::~SnyMultiRTMPPublish() { stop(); }

void SnyMultiRTMPPublish::start() {
  for (int i = 0; i < publish_streams_.size(); i++) {
    threads_.start(i);
  }
}

void SnyMultiRTMPPublish::stop() {
  threads_.stopAll();
  for (auto& item : cv_) {
    item.second.notify_all();
  }
  threads_.waitAll();
}

int SnyMultiRTMPPublish::onThreadProc(int id) {
  std::string publish_name = publish_streams_[id].publish_name_;
  std::string publish_url = publish_streams_[id].publish_url_;
  auto rtmp_muxer = createRtmpMuxer(publish_url);
  if (!rtmp_muxer) {
    LOG(ERROR) << "failed to create muxer for " << publish_url;
    return id;
  }
  std::unique_lock<std::mutex> lock(mutex_cv_[id]);
  while (!threads_.isStop(id)) {
    mutex_.lock();
    std::shared_ptr<sny::SnyMediaSample> sample = nullptr;
    if (!samples[id].empty()) {
      sample = samples[id].front();
      samples[id].pop_front();
    }
    mutex_.unlock();
    if (sample) {
      rtmp_muxer->writeSample(sample);
    } else {
      // std::this_thread::sleep_for(5ms);
      cv_[id].wait(lock);
    }
  }
  return id;
}

void SnyMultiRTMPPublish::onRtmpAppStreamName(std::string conn_name, std::string app_name, std::string stream_name) {
  LOG(DEBUG) << "/" << app_name << "/" << stream_name;
  app_name_ = app_name;
  stream_name_ = stream_name;
}

void SnyMultiRTMPPublish::onTrack(std::string conn_name, std::map<int32_t, std::shared_ptr<MediaTrack>> tracks) {
  tracks_ = tracks;
  auto& streams = cnf_->streams_;
  for (auto& item : cnf_->streams_) {
    if (item.app_name_ == app_name_ && item.stream_name_ == stream_name_) {
      publish_streams_ = item.publish_items_;
      break;
    }
  }
  for (int i = 0; i < publish_streams_.size(); i++) {
    std::deque<std::shared_ptr<sny::SnyMediaSample>> q;
    samples.insert(std::make_pair(i, q));
  }
  start();
}

void SnyMultiRTMPPublish::onSample(std::string conn_name, std::shared_ptr<sny::SnyMediaSample> sample) {
  mutex_.lock();
  for (auto& item : samples) {
    item.second.push_back(sample);
  }
  mutex_.unlock();
  for (auto& item : cv_) {
    item.second.notify_all();
  }
}

std::shared_ptr<sny::SnyIMuxer> SnyMultiRTMPPublish::createRtmpMuxer(const std::string& url) {
  auto muxer = sny::SnyMuxerFactory::createMux(sny::SnyMuxerFactory::kFFMPEGMUX);
  muxer->setPath(url, "flv");
  for (const auto& item : tracks_) {
    auto& track = item.second;
    muxer->addMediaTrack(track);
  }
  if (muxer->open()) {
    return muxer;
  } else {
    return nullptr;
  }
}
}  // namespace app