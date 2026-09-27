#include "qcae/resource_client.hpp"
#include "qcae/resource_store.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace qcae;
namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    check(result.ok(), result.error ? result.error->message.c_str() : "Missing resource result");
    return std::move(*result.value);
}
template <class Predicate> void wait_until(Predicate predicate, const char* message) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        check(elapsed.elapsed() < 5000, message);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}
ResourceVersion version() {
    return {{DocumentId("resource-document"), DocumentEpoch("resource-epoch")}, 42, "view", 9};
}
QByteArray payload(qsizetype size) {
    QByteArray bytes(size, '\0');
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = static_cast<char>((i * 37) ^ (i >> 8));
    return bytes;
}
QByteArray encoded_response(const QJsonObject& data) {
    return QJsonDocument(
               QJsonObject{{"request_id", "resource-test"}, {"status", "success"}, {"data", data}})
               .toJson(QJsonDocument::Compact) +
           '\n';
}
void store_roundtrip_and_bounds() {
    ipc::ResourceStore store;
    const Caller caller{"owner"};
    const auto bytes = payload(4 * 1024 * 1024);
    const auto manifest = good(store.publish(caller, version(), bytes, "test.bytes.v1"));
    check(manifest.byte_length == 4 * 1024 * 1024 && manifest.chunk_count == 32,
          "4MiB manifest has incorrect length or chunk count");
    check(good(ResourceClient::parseManifest(ipc::resource_manifest_json(manifest))).sha256 ==
              manifest.sha256,
          "Resource manifest JSON failed its own decoder");
    QByteArray collected;
    qsizetype largest_wire{};
    for (std::uint64_t offset = 0; offset < manifest.byte_length; offset += resource_chunk_bytes) {
        const auto chunk = good(store.read(caller, manifest.resource_id, version(), offset));
        check(chunk.bytes.size() <= resource_chunk_bytes, "Raw resource chunk exceeded 128KiB");
        largest_wire =
            std::max(largest_wire, encoded_response(ipc::resource_chunk_json(chunk)).size());
        collected += chunk.bytes;
    }
    check(largest_wire <= resource_max_wire_bytes, "Base64 chunk envelope exceeded 256KiB");
    check(
        collected == bytes &&
            QCryptographicHash::hash(collected, QCryptographicHash::Sha256).toHex().toStdString() ==
                manifest.sha256,
        "4MiB resource roundtrip length, contents or SHA-256 differs");
    check(!store.read(caller, manifest.resource_id, version(), 1).ok(),
          "Unaligned resource offset accepted");
    check(!store.read(caller, manifest.resource_id, version(), manifest.byte_length).ok(),
          "Out-of-range resource offset accepted");
    check(!store.describe(Caller{"other"}, manifest.resource_id, version()).ok(),
          "Resource lease escaped caller ownership");
    check(!store.release(Caller{"other"}, manifest.resource_id).ok(),
          "Other caller released a resource lease");
    for (int changed = 0; changed < 5; ++changed) {
        auto current = version();
        if (changed == 0)
            current.document.id = DocumentId("another");
        if (changed == 1)
            current.document.epoch = DocumentEpoch("another");
        if (changed == 2)
            ++current.revision;
        if (changed == 3)
            current.view_session_id = "another";
        if (changed == 4)
            ++current.view_revision;
        check(!store.describe(caller, manifest.resource_id, current).ok() &&
                  !store.read(caller, manifest.resource_id, current, 0).ok(),
              "Wrong document, epoch, model or view version was accepted");
    }
    check(!store.publish(caller, version(), QByteArray(resource_max_bytes + 1, 'x'), "test").ok(),
          "Resource byte cap was not enforced");
    std::cout << "resource evidence: raw_bytes=" << bytes.size()
              << " chunks=" << manifest.chunk_count << " max_raw_chunk=" << resource_chunk_bytes
              << " max_framed_base64_bytes=" << largest_wire << " sha256=" << manifest.sha256
              << '\n';
}
void leases_and_eviction() {
    auto now = std::chrono::steady_clock::time_point{};
    ipc::ResourceStore::Limits limits{16, 32, 2, std::chrono::milliseconds(10)};
    ipc::ResourceStore store(limits, [&] { return now; });
    const Caller caller{"owner"};
    QByteArray original(16, 'a');
    const auto first = good(store.publish(caller, version(), original, "test"));
    original[0] = 'b';
    check(good(store.read(caller, first.resource_id, version(), 0)).bytes[0] == 'a',
          "Published immutable bytes changed through caller's QByteArray");
    const auto second = good(store.publish(caller, version(), original, "test"));
    check(!store.publish(caller, version(), original, "test").ok(),
          "Active leases were evicted to bypass the cache cap");
    good(store.release(caller, first.resource_id));
    const auto third = good(store.publish(caller, version(), original, "test"));
    check(!store.describe(caller, first.resource_id, version()).ok(),
          "Released least-recent resource was not evicted");
    check(store.describe(caller, second.resource_id, version()).ok() &&
              store.describe(caller, third.resource_id, version()).ok() &&
              store.cached_bytes() == 32 && store.size() == 2,
          "Cache eviction broke active leases or byte accounting");
    now += std::chrono::milliseconds(11);
    check(!store.read(caller, second.resource_id, version(), 0).ok() && store.cached_bytes() == 0 &&
              store.size() == 0,
          "Expired resources retained data or remained readable");
}

class WireEngine {
  public:
    using Transform = std::function<bool(const QJsonObject&, QJsonObject&)>;
    WireEngine() : directory("/tmp/qc3-resource-XXXXXX") {
        check(directory.isValid(), "Resource test temporary directory creation failed");
        if (!server.listen(endpoint()))
            throw std::runtime_error("Resource test socket listen failed at " +
                                     endpoint().toStdString() + ": " +
                                     server.errorString().toStdString());
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            peer = server.nextPendingConnection();
            QObject::connect(peer, &QLocalSocket::readyRead, &server, [this] {
                incoming += peer->readAll();
                while (true) {
                    const auto newline = incoming.indexOf('\n');
                    if (newline < 0)
                        return;
                    const auto request = QJsonDocument::fromJson(incoming.left(newline)).object();
                    incoming.remove(0, newline + 1);
                    handle(request);
                }
            });
        });
    }
    QString endpoint() const {
        return directory.filePath("engine.sock");
    }
    ResourceManifest publish(const QByteArray& bytes) {
        return good(store.publish(caller, current, bytes, "test.bytes.v1"));
    }
    void send(const QJsonObject& reply) {
        const auto wire = QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n';
        max_wire = std::max(max_wire, wire.size());
        peer->write(wire);
    }
    void handle(const QJsonObject& request) {
        const auto operation = request.value("operation").toString();
        QJsonObject data;
        bool ok = true;
        if (operation == "runtime.handshake")
            data = {{"api_version", "1.1"}};
        else if (operation == "test.barrier") {
        } else {
            const auto params = request.value("parameters").toObject();
            const auto id = params.value("resource_id").toString().toStdString();
            if (operation == "resources.release") {
                ++releases;
                ok = store.release(caller, id).ok();
            } else {
                check(request.value("document_id").toString().toStdString() ==
                              current.document.id.value &&
                          request.value("document_epoch").toString().toStdString() ==
                              current.document.epoch.value &&
                          request.value("expected_revision").toString().toULongLong() ==
                              current.revision &&
                          request.value("requested_version").toInt() == 1 &&
                          params.value("view_session_id").toString().toStdString() ==
                              current.view_session_id &&
                          params.value("expected_view_revision").toString().toULongLong() ==
                              current.view_revision,
                      "ResourceClient omitted its pinned request context");
                if (operation == "resources.describe") {
                    ++describes;
                    const auto result = store.describe(caller, id, current);
                    ok = result.ok();
                    if (ok)
                        data = ipc::resource_manifest_json(*result.value);
                } else if (operation == "resources.read") {
                    ++reads;
                    const auto result = store.read(
                        caller, id, current, params.value("offset").toString().toULongLong());
                    ok = result.ok();
                    if (ok)
                        data = ipc::resource_chunk_json(*result.value);
                } else
                    throw std::runtime_error("Unexpected resource client operation");
            }
        }
        QJsonObject response{{"request_id", request.value("request_id")},
                             {"status", ok ? "success" : "failed"},
                             {"data", data}};
        if (!ok)
            response.insert(
                "error",
                QJsonObject{{"code", "INVALID_INPUT"}, {"message", "Resource unavailable"}});
        if (!transform || transform(request, response))
            send(response);
    }
    QTemporaryDir directory;
    QLocalServer server;
    QPointer<QLocalSocket> peer;
    QByteArray incoming;
    ipc::ResourceStore store;
    Caller caller{"wire-owner"};
    ResourceVersion current{version()};
    Transform transform;
    int reads{}, describes{}, releases{};
    qsizetype max_wire{};
};
void barrier(DesktopClient& client) {
    bool reached = false;
    (void)client.request("test.barrier", {}, {}, [&](const auto&) { reached = true; });
    wait_until([&] { return reached; }, "Resource client barrier timed out");
}
Result<QByteArray> fetch(ResourceClient& resources, const ResourceManifest& manifest) {
    std::optional<Result<QByteArray>> result;
    resources.fetch(manifest, [&](auto value) { result = std::move(value); });
    wait_until([&] { return result.has_value(); }, "Resource transfer did not complete or reject");
    return std::move(*result);
}
void client_roundtrip_and_rejections() {
    WireEngine engine;
    DesktopClient client({engine.endpoint(), engine.directory.path(), {}, false, 150});
    client.start();
    wait_until([&] { return client.ready(); }, "Resource client handshake failed");
    ResourceClient resources(client);
    resources.setContext(engine.current);
    const auto large = payload(4 * 1024 * 1024);
    check(good(fetch(resources, engine.publish(large))) == large,
          "Real resource client 4MiB transfer changed bytes");
    check(engine.reads == 32 && engine.max_wire <= resource_max_wire_bytes,
          "Resource client exceeded chunk count or encoded wire cap");
    barrier(client);
    check(engine.releases == 1, "Completed resource transfer leaked its lease");
    check(good(fetch(resources, engine.publish({}))).isEmpty(),
          "Empty resource could not complete");
    const auto bytes = payload(resource_chunk_bytes * 2 + 7);
    int rejected = 0;
    for (const auto* fault : {"missing",
                              "out-of-order",
                              "duplicate",
                              "offset",
                              "length",
                              "digest",
                              "document",
                              "epoch",
                              "revision",
                              "view",
                              "resource",
                              "base64"}) {
        int chunks = 0;
        QJsonObject first;
        engine.transform = [&, fault = std::string(fault)](const auto& request, auto& response) {
            if (request.value("operation") != "resources.read")
                return true;
            auto data = response.value("data").toObject();
            ++chunks;
            if (chunks == 1)
                first = data;
            if (fault == "missing" && chunks == 2)
                return false;
            if (fault == "duplicate" && chunks == 2)
                data = first;
            if (fault == "out-of-order" && chunks == 1)
                data.insert("offset", QString::number(resource_chunk_bytes));
            if (fault == "offset")
                data.insert("offset", "1");
            if (fault == "length")
                data.insert("raw_length", data.value("raw_length").toInt() - 1);
            if (fault == "digest") {
                auto raw = QByteArray::fromBase64(data.value("data_base64").toString().toLatin1());
                raw[0] ^= 1;
                data.insert("data_base64", QString::fromLatin1(raw.toBase64()));
            }
            auto manifest = data.value("manifest").toObject();
            if (fault == "document")
                manifest.insert("document_id", "wrong");
            if (fault == "epoch")
                manifest.insert("document_epoch", "wrong");
            if (fault == "revision")
                manifest.insert("revision", "43");
            if (fault == "view")
                manifest.insert("view_revision", "10");
            if (fault == "resource")
                manifest.insert("resource_id", "wrong");
            if (fault == "base64") {
                auto encoded = data.value("data_base64").toString();
                encoded[0] = '?';
                data.insert("data_base64", encoded);
            }
            data.insert("manifest", manifest);
            response.insert("data", data);
            return true;
        };
        const auto result = fetch(resources, engine.publish(bytes));
        check(!result.ok() && !result.value,
              "Invalid resource exposed partial or successful bytes");
        ++rejected;
        barrier(client);
    }
    engine.transform = {};
    auto excessive = engine.publish(bytes);
    excessive.byte_length = resource_max_bytes + 1;
    const auto describes = engine.describes;
    check(!fetch(resources, excessive).ok() && engine.describes == describes,
          "Excessive manifest initiated a resource transfer");
    good(engine.store.release(engine.caller, excessive.resource_id));
    std::cout << "resource client evidence: 4MiB exact, max_framed_base64_bytes=" << engine.max_wire
              << " rejected_samples=" << rejected << " invalid_display_publications=0\n";
}
void stale_callbacks_cannot_publish() {
    WireEngine engine;
    DesktopClient client({engine.endpoint(), engine.directory.path(), {}, false});
    client.start();
    wait_until([&] { return client.ready(); }, "Resource client handshake failed");
    const auto manifest = engine.publish(payload(resource_chunk_bytes + 1));
    for (int changed = 0; changed < 4; ++changed) {
        QJsonObject held;
        engine.transform = [&](const auto& request, auto& response) {
            if (request.value("operation") == "resources.read") {
                held = response;
                return false;
            }
            return true;
        };
        int publications = 0;
        auto resources = std::make_unique<ResourceClient>(client);
        resources->setContext(engine.current);
        resources->fetch(manifest, [&](auto) { ++publications; });
        wait_until([&] { return !held.isEmpty(); }, "Resource chunk was not held");
        auto next = engine.current;
        if (changed == 0)
            next.document.id = DocumentId("next");
        if (changed == 1)
            next.document.epoch = DocumentEpoch("next");
        if (changed == 2)
            ++next.revision;
        if (changed == 3)
            resources.reset();
        else
            resources->setContext(next);
        engine.send(held);
        barrier(client);
        check(publications == 0, "Changed context or destroyed client published an old resource");
    }
    check(engine.releases == 4, "Cancelled or destroyed resource fetches leaked leases");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        store_roundtrip_and_bounds();
        leases_and_eviction();
        client_roundtrip_and_rejections();
        stale_callbacks_cannot_publish();
        std::cout << "PASS: versioned resources, leases, bounded wire, validated client assembly\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
