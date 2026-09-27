#pragma once
#include "qcae/query.hpp"
#include "qcae/record_application.hpp"
#include "qcae/render_projector.hpp"
#include "qcae/resource_store.hpp"
#include <QJsonObject>
#include <map>

namespace qcae::ipc {
// Disposable display projections. The application and view service remain authoritative.
class RenderService {
  public:
    RenderService(RecordApplication&, SelectionService&, ResourceStore&);
    bool supports(const QString&) const;
    QJsonObject dispatch(const QJsonObject&, const Caller&);

  private:
    RecordApplication& app_;
    SelectionService& selections_;
    ResourceStore& resources_;
    std::map<std::string, RenderProjector> projectors_;
    std::optional<DocumentRef> document_;
};
} // namespace qcae::ipc
