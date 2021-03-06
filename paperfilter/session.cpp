#include "session.hpp"
#include "buffer.hpp"
#include "dissector.hpp"
#include "packet_dispatcher.hpp"
#include "filter_thread.hpp"
#include "layer.hpp"
#include "packet.hpp"
#include "packet_store.hpp"
#include "pcap.hpp"
#include "permission.hpp"
#include "stream_chunk.hpp"
#include "stream_dispatcher.hpp"
#include "log_message.hpp"
#include <nan.h>
#include <thread>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <uv.h>
#include <v8pp/class.hpp>
#include <v8pp/object.hpp>
#include <v8pp/json.hpp>

using namespace v8;

struct FilterContext {
  std::vector<std::unique_ptr<FilterThread>> threads;
  std::shared_ptr<FilterThread::Context> ctx;
  std::chrono::time_point<std::chrono::system_clock> startTime =
      std::chrono::system_clock::now();
  uint32_t initialMaxSeq = 0;
};

class Session::Private {
public:
  Private();
  ~Private();
  void log(const LogMessage &msg);
  v8::Local<v8::Object> status();

public:
  std::unique_ptr<PacketStore> store;
  std::unique_ptr<PacketDispatcher> packetDispatcher;
  std::unordered_map<std::string, FilterContext> filterThreads;
  std::string ns;
  std::string config;

  UniquePersistent<Function> statusCb;
  UniquePersistent<Function> logCb;
  uv_async_t statusCbAsync;
  uv_async_t logCbAsync;

  std::unique_ptr<StreamDispatcher> streamDispatcher;
  std::unique_ptr<Pcap> pcap;

  std::mutex errorMutex;
  std::unordered_map<std::string, LogMessage> recentLogs;

  uint32_t prevQueue = 0;
  bool capturing = false;
  int threads;
};

Session::Private::Private() {
  logCbAsync.data = this;
  uv_async_init(uv_default_loop(), &logCbAsync, [](uv_async_t *handle) {
    Session::Private *d = static_cast<Session::Private *>(handle->data);
    if (!d->logCb.IsEmpty()) {
      std::unordered_map<std::string, LogMessage> logs;
      {
        std::lock_guard<std::mutex> lock(d->errorMutex);
        logs.swap(d->recentLogs);
      }

      Isolate *isolate = Isolate::GetCurrent();
      for (const auto &pair : logs) {
        const LogMessage &msg = pair.second;
        Local<Object> obj = Object::New(isolate);

        const char *levels[] = {"debug", "info", "warn", "error"};
        v8pp::set_option(isolate, obj, "level", levels[msg.level]);
        v8pp::set_option(isolate, obj, "message", msg.message);
        v8pp::set_option(isolate, obj, "domain", msg.domain);
        v8pp::set_option(isolate, obj, "resourceName", msg.resourceName);
        v8pp::set_option(isolate, obj, "sourceLine", msg.sourceLine);
        if (msg.lineNumber >= 0)
          v8pp::set_option(isolate, obj, "lineNumber", msg.lineNumber);
        if (msg.startPosition >= 0)
          v8pp::set_option(isolate, obj, "startPosition", msg.startPosition);
        if (msg.endPosition >= 0)
          v8pp::set_option(isolate, obj, "endPosition", msg.endPosition);
        if (msg.startColumn >= 0)
          v8pp::set_option(isolate, obj, "startColumn", msg.startColumn);
        if (msg.endColumn >= 0)
          v8pp::set_option(isolate, obj, "endColumn", msg.endColumn);

        Handle<Value> args[1] = {obj};
        Local<Function> func = Local<Function>::New(isolate, d->logCb);
        func->Call(isolate->GetCurrentContext()->Global(), 1, args);
      }
    }
  });

  statusCbAsync.data = this;
  uv_async_init(uv_default_loop(), &statusCbAsync, [](uv_async_t *handle) {
    Session::Private *d = static_cast<Session::Private *>(handle->data);
    if (!d->statusCb.IsEmpty()) {
      uint32_t packets = d->store->maxSeq();
      uint32_t queue =
          d->packetDispatcher->queueSize() + d->streamDispatcher->queueSize();

      if (packets > 1024 && d->prevQueue > 0 && queue > 1024) {
        int plog = 2 << static_cast<int>(std::log2(d->prevQueue));
        int qlog = 2 << static_cast<int>(std::log2(queue));
        if (plog == qlog && (d->prevQueue / plog) == (queue / plog)) {
          d->prevQueue = queue;
          return;
        }
      }
      d->prevQueue = queue;

      Isolate *isolate = Isolate::GetCurrent();
      Handle<Value> args[1] = {d->status()};
      Local<Function> func = Local<Function>::New(isolate, d->statusCb);
      func->Call(isolate->GetCurrentContext()->Global(), 1, args);
    }
  });
}

v8::Local<v8::Object> Session::Private::status() {
  uint32_t packets = store->maxSeq();
  uint32_t queue =
      packetDispatcher->queueSize() + streamDispatcher->queueSize();

  Isolate *isolate = Isolate::GetCurrent();
  Local<Object> obj = Object::New(isolate);
  v8pp::set_option(isolate, obj, "capturing", capturing);
  v8pp::set_option(isolate, obj, "packets", packets);
  v8pp::set_option(isolate, obj, "queue", queue);
  Local<Object> filtered = Object::New(isolate);

  for (auto &pair : filterThreads) {
    FilterContext &context = pair.second;
    if (context.initialMaxSeq > 0 &&
        context.ctx->packets.maxSeq() >= context.initialMaxSeq) {
      int ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now() - context.startTime)
                   .count();
      LogMessage msg;
      msg.level = LogMessage::LEVEL_DEBUG;
      msg.message = "Filter [" + pair.first + "]: " +
                    std::to_string(context.initialMaxSeq) + "packets / " +
                    std::to_string(ms / 1000.0) + "sec";
      msg.domain = "filter";
      log(msg);
      context.initialMaxSeq = 0;
    }
    v8pp::set_option(isolate, filtered, pair.first.c_str(),
                     context.ctx->packets.size());
  }

  v8pp::set_option(isolate, obj, "filtered", filtered);
  return obj;
}

void Session::Private::log(const LogMessage &msg) {
  {
    std::lock_guard<std::mutex> lock(errorMutex);
    recentLogs[msg.key()] = msg;
  }
  uv_async_send(&logCbAsync);
}

Session::Private::~Private() {
  filterThreads.clear();
  streamDispatcher.reset();
  packetDispatcher.reset();
  pcap.reset();
  uv_close((uv_handle_t *)&statusCbAsync, nullptr);
  uv_close((uv_handle_t *)&logCbAsync, nullptr);
}

Session::Session(v8::Local<v8::Object> option) : d(new Private()) {
  reset(option);
}

Session::~Session() {
  delete d;
  d = nullptr;
}

void Session::analyze(std::unique_ptr<Packet> pkt) {
  const auto &layer = std::make_shared<Layer>(d->ns);
  layer->setName("Frame");
  layer->setPayload(pkt->payload());
  pkt->addLayer(layer);
  d->packetDispatcher->analyze(std::move(pkt));
}

void Session::analyze(std::vector<std::unique_ptr<Packet>> packets) {
  for (auto &pkt : packets) {
    const auto &layer = std::make_shared<Layer>(d->ns);
    layer->setName("Frame");
    layer->setPayload(pkt->payload());
    pkt->addLayer(layer);
  }
  d->packetDispatcher->analyze(std::move(packets));
}

void Session::filter(const std::string &name, const std::string &filter) {
  d->filterThreads.erase(name);

  if (!filter.empty()) {
    FilterContext &context = d->filterThreads[name];
    context.initialMaxSeq = d->store->maxSeq();
    context.ctx = std::make_shared<FilterThread::Context>();
    context.ctx->store = d->store.get();
    context.ctx->filter = filter;
    context.ctx->packets.addHandler(
        [this](uint32_t seq) { uv_async_send(&d->statusCbAsync); });
    context.ctx->logCb =
        std::bind(&Private::log, std::ref(d), std::placeholders::_1);
    for (int i = 0; i < d->threads; ++i) {
      context.threads.emplace_back(new FilterThread(context.ctx));
    }
  }

  uv_async_send(&d->statusCbAsync);
}

v8::Local<v8::Function> Session::logCallback() const {
  return Local<Function>::New(Isolate::GetCurrent(), d->logCb);
}

void Session::setLogCallback(const v8::Local<v8::Function> &cb) {
  d->logCb.Reset(Isolate::GetCurrent(), cb);
}

Local<Function> Session::statusCallback() const {
  return Local<Function>::New(Isolate::GetCurrent(), d->statusCb);
}

void Session::setStatusCallback(const Local<Function> &cb) {
  d->statusCb.Reset(Isolate::GetCurrent(), cb);
}

std::shared_ptr<const Packet> Session::get(uint32_t seq) const {
  return d->store->get(seq);
}

std::vector<uint32_t> Session::getFiltered(const std::string &name,
                                           uint32_t start, uint32_t end) const {
  const auto it = d->filterThreads.find(name);
  if (it == d->filterThreads.end())
    return std::vector<uint32_t>();
  return it->second.ctx->packets.get(start, end);
}

std::string Session::ns() const { return d->ns; }

bool Session::permission() { return Permission::test(); }

v8::Local<v8::Array> Session::devices() {
  Isolate *isolate = Isolate::GetCurrent();
  const std::vector<Pcap::Device> &devs = Pcap::devices();
  v8::Local<v8::Array> array = v8::Array::New(isolate, devs.size());
  for (size_t i = 0; i < devs.size(); ++i) {
    const Pcap::Device &dev = devs[i];
    v8::Local<v8::Object> obj = v8::Object::New(isolate);
    v8pp::set_option(isolate, obj, "id", dev.id);
    v8pp::set_option(isolate, obj, "name", dev.name);
    v8pp::set_option(isolate, obj, "description", dev.description);
    v8pp::set_option(isolate, obj, "link", dev.link);
    v8pp::set_option(isolate, obj, "loopback", dev.loopback);
    array->Set(i, obj);
  }
  return array;
}

void Session::setInterface(const std::string &ifs) {
  d->pcap->setInterface(ifs);
}
std::string Session::networkInterface() const {
  return d->pcap->networkInterface();
}
void Session::setPromiscuous(bool promisc) { d->pcap->setPromiscuous(promisc); }
bool Session::promiscuous() const { return d->pcap->promiscuous(); }
void Session::setSnaplen(int len) { d->pcap->setSnaplen(len); }
int Session::snaplen() const { return d->pcap->snaplen(); }
bool Session::setBPF(const std::string &filter, std::string *error) {
  return d->pcap->setBPF(filter, error);
}

v8::Local<v8::Object> Session::status() const { return d->status(); }

void Session::start() {
  d->pcap->start();
  d->capturing = true;
  uv_async_send(&d->statusCbAsync);
}

void Session::stop() {
  d->pcap->stop();
  d->capturing = false;
  uv_async_send(&d->statusCbAsync);
}

void Session::reset(v8::Local<v8::Object> opt) {
  Isolate *isolate = Isolate::GetCurrent();
  d->prevQueue = 0;

  v8pp::get_option(isolate, opt, "namespace", d->ns);

  v8::Local<v8::Object> config;
  if (v8pp::get_option(isolate, opt, "config", config)) {
    d->config = v8pp::json_str(isolate, config);
  }

  d->threads = std::thread::hardware_concurrency();
  v8pp::get_option(isolate, opt, "threads", d->threads);
  d->threads = std::max(1, d->threads - 1);

  Local<Array> dissectorArray;
  std::vector<Dissector> dissectors;
  if (v8pp::get_option(isolate, opt, "dissectors", dissectorArray)) {
    for (uint32_t i = 0; i < dissectorArray->Length(); ++i) {
      Local<Value> diss = dissectorArray->Get(i);
      if (!diss.IsEmpty() && diss->IsObject()) {
        dissectors.emplace_back(diss.As<Object>());
      }
    }
  }

  Local<Array> streamDissectorArray;
  std::vector<Dissector> streamDissectors;
  if (v8pp::get_option(isolate, opt, "stream_dissectors",
                       streamDissectorArray)) {
    for (uint32_t i = 0; i < streamDissectorArray->Length(); ++i) {
      Local<Value> diss = streamDissectorArray->Get(i);
      if (!diss.IsEmpty() && diss->IsObject()) {
        streamDissectors.emplace_back(diss.As<Object>());
      }
    }
  }

  auto dissCtx = std::make_shared<PacketDispatcher::Context>();
  dissCtx->threads = d->threads;
  dissCtx->config = d->config;
  dissCtx->packetCb = [this](
      const std::vector<std::shared_ptr<Packet>> &packets) {
    d->store->insert(packets);
  };
  dissCtx->streamsCb = [this](
      uint32_t seq, std::vector<std::unique_ptr<StreamChunk>> streams) {
    if (d->streamDispatcher)
      d->streamDispatcher->insert(seq, std::move(streams));
  };
  dissCtx->dissectors.swap(dissectors);
  dissCtx->logCb = std::bind(&Private::log, std::ref(d), std::placeholders::_1);
  d->packetDispatcher.reset(new PacketDispatcher(dissCtx));

  auto streamCtx = std::make_shared<StreamDispatcher::Context>();
  streamCtx->threads = d->threads;
  streamCtx->config = d->config;
  streamCtx->dissectors.swap(streamDissectors);
  streamCtx->logCb =
      std::bind(&Private::log, std::ref(d), std::placeholders::_1);
  streamCtx->streamsCb = [this](
      std::vector<std::unique_ptr<StreamChunk>> streams) {
    d->streamDispatcher->insert(std::move(streams));
  };
  streamCtx->vpLayersCb = [this](std::vector<std::unique_ptr<Layer>> layers) {
    for (auto &layer : layers) {
      d->packetDispatcher->analyze(
          std::unique_ptr<Packet>(new Packet(std::move(layer))));
    }
  };
  d->streamDispatcher.reset(new StreamDispatcher(streamCtx));

  auto pcapCtx = std::make_shared<Pcap::Context>();
  pcapCtx->logCb = std::bind(&Private::log, std::ref(d), std::placeholders::_1);
  pcapCtx->packetCb = [this](std::unique_ptr<Packet> pkt) {
    analyze(std::move(pkt));
  };
  d->pcap.reset(new Pcap(pcapCtx));

  std::vector<std::shared_ptr<Packet>> packets;
  if (d->store) {
    packets = d->store->get(1, d->store->maxSeq());
  }
  auto storeCb = [this](uint32_t maxSeq) { uv_async_send(&d->statusCbAsync); };
  d->store.reset(new PacketStore());
  d->store->addHandler(storeCb);

  std::vector<std::pair<std::string, std::string>> filters;
  for (const auto &pair : d->filterThreads) {
    filters.push_back(std::make_pair(pair.first, pair.second.ctx->filter));
  }
  d->filterThreads.clear();
  for (const auto &pair : filters) {
    filter(pair.first, pair.second);
  }

  for (const auto &pkt : packets) {
    if (!pkt->vpacket()) {
      analyze(pkt->shallowClone());
    }
  }

  uv_async_send(&d->statusCbAsync);
}
