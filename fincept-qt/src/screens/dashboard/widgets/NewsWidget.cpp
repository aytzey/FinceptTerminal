#include "screens/dashboard/widgets/NewsWidget.h"

#include "ui/theme/Theme.h"

#include <QLabel>
#include <QPointer>

#include <memory>

namespace fincept::screens::widgets {

NewsWidget::NewsWidget(QWidget* parent) : BaseWidget("MARKET NEWS", parent, ui::colors::CYAN) {
    scroll_area_ = new QScrollArea;
    scroll_area_->setWidgetResizable(true);

    auto* container = new QWidget(this);
    news_layout_ = new QVBoxLayout(container);
    news_layout_->setContentsMargins(4, 4, 4, 4);
    news_layout_->setSpacing(0);
    news_layout_->addStretch();

    scroll_area_->setWidget(container);
    content_layout()->addWidget(scroll_area_);

    connect(this, &BaseWidget::refresh_requested, this, &NewsWidget::refresh_data);

    apply_styles();
    set_loading(true);
    refresh_data();
}

void NewsWidget::apply_styles() {
    scroll_area_->setStyleSheet(QString("QScrollArea { border: none; background: transparent; }"
                                        "QScrollBar:vertical { width: 6px; background: transparent; }"
                                        "QScrollBar::handle:vertical { background: %1; border-radius: 3px; }"
                                        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
                                    .arg(ui::colors::BORDER_MED()));
}

void NewsWidget::on_theme_changed() {
    apply_styles();
    if (!last_articles_.isEmpty())
        populate(last_articles_);
}

void NewsWidget::refresh_data() {
    set_loading(true);

    QPointer<NewsWidget> self = this;
    auto* svc = &services::NewsService::instance();
    auto partial_conn = std::make_shared<QMetaObject::Connection>();
    *partial_conn = connect(svc, &services::NewsService::articles_partial, this,
                            [self, partial_conn](QVector<services::NewsArticle> articles, int, int) {
                                if (!self)
                                    return;
                                if (articles.isEmpty())
                                    return;
                                if (articles.size() > 12)
                                    articles = articles.mid(0, 12);
                                self->set_loading(false);
                                self->populate(articles);
                                QObject::disconnect(*partial_conn);
                            });

    svc->fetch_all_news_progressive(false, [self, partial_conn](bool ok, QVector<services::NewsArticle> articles) {
        QObject::disconnect(*partial_conn);
        if (!self)
            return;
        if (articles.size() > 12)
            articles = articles.mid(0, 12);
        self->set_loading(false);
        if (!ok || articles.isEmpty()) {
            if (self->news_layout_->count() <= 1) {
                self->set_error("No news available.");
            }
            return;
        }
        self->populate(articles);
    });
}

void NewsWidget::populate(const QVector<services::NewsArticle>& articles) {
    last_articles_ = articles;

    // Clear old items (keep the stretch)
    while (news_layout_->count() > 1) {
        auto* item = news_layout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    for (const auto& article : articles) {
        if (article.headline.isEmpty())
            continue;

        QString time_str = services::relative_time(article.sort_ts);
        if (time_str.isEmpty())
            time_str = article.time;

        auto* row = new QWidget(this);
        row->setStyleSheet(QString("border-bottom: 1px solid %1;").arg(ui::colors::BORDER_DIM()));
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(4, 4, 4, 4);
        rl->setSpacing(8);

        if (!time_str.isEmpty()) {
            auto* time_lbl = new QLabel(time_str);
            time_lbl->setFixedWidth(36);
            time_lbl->setStyleSheet(
                QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::CYAN()));
            rl->addWidget(time_lbl);
        }

        auto* headline = new QLabel(article.headline);
        headline->setWordWrap(true);
        headline->setStyleSheet(
            QString("color: %1; font-size: 11px; background: transparent;").arg(ui::colors::TEXT_PRIMARY()));
        rl->addWidget(headline, 1);

        if (!article.source.isEmpty()) {
            auto* src = new QLabel(article.source);
            src->setStyleSheet(
                QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::TEXT_TERTIARY()));
            rl->addWidget(src);
        }

        // Insert before the stretch
        news_layout_->insertWidget(news_layout_->count() - 1, row);
    }
}

} // namespace fincept::screens::widgets
