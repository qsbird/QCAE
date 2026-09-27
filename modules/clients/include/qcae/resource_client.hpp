#pragma once
#include "qcae/desktop_client.hpp"
#include "qcae/resource.hpp"
#include <QByteArray>
#include <QPointer>
#include <memory>

namespace qcae {
// A single bounded transfer. Replacing its context or request cancels publication of old
// callbacks; only bytes that passed all manifest, chunk and digest checks reach Reply.
class ResourceClient : public QObject {
  public:
    using Reply = std::function<void(Result<QByteArray>)>;
    explicit ResourceClient(DesktopClient&, QObject* parent = nullptr);
    ~ResourceClient() override;
    void setContext(const ResourceVersion&);
    void clear();
    void fetch(const ResourceManifest&, Reply);
    static Result<ResourceManifest> parseManifest(const QJsonObject&);

  private:
    struct Fetch;
    void abandon();
    void release(const ResourceManifest&);
    void request(const std::shared_ptr<Fetch>&,
                 const QString&,
                 const QJsonObject&,
                 DesktopClient::Reply);
    void readNext(const std::shared_ptr<Fetch>&);
    void finish(const std::shared_ptr<Fetch>&, Result<QByteArray>);
    QPointer<DesktopClient> client_;
    std::optional<ResourceVersion> context_;
    std::shared_ptr<Fetch> active_;
    std::uint64_t generation_{};
};
} // namespace qcae
