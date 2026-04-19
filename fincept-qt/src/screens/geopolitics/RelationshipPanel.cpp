// src/screens/geopolitics/RelationshipPanel.cpp
#include "screens/geopolitics/RelationshipPanel.h"

#include "services/geopolitics/GeopoliticsService.h"
#include "ui/theme/Theme.h"

#include <QHash>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace fincept::screens {

using namespace fincept::services::geo;

namespace {
QString node_key(const QString& prefix, const QString& value) {
    return prefix + ":" + value.trimmed().toLower();
}

QString source_name(const NewsEvent& ev) {
    if (!ev.domain.trimmed().isEmpty())
        return ev.domain.trimmed();
    const QUrl url(ev.url);
    return url.host().isEmpty() ? QStringLiteral("Unknown") : url.host();
}

QStringList sorted_set_values(const QSet<QString>& set, int limit = 6) {
    QStringList values = set.values();
    values.removeAll({});
    std::sort(values.begin(), values.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    if (limit > 0 && values.size() > limit)
        values = values.mid(0, limit);
    return values;
}

QColor node_color(const RelationshipNode& node) {
    if (node.type == QStringLiteral("category"))
        return category_color(node.id.section(':', 1));
    if (node.type == QStringLiteral("country"))
        return QColor(ui::colors::WARNING());
    if (node.type == QStringLiteral("source"))
        return QColor(ui::colors::INFO());
    return QColor(ui::colors::TEXT_SECONDARY());
}

void sort_nodes(QVector<RelationshipNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(), [](const RelationshipNode& a, const RelationshipNode& b) {
        if (a.dataset_count != b.dataset_count)
            return a.dataset_count > b.dataset_count;
        return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });
}

QVector<RelationshipNode> build_nodes_from_events(const QVector<NewsEvent>& events) {
    QHash<QString, int> country_counts;
    QHash<QString, int> category_counts;
    QHash<QString, int> source_counts;

    QHash<QString, QSet<QString>> country_categories;
    QHash<QString, QSet<QString>> country_sources;
    QHash<QString, QSet<QString>> category_countries;
    QHash<QString, QSet<QString>> source_countries;

    for (const auto& ev : events) {
        const QString country = ev.country.trimmed();
        const QString category = ev.event_category.trimmed().isEmpty() ? QStringLiteral("unclassified")
                                                                       : ev.event_category.trimmed();
        const QString source = source_name(ev);

        if (!country.isEmpty())
            country_counts[country] += 1;
        if (!category.isEmpty())
            category_counts[category] += 1;
        if (!source.isEmpty())
            source_counts[source] += 1;

        if (!country.isEmpty() && !category.isEmpty()) {
            country_categories[country].insert(category);
            category_countries[category].insert(country);
        }
        if (!country.isEmpty() && !source.isEmpty()) {
            country_sources[country].insert(source);
            source_countries[source].insert(country);
        }
    }

    QVector<RelationshipNode> countries;
    for (auto it = country_counts.cbegin(); it != country_counts.cend(); ++it) {
        QStringList connections = sorted_set_values(country_categories.value(it.key()), 4);
        connections.append(sorted_set_values(country_sources.value(it.key()), 2));
        countries.append({node_key(QStringLiteral("country"), it.key()), it.key(), QStringLiteral("country"), {},
                          it.value(), connections});
    }
    sort_nodes(countries);
    if (countries.size() > 15)
        countries.resize(15);

    QVector<RelationshipNode> categories;
    for (auto it = category_counts.cbegin(); it != category_counts.cend(); ++it) {
        categories.append({node_key(QStringLiteral("category"), it.key()), it.key(), QStringLiteral("category"), {},
                           it.value(), sorted_set_values(category_countries.value(it.key()), 6)});
    }
    sort_nodes(categories);

    QVector<RelationshipNode> sources;
    for (auto it = source_counts.cbegin(); it != source_counts.cend(); ++it) {
        sources.append({node_key(QStringLiteral("source"), it.key()), it.key(), QStringLiteral("source"), {},
                        it.value(), sorted_set_values(source_countries.value(it.key()), 6)});
    }
    sort_nodes(sources);
    if (sources.size() > 12)
        sources.resize(12);

    QVector<RelationshipNode> nodes;
    nodes.reserve(countries.size() + categories.size() + sources.size());
    nodes += countries;
    nodes += categories;
    nodes += sources;
    return nodes;
}
} // namespace

RelationshipPanel::RelationshipPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
    connect(&GeopoliticsService::instance(), &GeopoliticsService::events_loaded, this,
            [this](QVector<NewsEvent> events, int) { set_events(events); });
}

void RelationshipPanel::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header
    auto* header = new QWidget(this);
    header->setFixedHeight(48);
    header->setStyleSheet(
        QString("background:%1; border-bottom:1px solid %2;").arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    auto* hhl = new QHBoxLayout(header);
    hhl->setContentsMargins(16, 0, 16, 0);
    hhl->setSpacing(12);

    auto* title = new QLabel("GEOPOLITICAL RELATIONSHIP NETWORK", header);
    title->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3; letter-spacing:1px;")
                             .arg(ui::colors::INFO())
                             .arg(ui::fonts::TINY)
                             .arg(ui::fonts::DATA_FAMILY()));
    hhl->addWidget(title);
    hhl->addStretch();

    stats_label_ = new QLabel("WAITING FOR LOCAL EVENTS", header);
    stats_label_->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3; padding:2px 8px;"
                                        "background:rgba(255,255,255,0.04); border:1px solid %4;")
                                    .arg(ui::colors::TEXT_TERTIARY())
                                    .arg(ui::fonts::TINY)
                                    .arg(ui::fonts::DATA_FAMILY)
                                    .arg(ui::colors::BORDER_DIM()));
    hhl->addWidget(stats_label_);
    root->addWidget(header);

    // Network view
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(QString("QScrollArea { border:none; background:%1; }"
                                  "QScrollBar:vertical { background:%1; width:6px; }"
                                  "QScrollBar::handle:vertical { background:%2; border-radius:3px; }"
                                  "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }")
                              .arg(ui::colors::BG_BASE(), ui::colors::BORDER_MED()));

    auto* content = new QWidget(scroll);
    content->setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    auto* cvl = new QVBoxLayout(content);
    cvl->setContentsMargins(16, 16, 16, 16);
    cvl->setSpacing(16);
    network_layout_ = cvl;

    build_network_view();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
}

void RelationshipPanel::set_events(const QVector<NewsEvent>& events) {
    nodes_ = build_nodes_from_events(events);

    int countries = 0;
    int categories = 0;
    int sources = 0;
    for (const auto& node : nodes_) {
        if (node.type == QStringLiteral("country"))
            ++countries;
        else if (node.type == QStringLiteral("category"))
            ++categories;
        else if (node.type == QStringLiteral("source"))
            ++sources;
    }

    if (stats_label_) {
        stats_label_->setText(QString("NODES: %1  |  COUNTRIES: %2  |  CATEGORIES: %3  |  SOURCES: %4")
                                  .arg(nodes_.size())
                                  .arg(countries)
                                  .arg(categories)
                                  .arg(sources));
    }

    build_network_view();
}

void RelationshipPanel::build_network_view() {
    if (!network_layout_)
        return;

    while (network_layout_->count() > 0) {
        auto* item = network_layout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    if (nodes_.isEmpty()) {
        auto* empty = new QLabel("Run the monitor to build a local country-category-source network.", this);
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(180);
        empty->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3;")
                                 .arg(ui::colors::TEXT_TERTIARY())
                                 .arg(ui::fonts::SMALL)
                                 .arg(ui::fonts::DATA_FAMILY));
        network_layout_->addWidget(empty);
        network_layout_->addStretch();
        return;
    }

    auto add_section = [this](const QString& title, const QString& color, const QString& type) {
        int count = 0;
        for (const auto& n : nodes_) {
            if (n.type == type)
                ++count;
        }
        if (count == 0)
            return;

        auto* sec_lbl = new QLabel(title, this);
        sec_lbl->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;"
                                       "letter-spacing:2px; padding-bottom:4px; border-bottom:1px solid rgba(%4,0.3);")
                                   .arg(color)
                                   .arg(ui::fonts::SMALL)
                                   .arg(ui::fonts::DATA_FAMILY)
                                   .arg(color.mid(1)));
        network_layout_->addWidget(sec_lbl);

        auto* grid_w = new QWidget(this);
        auto* gl = new QGridLayout(grid_w);
        gl->setContentsMargins(0, 0, 0, 0);
        gl->setSpacing(10);
        gl->setColumnStretch(0, 1);
        gl->setColumnStretch(1, 1);
        gl->setColumnStretch(2, 1);

        int col = 0, row = 0;
        for (const auto& n : nodes_) {
            if (n.type != type)
                continue;
            gl->addWidget(build_node_card(n, grid_w), row, col);
            if (++col >= 3) {
                col = 0;
                row++;
            }
        }
        while (col > 0 && col < 3) {
            auto* spacer = new QWidget(grid_w);
            spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            gl->addWidget(spacer, row, col++);
        }

        network_layout_->addWidget(grid_w);
    };

    add_section("COUNTRIES", ui::colors::WARNING(), "country");
    add_section("EVENT CATEGORIES", ui::colors::NEGATIVE(), "category");
    add_section("SOURCES", ui::colors::INFO(), "source");
    network_layout_->addStretch();
}

QWidget* RelationshipPanel::build_node_card(const RelationshipNode& node, QWidget* parent) {
    auto color = node_color(node);
    const QString col_hex = color.name();
    const QString col_rgb = QString("%1,%2,%3").arg(color.red()).arg(color.green()).arg(color.blue());

    auto* card = new QWidget(parent);
    card->setObjectName("nodeCard");
    card->setMinimumHeight(110);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    card->setStyleSheet(QString("#nodeCard { background:%1; border:1px solid rgba(%2,0.25);"
                                "border-left:3px solid %3; }")
                            .arg(ui::colors::BG_RAISED())
                            .arg(col_rgb)
                            .arg(col_hex));

    auto* vl = new QVBoxLayout(card);
    vl->setContentsMargins(12, 10, 12, 10);
    vl->setSpacing(4);

    // Name row
    auto* name_row = new QWidget(card);
    auto* name_hl = new QHBoxLayout(name_row);
    name_hl->setContentsMargins(0, 0, 0, 0);
    name_hl->setSpacing(6);

    auto* name = new QLabel(node.label.toUpper(), name_row);
    name->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;")
                            .arg(col_hex)
                            .arg(ui::fonts::SMALL)
                            .arg(ui::fonts::DATA_FAMILY));
    name_hl->addWidget(name, 1);

    const QString badge_text = node.type.toUpper();
    auto* badge = new QLabel(badge_text, name_row);
    badge->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;"
                                 "padding:2px 6px; background:rgba(%4,0.12); border:1px solid rgba(%4,0.3);"
                                 "border-radius:8px; letter-spacing:1px;")
                             .arg(col_hex)
                             .arg(ui::fonts::TINY)
                             .arg(ui::fonts::DATA_FAMILY)
                             .arg(col_rgb));
    name_hl->addWidget(badge);
    vl->addWidget(name_row);

    // Type label
    auto* type_lbl = new QLabel(node.type.toUpper(), card);
    type_lbl->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3;")
                                .arg(ui::colors::TEXT_TERTIARY())
                                .arg(ui::fonts::TINY)
                                .arg(ui::fonts::DATA_FAMILY));
    vl->addWidget(type_lbl);

    // Dataset count row
    auto* ds_row = new QWidget(card);
    auto* ds_hl = new QHBoxLayout(ds_row);
    ds_hl->setContentsMargins(0, 0, 0, 0);
    ds_hl->setSpacing(4);

    auto* ds_num = new QLabel(QString::number(node.dataset_count), ds_row);
    ds_num->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;")
                              .arg(ui::colors::TEXT_PRIMARY())
                              .arg(ui::fonts::SMALL)
                              .arg(ui::fonts::DATA_FAMILY));
    ds_hl->addWidget(ds_num);

    auto* ds_lbl = new QLabel("events", ds_row);
    ds_lbl->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3;")
                              .arg(ui::colors::TEXT_TERTIARY())
                              .arg(ui::fonts::SMALL)
                              .arg(ui::fonts::DATA_FAMILY));
    ds_hl->addWidget(ds_lbl);
    ds_hl->addStretch();
    vl->addWidget(ds_row);

    // Connection pills (max 3)
    if (!node.connections.isEmpty()) {
        auto* pills_row = new QWidget(card);
        auto* pills_hl = new QHBoxLayout(pills_row);
        pills_hl->setContentsMargins(0, 2, 0, 0);
        pills_hl->setSpacing(4);

        const int max_pills = 3;
        int shown = qMin(node.connections.size(), max_pills);
        for (int i = 0; i < shown; ++i) {
            auto* pill = new QLabel(node.connections[i], pills_row);
            pill->setStyleSheet(QString("color:%1; font-size:%2px; padding:1px 6px;"
                                        "background:rgba(255,255,255,0.05); border:1px solid %3;"
                                        "border-radius:8px; font-family:%4;")
                                    .arg(ui::colors::TEXT_SECONDARY())
                                    .arg(ui::fonts::TINY)
                                    .arg(ui::colors::BORDER_DIM())
                                    .arg(ui::fonts::DATA_FAMILY));
            pills_hl->addWidget(pill);
        }

        if (node.connections.size() > max_pills) {
            auto* more = new QLabel(QString("+%1").arg(node.connections.size() - max_pills), pills_row);
            more->setStyleSheet(QString("color:%1; font-size:%2px; padding:1px 6px;"
                                        "background:rgba(255,255,255,0.05); border:1px solid %3;"
                                        "border-radius:8px; font-family:%4;")
                                    .arg(ui::colors::TEXT_TERTIARY())
                                    .arg(ui::fonts::TINY)
                                    .arg(ui::colors::BORDER_DIM())
                                    .arg(ui::fonts::DATA_FAMILY));
            pills_hl->addWidget(more);
        }

        pills_hl->addStretch();
        vl->addWidget(pills_row);
    }

    return card;
}

} // namespace fincept::screens
