#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/news/NewsService.h"

#include <QScrollArea>

namespace fincept::screens::widgets {

/// Market news widget — fetches real news via yfinance for major tickers.
class NewsWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit NewsWidget(QWidget* parent = nullptr);

  protected:
    void on_theme_changed() override;

  private:
    void apply_styles();
    void refresh_data();
    void populate(const QVector<services::NewsArticle>& articles);

    QScrollArea* scroll_area_ = nullptr;
    QVBoxLayout* news_layout_ = nullptr;
    QVector<services::NewsArticle> last_articles_; // cached for theme-change re-populate
};

} // namespace fincept::screens::widgets
