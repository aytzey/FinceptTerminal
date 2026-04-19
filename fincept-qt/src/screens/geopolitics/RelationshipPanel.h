// src/screens/geopolitics/RelationshipPanel.h
#pragma once
#include "services/geopolitics/GeopoliticsTypes.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

namespace fincept::screens {

/// Local event relationship visualization of countries, event categories, and sources.
class RelationshipPanel : public QWidget {
    Q_OBJECT
  public:
    explicit RelationshipPanel(QWidget* parent = nullptr);

  private:
    void build_ui();
    void build_network_view();
    void set_events(const QVector<fincept::services::geo::NewsEvent>& events);
    QWidget* build_node_card(const fincept::services::geo::RelationshipNode& node, QWidget* parent);

    QVBoxLayout* network_layout_ = nullptr;
    QLabel* stats_label_ = nullptr;
    QVector<fincept::services::geo::RelationshipNode> nodes_;
};

} // namespace fincept::screens
